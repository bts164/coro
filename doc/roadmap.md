# Roadmap

Planned and designed work not yet implemented, in rough priority order.

## Remove `Synchronize`

`JoinSet` is complete and tests pass. `Synchronize` can now be deleted:

- Remove `include/coro/sync/synchronize.h`
- Remove `test/test_synchronize.cpp` and its `CMakeLists.txt` entry
- Remove all `Synchronize` references from docs (`task_and_executor.md`,
  `coroutine_scope.md`, `module_structure.md`, `getting_started.md`)
- Remove `Runtime::schedule_task()` if it was only used by `Synchronize`

## Document: C++ vs. Rust cancellation model

Write a design document (`doc/cancellation_model.md`) explaining why C++ coroutines cannot
be cancelled by simply dropping the task, and what consequences this has throughout the
library's design. This is one of the most fundamental differences from Tokio and should be
documented prominently. Key points to cover:

- **Why Rust can drop:** a Rust `Future` is a plain value; dropping it at any `await`
  point is safe because the compiler synthesizes `Drop` for all locals in scope and the
  borrow checker guarantees no dangling references. Cancellation in Tokio is literally
  "stop holding the `JoinHandle`" — the task's memory is freed immediately.
- **Why C++ cannot:** a C++ coroutine frame is heap-allocated by the compiler. The only
  way to run destructors for locals and release resources is to *resume* the coroutine and
  let it unwind. Dropping the `shared_ptr<Task>` mid-suspension destroys the frame
  without running any destructors — a resource leak or, if child tasks hold references to
  parent-owned data, a use-after-free.
- **The `PollDropped` contract:** cancellation is delivered as a poll signal
  (`PollDropped`), not an exception. The coroutine must resume, observe the signal,
  propagate it to any child tasks it has spawned (waiting for their `PollDropped` before
  continuing), and then return its own `PollDropped`. Only after this full drain is the
  task's memory safe to free.
- **Impact on executor design:** a cancelled task in `Idle` state must be re-enqueued
  (via `waker->wake()` after setting `cancelled`) so it is polled through the `PollDropped`
  path. The executor cannot simply discard it.
- **Impact on `CoroutineScope` / `JoinSet`:** these primitives exist partly *because* of
  this constraint — they provide a structured way to ensure all child tasks are drained
  before the parent scope exits, which is mandatory rather than optional.
- **Impact on `select` / `timeout`:** the losing branch of a `select` or an expired
  `timeout` must be cancelled and fully drained before the result is delivered to the
  caller. This adds latency that Tokio does not have.
- **User-facing implication:** spawned tasks that capture references to parent-owned data
  are safe *only* within a scope that guarantees the parent outlives all children
  (`CoroutineScope`, `JoinSet`). This is the C++ analogue of Rust's `'static` bound on
  `tokio::spawn`.

## Cancellation propagation tests

The end-to-end cancellation tests deferred in `test/test_coro_scope.cpp` can now be written — `select` and `timeout` are both implemented. Specifically:

- A cancelled coroutine that has spawned children via `spawn()` must drain those children (waiting for their `PollDropped`) before returning its own `PollDropped`.
- Children receive cancellation via `select`/`timeout` combinators (e.g. a branch that loses a select is cancelled and must drain before the select result is delivered).

These tests require no new infrastructure; they are Phase 3 validation for the interaction between `CoroutineScope`, `SelectFuture`, and `Coro<T>`.

## ~~Work-sharing executor~~ — complete

`WorkSharingExecutor` is implemented and all tests pass. `Runtime` now selects the executor
at construction time: `num_threads <= 1` → `SingleThreadedExecutor`, otherwise
`WorkSharingExecutor`. Design documented in `doc/executor_design.md`.

## Executor local queue and injection queue

Design documented in `doc/executor_design.md`. Fixes a correctness bug in
`SingleThreadedExecutor` and lays the groundwork for a work-stealing executor.

### `SingleThreadedExecutor` (correctness fix)

The timer service background thread calling `wake_task` races with the poll loop:
- `wake_task` accesses `m_ready`, `m_suspended`, and the self-wake flag without a mutex.
- `wait_for_completion` exits early when the ready queue is empty, even if tasks are
  suspended awaiting timer or I/O wakeups.

Fix: split `wake_task` into a local path (same thread — existing code, no lock) and a
remote path (different thread — mutex-protected `m_incoming_wakes` injection queue +
`m_remote_cv`). `wait_for_completion` blocks on `m_remote_cv` instead of returning early.

### `WorkSharingExecutor` (performance optimization)

External wakes are already safe (all paths hold `m_mutex`). Adding per-worker local queues
reduces contention: a worker re-enqueuing a just-polled task goes to its local queue with
no lock; only cross-thread wakes use the shared injection queue. This is also the direct
precursor to the work-stealing executor.

## Channels — `mpsc`, `oneshot`, `watch`, & `broadcast`

User-facing async channels for inter-task communication. The internal `Channel<T>` backing
`StreamHandle` already provides the core machinery; this item exposes it as a public API
and adds an `oneshot` variant.

### `mpsc` — multi-producer, single-consumer

```cpp
auto [tx, rx] = coro::mpsc::channel<int>(/*buffer=*/16);
// Producer side (Future<void>):
co_await tx.send(42);
// Consumer side (Stream<int>):
while (auto item = co_await coro::next(rx))
    use(*item);
```

- `Sender<T>` — `Future<void>` that resolves once the item is buffered or the receiver
  is ready. Multiple `Sender`s can be created by cloning (`tx.clone()`).
- `Receiver<T>` — satisfies `Stream<T>`; yields items in send order; signals exhaustion
  when all senders are dropped.
- Bounded buffer with backpressure: `send()` suspends when full.

### `oneshot` — single-value, single-producer, single-consumer

```cpp
auto [tx, rx] = coro::oneshot::channel<int>();
coro::spawn(co_invoke([tx = std::move(tx)]() mutable -> coro::Coro<void> {
    tx.send(99);  // synchronous — oneshot send never blocks
})).submit().detach();
int value = co_await rx;  // rx satisfies Future<T>
```

- `OneshotSender<T>` — synchronous `send(T)`; dropping without sending causes `rx` to
  receive `PollError`.
- `OneshotReceiver<T>` — satisfies `Future<T>`; resolves when the sender fires or errors
  if the sender is dropped.

### `watch` — single-producer, multi-consumer, latest-value

A channel that holds exactly one value. Any number of receivers can observe the current
value or wait for the next change. New sends overwrite the previous value — there is no
queue and receivers never apply backpressure.

```cpp
auto [tx, rx] = coro::watch::channel<int>(/*initial=*/0);

// Sender (any thread / task):
tx.send(42);  // synchronous overwrite; never blocks

// Receiver — await the next change, then read the current value:
co_await rx.changed();  // suspends until a new value is sent
int current = rx.borrow();

// Clone to give independent cursors to multiple tasks:
auto rx2 = rx.clone();
```

- `WatchSender<T>` — synchronous `send(T)` (no suspension); last write wins.
- `WatchReceiver<T>` — `changed()` returns `Future<void>` that resolves when a new value
  has been sent since the last `borrow()` or `changed()` call. `borrow()` returns a
  const reference to the current value under a read lock. Cloneable for fan-out.
- Use case: configuration/state that many tasks need to observe, where only the latest
  value matters (e.g. feature flags, connection state, metrics thresholds).

### `broadcast` — single-producer, multi-consumer, all-values

**Lower priority that the other three, and may be deferred**

A channel where every active receiver sees every message. Unlike `watch`, no values are
dropped as long as the slowest receiver keeps up; a receiver that falls too far behind
receives a lag error indicating missed messages.

```cpp
auto [tx, rx] = coro::broadcast::channel<int>(/*capacity=*/64);

// Clone rx to give each subscriber an independent cursor:
auto rx2 = rx.clone();

// Producer:
co_await tx.send(1);
co_await tx.send(2);

// Each receiver sees all messages sent after its creation:
while (auto item = co_await coro::next(rx))
    use(*item);
```

- `BroadcastSender<T>` — `send(T)` returns `Future<void>`; suspends if all receiver
  buffers are full (slowest receiver applies backpressure).
- `BroadcastReceiver<T>` — satisfies `Stream<T>`; yields messages in send order.
  If a receiver falls behind by more than `capacity` messages it receives a lag error
  on the next `next()` call. Cloneable.
- Use case: event buses, log fanout, pub/sub within a single runtime.

Lives in `include/coro/sync/channel.h`.

## Async `Mutex<T>`

`std::mutex` blocks the OS thread, starving the executor. `Mutex<T>` suspends the *task*
until the lock is free, yielding the thread back to the executor in the meantime.

```cpp
coro::Mutex<std::vector<int>> shared;

coro::Coro<void> append(int value) {
    auto guard = co_await shared.lock();  // suspends if contended; never blocks thread
    guard->push_back(value);
}   // guard released here
```

- `Mutex<T>` owns `T` and exposes it only through the guard, enforcing lock discipline.
- `lock()` returns `Future<MutexGuard<T>>`; the guard releases on destruction.
- Fair queuing: waiting tasks are woken in FIFO order.

Lives in `include/coro/sync/mutex.h`.

## libuv integration

Currently `SleepFuture` is clock-polling only: the executor is not woken at the deadline; it only fires when the executor happens to re-poll. This needs:

- **Timer waker** in `SleepFuture::poll()`: register a one-shot libuv timer that calls `waker->wake()` when the deadline expires, replacing busy-polling.
- **Event loop in `block_on`**: drive `uv_run(loop, UV_RUN_NOWAIT)` alongside `poll_ready_tasks()` so I/O and timer callbacks are processed.
- **Thread pool startup** in `Runtime`: create libuv loop and worker threads; shut them down in the destructor.

The waker/context design already supports this — leaf futures call `ctx.getWaker()` and store it for the callback to fire.

## Work-stealing executor

A multi-threaded work-stealing executor to replace (or sit alongside) `SingleThreadedExecutor`. Modelled on Tokio's scheduler:

- Per-thread run queues with work-stealing on underload.
- The `Executor` interface is already designed to be pluggable; `Runtime` can select the implementation at construction time (e.g. `num_threads == 1` → single-threaded, otherwise work-stealing).
- Thread-local `t_current_coro` and the `CoroutineScope` mutex are already safe for multi-threaded use.
- Natural evolution from the work-sharing executor — the shared queue becomes per-thread queues with a steal path added.

## ~~Fate of `Synchronize`~~ — resolved: remove after `JoinSet` lands

Once `JoinSet` (item 6) is implemented and its tests pass, `Synchronize` will be removed.

**Why `Synchronize` becomes redundant:**

- `co_invoke` already handles the lambda object lifetime problem (`Synchronize`'s primary
  safety rationale).
- `JoinSet::drain()` provides an explicit mid-coroutine drain point with the same guarantee.
- `JoinSet` is strictly more capable — it can collect results, cancel early on error, and
  compose with `select`. `Synchronize` can only drain-and-discard.

Maintaining two APIs for the same pattern creates unnecessary "which one do I use?" friction
for new users. `Synchronize` will not be deprecated first — since the library is pre-1.0,
it will simply be deleted once `JoinSet` covers all its use cases.

**Migration path** (`Synchronize` → `co_invoke` + `JoinSet`):

```cpp
// Before
co_await Synchronize([&](Synchronize& sync) -> Coro<void> {
    sync.spawn(worker(local_data));
});

// After
co_await co_invoke([&]() -> Coro<void> {
    JoinSet<void> js;
    js.spawn(worker(local_data));
    co_await js.drain();
});
```

## Stream combinators

The `Stream` concept and `next()` are implemented but there are no combinators. Without
them, every stream consumer must write a boilerplate `while (auto item = co_await next(s))`
loop. Modelled on Rust's `StreamExt` / `Iterator`:

- **`map(stream, fn)`** — transform each item: `Stream<T>` → `Stream<U>`
- **`filter(stream, pred)`** — drop items that don't satisfy a predicate
- **`take(stream, n)`** — yield at most `n` items then signal exhaustion
- **`chain(s1, s2)`** — concatenate two streams of the same item type
- **`flat_map(stream, fn)`** — map each item to a stream and flatten one level

All combinators are lazy (zero-cost wrappers satisfying `Stream`) and live in
`include/coro/stream/` (new subdirectory). Each has a corresponding free function
returning a `[[nodiscard]]` adaptor type.

## `AbortHandle`

Currently the only way to cancel a spawned task is to drop its `JoinHandle`, which also
gives up the ability to observe the result. `AbortHandle` decouples cancellation from
result collection:

```cpp
auto [handle, abort] = coro::spawn(task()).submit_with_abort();
abort.abort();          // request cancellation
co_await handle;        // still await completion (receives PollDropped / exception)
```

- `AbortHandle` — a lightweight cancellation token tied to one task; `abort()` marks the
  underlying `TaskState::cancelled`.
- `JoinHandle` — unchanged; still the only way to await the result.
- `SpawnBuilder::submit_with_abort()` returns `pair<JoinHandle<T>, AbortHandle>`.

Lives in `include/coro/task/abort_handle.h`.

## libuv I/O primitives

Dependent on item 2 (libuv integration). Once the event loop is wired into `block_on`,
expose async I/O wrappers in `include/coro/io/`:

- **`TcpListener` / `TcpStream`** — accept connections, read/write with backpressure.
- **`UdpSocket`** — async send/recv.
- **`File`** — async read/write using libuv's thread-pool file I/O.
- **DNS resolution** — `resolve(hostname)` returning `Future<IpAddress>`.

Each wraps the corresponding libuv handle, storing a `Waker` in the callback and waking
the task when the operation completes.

## `spawn_blocking()`

Run a blocking closure on a dedicated thread pool without stalling the async executor.
Essential for CPU-intensive work or unavoidable blocking I/O.

```cpp
int result = co_await coro::spawn_blocking([]() -> int {
    return expensive_computation();  // runs on thread pool, not executor thread
});
```

- Submits the closure to a `std::thread` pool (or libuv's thread pool once integrated).
- Returns a `Future<T>` that resolves when the thread completes.
- The calling task suspends (freeing the executor thread) until the blocking work finishes.

Lives in `include/coro/task/spawn_blocking.h`.

## libwebsockets integration

Dependent on the libuv integration item. Wraps [libwebsockets](https://libwebsockets.org)
using the libuv event loop as the backend, exposing async WebSocket client and server
primitives that compose naturally with the rest of the library.

```cpp
#include <coro/io/websocket.h>

// Client:
coro::Coro<void> run() {
    auto ws = co_await coro::WebSocket::connect("wss://example.com/feed");

    // Send a message:
    co_await ws.send("hello");

    // Receive messages as a stream:
    while (auto msg = co_await coro::next(ws))
        std::cout << msg->text() << "\n";
}

// Server:
coro::Coro<void> serve() {
    auto listener = coro::WebSocketListener::bind("0.0.0.0", 9000);
    while (auto conn = co_await coro::next(listener)) {
        coro::spawn(co_invoke([conn = std::move(*conn)]() mutable -> coro::Coro<void> {
            while (auto msg = co_await coro::next(conn))
                co_await conn.send(msg->text());  // echo
        })).submit().detach();
    }
}
```

- `WebSocket` — satisfies `Stream<Message>` for incoming frames; `send()` returns
  `Future<void>`. Backed by a libwebsockets context driven by the libuv event loop,
  so no dedicated thread is needed.
- `WebSocketListener` — satisfies `Stream<WebSocket>`; yields one `WebSocket` per
  accepted connection.
- `Message` — carries the frame payload and type (`text`, `binary`, `ping`, `close`).
- Fragmented frames are reassembled before delivery.
- TLS is handled by libwebsockets' built-in OpenSSL/mbedTLS integration.

Lives in `include/coro/io/websocket.h`. Requires libwebsockets built with libuv event
loop support (`-DLWS_WITH_LIBUV=ON`).

## Logging

The library currently has no structured logging. Executor internals, task lifecycle
events, and fatal invariant violations (e.g. unexpected CAS failures on `SchedulingState`)
need a way to emit diagnostics without coupling to a specific logging framework.

Design goals:

- **Zero-cost when disabled:** logging calls compile away entirely when the log level is
  below the threshold. No heap allocation, no format string evaluation.
- **Pluggable sink:** the library does not own stderr. Users wire in their own sink
  (spdlog, std::print, a custom callback) via a single registration point.
- **Structured levels:** at minimum `TRACE`, `DEBUG`, `INFO`, `WARN`, `ERROR`, `FATAL`.
  `FATAL` logs and then calls `std::terminate()` — used for executor invariant violations
  where continuing would produce undefined behaviour.
- **Task identity:** log messages emitted from within a task should optionally include a
  task ID so scheduler traces can be correlated across threads.

Suggested events to instrument once logging exists:

- Task created / scheduled / polled / completed / cancelled
- `SchedulingState` transitions (at `TRACE` level)
- CAS failures that indicate bugs (`FATAL`)
- `TimerService` wakeup firing
- Worker thread start / stop
- Injection queue drain counts (task budget enforcement)
