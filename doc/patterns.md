# Patterns

Recurring async programming patterns that appear frequently in real code but are not
always obvious to users new to the library. Each pattern is described with a brief
problem statement, the idiomatic solution, and a concise code example.

---

## Task lifetime

### Background periodic task

**Problem:** Run a task indefinitely on a fixed interval — polling a connection, flushing
a write buffer, emitting heartbeats — without blocking any thread.

```cpp
coro::Coro<void> connection_monitor(Connection& conn) {
    using namespace std::chrono_literals;
    for (;;) {
        co_await coro::sleep_for(5s);
        if (!co_await conn.ping())
            log_warning("connection unhealthy");
    }
}

// Keep the handle alive for as long as monitoring is needed.
// Dropping it cancels the task.
coro::JoinHandle<void> monitor = coro::spawn(connection_monitor(conn));
```

The `JoinHandle` acts as a lifetime token. When it goes out of scope the task is
cancelled and drained automatically — no explicit stop flag needed.

### Structured cancellation: shutting down a task tree

**Problem:** Stop a group of related tasks cleanly when work is done or an error occurs.

Dropping a `JoinHandle` cancels that task and, transitively, every task it has spawned
that has not yet been detached. Structure ownership so the root handle is held by the
scope that knows when to stop.

```cpp
coro::Coro<void> supervisor(coro::MpscReceiver<WorkItem> rx) {
    // Sub-tasks are owned by their handles; dropping them cancels the tasks.
    auto h_monitor = coro::spawn(connection_monitor(db));
    auto h_flush   = coro::spawn(periodic_flush(db));

    while (auto item = co_await coro::next(rx))
        co_await process(*item);

    // h_monitor and h_flush cancelled here as handles go out of scope.
}

// Dropping this handle cancels the supervisor and both sub-tasks.
coro::JoinHandle<void> root = coro::spawn(supervisor(std::move(rx)));
```

Prefer this over explicit shutdown flags whenever the task tree has a clear owner. The
`co_await` points inside each task are already cancellation checkpoints — no extra
plumbing is needed.

> **Note:** `.detach()` severs the ownership link — a detached task cannot be cancelled
> via its former handle. Only detach tasks that are genuinely fire-and-forget and whose
> errors can be safely discarded.

> **Planned:** A `CancellationSource` / `CancellationToken` primitive (analogous to
> Tokio's `CancellationToken`) is planned for cases where tasks are spawned
> independently and cannot easily share a common owner — for example, tasks registered
> from multiple call sites that all need to observe the same shutdown signal. Until that
> is available, restructure to a shared owner where possible rather than threading a
> manual signal through unrelated tasks.

---

## Communication

### Request-reply (oneshot inside mpsc)

**Problem:** A client needs a response to a specific request it sent to a shared worker.
A shared result variable and mutex works, but ties the response to a single caller.

Send a `OneshotSender` alongside the request. The worker replies through it directly;
each caller gets its own private reply channel.

```cpp
struct ComputeRequest {
    int                      input;
    coro::OneshotSender<int> reply;
};

coro::Coro<void> calculator(coro::MpscReceiver<ComputeRequest> rx) {
    while (auto req = co_await coro::next(rx)) {
        req->reply.send(req->input * 2);  // synchronous, never suspends
    }
}

coro::Coro<int> call(coro::MpscSender<ComputeRequest> tx, int input) {
    auto [reply_tx, reply_rx] = coro::oneshot_channel<int>();
    co_await tx.send(ComputeRequest{input, std::move(reply_tx)});
    co_return (co_await reply_rx).value();
}
```

Multiple callers can share the same `MpscSender` (clone it per caller). Each gets an
independent `OneshotReceiver` and never sees another caller's reply.

### Actor pattern

**Problem:** Several tasks need to share mutable state. Protecting it with a mutex
creates contention and risks priority inversion.

Give the state to a dedicated task (the *actor*) and expose it only through a typed
message channel. The actor is the only writer — no mutex needed anywhere in the calling
code.

```cpp
struct Increment {};
struct Reset     {};
struct GetCount  { coro::OneshotSender<int> reply; };
using Command = std::variant<Increment, Reset, GetCount>;

coro::Coro<void> counter_actor(coro::MpscReceiver<Command> rx) {
    int count = 0;
    while (auto cmd = co_await coro::next(rx)) {
        std::visit([&](auto& c) {
            using T = std::decay_t<decltype(c)>;
            if constexpr (std::is_same_v<T, Increment>)
                ++count;
            else if constexpr (std::is_same_v<T, Reset>)
                count = 0;
            else if constexpr (std::is_same_v<T, GetCount>)
                c.reply.send(count);
        }, *cmd);
    }
}
```

The `MpscSender<Command>` is the actor's public handle — pass clones to every caller.
The actor shuts down naturally when all senders are dropped.

**When the actor itself needs to do async work**

The above pattern works when command handling is synchronous. When handlers need to
`co_await` — for example, an actor that owns a database connection and must issue
queries — dispatch to named coroutine functions rather than handling inline, since
`co_await` cannot appear inside a `std::visit` lambda.

```cpp
coro::Coro<void> handle(DbConnection& conn, QueryCommand& cmd) {
    auto rows = co_await conn.query(cmd.sql);
    cmd.reply.send(std::move(rows));
}

coro::Coro<void> handle(DbConnection& conn, PingCommand& cmd) {
    cmd.reply.send(co_await conn.ping());
}

coro::Coro<void> db_actor(coro::MpscReceiver<DbCommand> rx) {
    auto conn = co_await DbConnection::connect("db.internal");
    while (auto cmd = co_await coro::next(rx)) {
        if (auto* q = std::get_if<QueryCommand>(&*cmd))
            co_await handle(conn, *q);
        else if (auto* p = std::get_if<PingCommand>(&*cmd))
            co_await handle(conn, *p);
    }
}
```

The actor still serializes all operations — callers queue behind the channel and the
connection is never accessed concurrently — but each operation is now itself async.
This is the idiomatic way to own a resource that requires async I/O without exposing
it to shared access.

### Streaming results back to a caller

**Problem:** An operation produces results incrementally over time rather than as a
single return value. Buffering them all before returning wastes memory and increases
latency.

Return a `CoroStream<T>`. The caller consumes with the same `co_await next()` pattern
used everywhere else; no channel pair or extra task spawn is needed.

```cpp
coro::CoroStream<SearchResult> search(std::string query) {
    for (auto& result : run_query(query))
        co_yield result;
}

coro::Coro<void> run_search(std::string query) {
    auto stream = search(std::move(query));
    while (auto result = co_await coro::next(stream))
        display(*result);
}
```

This is a *pull* model — the generator resumes only when the consumer calls `next()`,
so producer and consumer naturally stay in step without any explicit backpressure
mechanism.

**Choosing between `CoroStream` and `mpsc`**

The key axis is whether task lifetime management should be bundled with the data flow
or kept separate:

| | Single producer | Multiple producers |
|---|---|---|
| **Bundled task management** | `CoroStream` | *(planned — see below)* |
| **Separate task management** | `mpsc` (1 sender) | `mpsc` (N senders) |

Use `mpsc` when you need the flexibility of decoupled lifetimes: the producer outlives
one consumer, the producer's errors must be awaited independently, or the sender needs
to be passed somewhere that doesn't own the task.

The "multiple producers, bundled task management" cell — spawning N `CoroStream`
producers and consuming their merged output through a single stream handle — is planned
but not yet implemented. See the [roadmap](roadmap.md) for details. Until then, use
`mpsc` with N cloned senders for this case.

> **Concurrent buffering:** if producing the next item is expensive enough that you want
> it overlapping with consumption of the current one, a `buffered(N)` adapter on
> `CoroStream` is planned. Until then, an `mpsc` channel between a spawned producer task
> and the consumer provides the same behaviour manually.

---

## Concurrency

### Fan-out / scatter-gather (JoinSet)

**Problem:** Apply an async operation to a dynamic collection of items and process
results as they complete.

`JoinSet` manages the task group and delivers results in completion order, not
submission order.

```cpp
coro::Coro<void> fetch_all(std::vector<std::string> urls) {
    coro::JoinSet<Response> js;
    for (auto& url : urls)
        js.spawn(fetch(url));

    while (auto r = co_await coro::next(js))
        process(*r);
}
```

You do not need to know the number of tasks in advance. For a small, fixed number of
concurrent operations where you want all results together as a tuple, prefer `join()`:

```cpp
auto [user, orders, prefs] = co_await coro::join(
    fetch_user(id),
    fetch_orders(id),
    fetch_preferences(id)
);
```

### Pipeline with backpressure

**Problem:** Process data through multiple transformation stages concurrently. If one
stage is slow the others should not run ahead unchecked.

Chain tasks with bounded mpsc channels. Backpressure flows upstream automatically —
a slow stage fills its output channel, which suspends the upstream stage.

```cpp
coro::Coro<void> pipeline() {
    auto [raw_tx,    raw_rx]    = coro::mpsc_channel<RawItem>(32);
    auto [parsed_tx, parsed_rx] = coro::mpsc_channel<ParsedItem>(32);

    auto h_read  = coro::spawn(read_source(std::move(raw_tx)));
    auto h_parse = coro::spawn(parse_stage(std::move(raw_rx), std::move(parsed_tx)));

    while (auto item = co_await coro::next(parsed_rx))
        co_await write_sink(*item);

    co_await std::move(h_read);
    co_await std::move(h_parse);
}
```

Each stage runs concurrently as an independent task. Channel capacity (32 here) is the
buffer between stages — tune it to absorb burst variance while keeping memory bounded.

---

## Resilience

### Retry with exponential backoff

**Problem:** A fallible async operation should be retried on transient failures, but
not forever and not as fast as possible.

Loop with a doubling delay and cap both the per-attempt wait and the total elapsed time.
Apply the total timeout at the call site, not inside the retry loop.

```cpp
coro::Coro<Connection> connect_with_retry(std::string host, int port) {
    using namespace std::chrono_literals;
    auto delay = 100ms;
    for (int attempt = 0; ; ++attempt) {
        try {
            co_return co_await TcpStream::connect(host, port);
        } catch (const std::exception&) {
            if (attempt >= 4) throw;
        }
        co_await coro::sleep_for(delay);
        delay = std::min(delay * 2, 30s);
    }
}

// Enforce a total deadline at the call site:
auto result = co_await coro::timeout(60s, connect_with_retry("db.internal", 5432));
if (result.index() == 1)
    throw std::runtime_error("could not connect within 60s");
```

Keeping the timeout at the call site lets callers choose their own deadline without
the retry function needing to know about it.

### Timeout with fallback

**Problem:** An operation might time out, but a timeout is not an error — there is a
reasonable fallback (cached data, a default value, a degraded response).

`timeout()` returns a variant. Handle both branches explicitly:

```cpp
auto r = co_await coro::timeout(500ms, fetch_live_data());
if (r.index() == 0)
    present(std::get<0>(r).value);  // fresh data
else
    present(cached_data());         // timed out — serve stale
```

Avoid `.value()` here — that throws on timeout rather than falling back.

### Cancellable operation via select

**Problem:** An operation needs to be stoppable on demand, but it does not accept a
cancellation token and modifying it is impractical.

Wrap it in `select` alongside a cancellation signal. The losing branch is cancelled
automatically.

```cpp
coro::Coro<void> cancellable_upload(coro::WatchReceiver<bool> cancel,
                                    Payload                   data) {
    auto sel = co_await coro::select(upload(std::move(data)), cancel.changed());
    if (sel.index() == 0)
        log("upload complete");
    else
        log("upload cancelled");  // upload was cancelled mid-flight
}
```

No changes to `upload()` are needed. This pattern composes — nest `select` calls to
cancel based on multiple independent signals.

---

## Coordination

### One-time startup rendezvous

**Problem:** A spawned task must complete some initialisation before the parent
proceeds. Polling a shared flag or using a sleep is fragile.

Pass an `OneshotSender` to the child task. The parent suspends on the receiver until
the child calls `send()`.

```cpp
coro::Coro<void> server(coro::OneshotSender<bool> ready, int port) {
    auto listener = co_await coro::TcpListener::bind("0.0.0.0", port);
    ready.send(true);  // parent may now proceed
    while (auto conn = co_await coro::next(listener))
        coro::spawn(handle_conn(std::move(*conn))).detach();
}

coro::Coro<void> run() {
    auto [ready_tx, ready_rx] = coro::oneshot_channel<bool>();
    auto h = coro::spawn(server(std::move(ready_tx), 8080));
    (co_await ready_rx).value();  // suspend until server is listening
    co_await run_client_tests();
    co_await std::move(h);
}
```

If the server task fails before sending, the receiver sees `ChannelError::Closed` —
`.value()` turns that into an exception, surfacing the failure to the parent immediately.

### Runtime-reconfigurable config via watch

**Problem:** Workers need access to configuration that can change at runtime (e.g. log
level, timeout thresholds, feature flags). Restarting workers on every change is
expensive.

Distribute config through a watch channel. Workers snapshot the current config at the
start of each unit of work — no restart needed.

```cpp
struct Config { int timeout_ms; std::string log_level; };

coro::Coro<void> worker(coro::MpscReceiver<WorkItem>  items,
                        coro::WatchReceiver<Config>   cfg) {
    while (auto item = co_await coro::next(items)) {
        Config c = *cfg.borrow();  // snapshot — do not hold across co_await
        co_await process(*item, c);
    }
}

coro::Coro<void> config_reloader(coro::WatchSender<Config> cfg_tx) {
    using namespace std::chrono_literals;
    for (;;) {
        co_await coro::sleep_for(30s);
        cfg_tx.send(load_config_from_disk());
    }
}

// At startup:
auto [cfg_tx, cfg_rx] = coro::watch_channel<Config>(load_config_from_disk());
for (int i = 0; i < N_WORKERS; ++i)
    coro::spawn(worker(rx.clone(), cfg_rx.clone())).detach();
coro::spawn(config_reloader(std::move(cfg_tx))).detach();
```

`borrow()` takes a shared read lock and is non-blocking. Workers on different threads
can call it simultaneously without contention.

### Debounce: coalescing rapid updates

**Problem:** A watch channel may fire many times in rapid succession (file-system
events, slider movements, rapid config edits). Processing each notification individually
is wasteful.

After the first notification, wait a short window before reading the value. Any
intermediate updates are absorbed — the handler sees only the latest.

```cpp
coro::Coro<void> watch_and_reindex(coro::WatchReceiver<FileList> rx) {
    using namespace std::chrono_literals;
    for (;;) {
        co_await rx.changed();           // block until any change
        co_await coro::sleep_for(50ms);  // coalesce rapid successive writes
        auto files = rx.borrow_and_update();
        co_await reindex(*files);        // process the latest state once
    }
}
```

`borrow_and_update()` marks the current version as seen, so updates that arrived during
the 50ms window are consumed in a single `reindex` call rather than triggering N
separate calls.

---

## Integration

### Bridging synchronous and asynchronous code

**Problem:** An existing synchronous codebase needs to call async functions, or an
async task needs to call a blocking library that cannot be made async.

**Sync calling async — `block_on`:**

`block_on` is the entry point from synchronous code into the async world. It drives the
runtime until the given coroutine completes, then returns the result synchronously.

```cpp
int main() {
    coro::Runtime rt;
    // Runs the runtime on the calling thread until fetch_report() completes.
    Report r = rt.block_on(fetch_report("db.internal", 5432));
    std::cout << r.summary << "\n";
}
```

For applications that need a persistent runtime available across many sync-to-async
calls, run the runtime on a dedicated background thread and route requests to it via
thread-safe channels.

**Async calling blocking — `spawn_blocking`:**

Blocking operations (file parsing, compression, CPU-intensive work) must not run
directly on an executor thread — they stall the thread and starve every other task
sharing it. `spawn_blocking` offloads them to a dedicated thread pool and returns a
future the caller can `co_await`.

```cpp
coro::Coro<void> process_upload(std::vector<std::byte> compressed) {
    // Decompress on the blocking pool; executor thread is free during decompression.
    auto data = co_await coro::spawn_blocking([compressed = std::move(compressed)]() {
        return decompress(compressed);
    });
    co_await store(std::move(data));
}
```

The rule of thumb: any operation that could block a thread for more than a few
microseconds belongs in `spawn_blocking`.

---

## I/O

### Buffered protocol reading

**Problem:** A stream delivers arbitrary-sized chunks of bytes. The application works
in terms of complete protocol frames (length-prefixed messages, newline-terminated
lines, etc.). A single read may deliver a partial frame, multiple frames, or both.

The standard approach is a read loop with an accumulation buffer: try to parse a
complete frame from what is already buffered; only read more bytes when the buffer does
not yet contain a complete frame.

```cpp
// try_parse_frame() removes a complete frame from buf and returns it,
// or returns nullopt and leaves buf unchanged if the frame is incomplete.
std::optional<Frame> try_parse_frame(std::vector<std::byte>& buf);

coro::Coro<void> handle_connection(coro::TcpStream stream) {
    std::vector<std::byte> buf;
    for (;;) {
        while (auto frame = try_parse_frame(buf))
            co_await process_frame(*frame);

        auto [n, chunk] = co_await stream.read(std::vector<std::byte>(4096));
        if (n == 0) co_return;  // peer closed connection
        buf.insert(buf.end(), chunk.begin(), chunk.begin() + n);
    }
}
```

TCP is used here as a concrete example but the pattern applies to any byte stream:
serial ports, pipes, Unix domain sockets, shared-memory rings.

**High-throughput variant — `PollStream`**

The loop above suspends on `co_await stream.read(...)` and wakes through the executor
on every chunk, which adds scheduling overhead. For high data-rate streams where that
overhead is measurable, `PollStream` lets the decoder run directly on the I/O thread
without going through the task scheduler for each read.

The decoder passed to `PollStream` is a state machine — it receives available bytes,
returns as much as it parsed, and signals whether it needs more data. This maps
naturally to the same accumulate-and-try-parse structure above; `PollStream` just
drives it at a lower level to eliminate the per-read wakeup cost. See
[Poll Streams](poll_streams.md) for the full interface.

> **Future direction:** the decoder interface of `PollStream` is a state machine
> expressed as a class. A coroutine is a compiler-generated state machine, so there is
> a natural correspondence: a coroutine-based decoder that suspends when it needs more
> bytes and resumes when they arrive would express the same logic in sequential code
> rather than an explicit state machine. Making the decoder a coroutine that executes
> on the I/O thread — bypassing the executor scheduler entirely — is an open design
> question on the roadmap.
