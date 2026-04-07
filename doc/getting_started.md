# Getting Started

This guide walks through the core concepts of the library with working examples, from a
minimal async function to spawning parallel tasks, async generators, and timeouts.

## Setup

The library requires C++20 and uses [Conan](https://conan.io) for dependencies and CMake to build.

```bash
conan install . --output-folder=build --build=missing
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=build/conan_toolchain.cmake
cmake --build build
```

Add these headers to your source files as needed:

```cpp
#include <coro/coro.h>               // Coro<T> — async function return type
#include <coro/coro_stream.h>        // CoroStream<T> — async generator return type
#include <coro/runtime/runtime.h>    // Runtime, spawn()
#include <coro/sync/join.h>          // join()
#include <coro/sync/select.h>        // select()
#include <coro/sync/sleep.h>         // sleep_for()
#include <coro/sync/timeout.h>       // timeout()
```

---

## 1. Your first coroutine

An async function returns `Coro<T>` and uses `co_return` to produce its value.
`Runtime::block_on()` drives the coroutine to completion from synchronous code.

```cpp
#include <coro/coro.h>
#include <coro/runtime/runtime.h>
#include <iostream>

coro::Coro<int> compute() {
    co_return 6 * 7;
}

int main() {
    coro::Runtime rt;
    int result = rt.block_on(compute());
    std::cout << result << "\n";  // 42
}
```

For functions that produce no value, use `Coro<void>`:

```cpp
coro::Coro<void> say_hello() {
    std::cout << "Hello from a coroutine!\n";
    co_return;
}

int main() {
    coro::Runtime rt;
    rt.block_on(say_hello());
}
```

### Choosing a runtime

`Runtime` accepts an optional thread count. With more than one thread it uses
the work-stealing multi-threaded executor; with one thread it runs everything on
the calling thread (useful for testing and single-threaded applications).

```cpp
coro::Runtime rt;       // hardware_concurrency() threads — work-stealing executor
coro::Runtime rt(4);    // 4 threads — work-stealing executor
coro::Runtime rt(1);    // 1 thread  — single-threaded executor (deterministic)
```

---

## 2. Awaiting another coroutine

Inside a coroutine, `co_await` suspends until another `Future` completes and
unwraps its result directly into a variable.

```cpp
coro::Coro<int> fetch_value() {
    co_return 100;
}

coro::Coro<void> run() {
    int v = co_await fetch_value();
    std::cout << v << "\n";  // 100
}
```

---

## 3. Spawning background tasks

`spawn()` schedules a future as an independent background task and returns a
`JoinHandle`. The handle is itself a `Future` — `co_await` it to retrieve the result.
Multiple tasks run concurrently and are driven by the same executor.

```cpp
#include <coro/coro.h>
#include <coro/runtime/runtime.h>

coro::Coro<int> fetch(int id) {
    co_return id * 10;
}

coro::Coro<void> run() {
    // Spawn both tasks before awaiting either — they run concurrently.
    // .name() is optional; it tags the task for debugging.
    auto h1 = coro::spawn(fetch(1)).name("fetch-1").submit();
    auto h2 = coro::spawn(fetch(2)).name("fetch-2").submit();

    int a = co_await h1;  // 10
    int b = co_await h2;  // 20
    std::cout << a + b << "\n";  // 30
}

int main() {
    coro::Runtime rt;
    rt.block_on(run());
}
```

### Dropping a handle without awaiting

If you drop a `JoinHandle` without awaiting it the task is cancelled. The enclosing
coroutine's implicit scope waits for the cancelled task to finish draining before it
completes — so there are no dangling tasks.

```cpp
coro::Coro<void> run() {
    auto handle = coro::spawn(fetch(1)).submit();
    // handle goes out of scope here — task is cancelled and drained automatically
    co_return;
}
```

To let the task run without cancelling or waiting for it, call `detach()`:

```cpp
coro::spawn(fetch(1)).submit().detach();  // fire and forget
```

### Task lifetime and the scope safety problem

In Rust, `tokio::spawn()` requires `'static` — spawned tasks must own all their data,
making it a compile-time error to capture a reference into the spawning context. C++ has
no equivalent constraint, so it is possible to spawn a task that borrows from the parent
coroutine:

```cpp
coro::Coro<void> unsafe_example() {
    int local_data = 42;
    // Dangerous — child captures a pointer to local_data.
    auto h = coro::spawn(worker(&local_data)).submit();
    co_return;  // local_data destroyed here; child may still be running
}
```

To close this gap, every `Coro<T>` implicitly tracks its spawned children and defers
frame destruction until they have drained. This catches most cases automatically — but
there is one gap: at `co_return`, locals are destroyed before the implicit drain wait
begins, so a child that references a local can still see dangling memory.

**The safe patterns are:**

1. **Pass owned data** — move values into the child instead of capturing references.
2. **Use `co_invoke` + `JoinSet::drain()`** — `co_invoke` keeps the lambda (and its
   captures) alive for the full lifetime of the scope, and `drain()` ensures all
   children finish before locals are destroyed.

```cpp
coro::Coro<void> safe_example() {
    int shared_value = 42;

    co_await coro::co_invoke([&]() -> coro::Coro<void> {
        coro::JoinSet<void> js;
        js.spawn(worker(&shared_value));  // safe — co_invoke keeps shared_value alive
        co_await js.drain();              // all children done before this scope exits
    });
    // shared_value still alive here
}
```

See the [Coroutine Scope design document](coroutine_scope.md) for a full explanation of
the implicit scope mechanism, its limits, and how it compares to Rust's `'static` bound.

---

## 4. Fan-out with `JoinSet`

When you need to spawn many tasks of the same type and collect their results, `JoinSet<T>`
is cleaner than managing individual `JoinHandle`s. It satisfies `Stream<T>`, so results
arrive in completion order (not spawn order) and compose naturally with `next()`.

```cpp
#include <coro/task/join_set.h>
#include <coro/co_invoke.h>

coro::Coro<int> compute(int x) { co_return x * x; }

coro::Coro<void> run() {
    co_await coro::co_invoke([&]() -> coro::Coro<void> {
        coro::JoinSet<int> js;
        for (int i = 1; i <= 5; ++i)
            js.spawn(compute(i));

        while (auto result = co_await coro::next(js))
            std::cout << *result << " ";  // prints squares in completion order
        std::cout << "\n";
    });
}
```

For `void` tasks, use `JoinSet<void>` and `drain()` to wait for all to finish:

```cpp
coro::Coro<void> run() {
    co_await coro::co_invoke([&]() -> coro::Coro<void> {
        coro::JoinSet<void> js;
        js.spawn(task_a());
        js.spawn(task_b());
        co_await js.drain();  // waits for both; rethrows first exception if any
    });
}
```

Dropping a `JoinSet` without calling `drain()` cancels all pending tasks. The enclosing
`co_invoke` scope ensures they finish draining before the coroutine returns.

> **Note:** When spawning tasks that capture references to locals, wrap the fan-out in
> `co_invoke` to keep the captures alive for the full duration of the `JoinSet` (see section 11).

---

## 5. Joining a fixed set of futures

`join()` runs a fixed set of heterogeneous futures concurrently and waits for **all** of
them to complete. Unlike `JoinSet`, the number and types of branches are fixed at compile
time. Results are returned as a `std::tuple` in branch order.

```cpp
#include <coro/sync/join.h>

coro::Coro<int>         fetch_int()    { co_return 42; }
coro::Coro<std::string> fetch_string() { co_return "hello"; }

coro::Coro<void> run() {
    auto [n, s] = co_await coro::join(fetch_int(), fetch_string());
    std::cout << n << " " << s << "\n";  // 42 hello
}
```

`void`-returning branches contribute a `VoidJoinBranch{}` placeholder in the tuple:

```cpp
coro::Coro<void> side_effect() { co_return; }

coro::Coro<void> run() {
    auto [n, _] = co_await coro::join(fetch_int(), side_effect());
    std::cout << n << "\n";  // 42
}
```

If any branch throws, the remaining branches are cancelled and drained, then the first
exception is re-thrown. Use `JoinSet` instead when the number of branches is dynamic.

---

## 6. Async generators

`CoroStream<T>` is an async generator: use `co_yield` to emit items and `co_return`
to signal exhaustion. Consume it with `co_await coro::next(stream)` in a loop.

```cpp
#include <coro/coro_stream.h>
#include <coro/stream.h>
#include <coro/coro.h>
#include <coro/runtime/runtime.h>
#include <iostream>

coro::CoroStream<int> range(int n) {
    for (int i = 0; i < n; ++i)
        co_yield i;
}

coro::Coro<void> consume() {
    auto stream = range(5);
    while (auto item = co_await coro::next(stream))
        std::cout << *item << " ";  // 0 1 2 3 4
    std::cout << "\n";
}

int main() {
    coro::Runtime rt;
    rt.block_on(consume());
}
```

A generator can also `co_await` futures internally, suspending the stream until
the awaited future resolves.

---

## 7. Sleeping

`sleep_for()` suspends the current coroutine for a duration without blocking any
worker thread. Other tasks continue to run on the executor while a coroutine sleeps.

```cpp
#include <coro/sync/sleep.h>
#include <chrono>

coro::Coro<void> run() {
    using namespace std::chrono_literals;
    std::cout << "before\n";
    co_await coro::sleep_for(100ms);
    std::cout << "after\n";
}
```

---

## 8. Racing futures with `select`

`select()` races two or more futures and returns as soon as one completes.
The result is a `std::variant` of `SelectBranch<N, T>` values identifying which
branch won and carrying its result. All other branches are cancelled and drained.

```cpp
#include <coro/coro.h>
#include <coro/sync/select.h>
#include <coro/runtime/runtime.h>
#include <iostream>

coro::Coro<int> fast() { co_return 1; }
coro::Coro<int> slow() { co_return 2; }

coro::Coro<void> run() {
    auto result = co_await coro::select(fast(), slow());

    // Branch 0 (fast) won — branch 1 (slow) is cancelled and drained.
    std::visit([](auto branch) {
        std::cout << "branch " << branch.index << " won\n";
    }, result);
}

int main() {
    coro::Runtime rt;
    rt.block_on(run());
}
```

---

## 9. Timeouts

`timeout(duration, future)` is a convenience wrapper around `select` that races your
future against a `sleep_for` timer.

```cpp
#include <coro/coro.h>
#include <coro/sync/timeout.h>
#include <coro/runtime/runtime.h>
#include <chrono>
#include <iostream>

coro::Coro<int> fetch_data() {
    co_return 42;
}

coro::Coro<void> run() {
    using namespace std::chrono_literals;

    auto result = co_await coro::timeout(500ms, fetch_data());

    if (result.index() == 0)
        std::cout << "got " << std::get<0>(result).value << "\n";  // fetch_data won
    else
        std::cout << "timed out\n";
}

int main() {
    coro::Runtime rt;
    rt.block_on(run());
}
```

The return type of `timeout(d, F)` is the same as `select(F, SleepFuture)`:
- `SelectBranch<0, T>` — the future completed in time.
- `SelectBranch<1, void>` — the deadline elapsed first.

---

## 10. Channels

> *"Do not communicate by sharing memory; instead, share memory by communicating."*
> — Go team

Channels are the practical expression of this principle: rather than protecting shared
state with mutexes, pass ownership of data between coroutines so only one task holds it
at a time making channels much less likely to be misused. Three variants are provided:

| Variant | Producers | Consumers | Notes |
|---|---|---|---|
| `oneshot` | 1 | 1 | Single value; send is synchronous |
| `mpsc`    | N (cloneable sender) | 1 | Bounded buffer; backpressured send |
| `watch`   | 1 | N (cloneable receiver) | Last-value-wins; send never suspends |

> *NOTE*: Tokio also provides a broadcast channel that is not implemented yet, but likely will be added in the near future

### RAII handles and disconnection

Every channel end is a RAII handle. Dropping a handle signals disconnection to the other
side automatically — no explicit `close()` call is needed:

- **Sender dropped** — any receiver waiting for a value wakes immediately and gets a
  `ChannelError` result. For `mpsc`, dropping all senders closes the channel and causes
  the receiver's `next()` loop to terminate naturally.
- **Receiver dropped** — any sender waiting to send wakes immediately and gets its value
  back in the error slot of the returned `std::expected`, so move-only values are never
  silently lost.

For `watch`, the sender dropping is the normal way to signal that no more updates will
arrive. Receivers observe this as an error on their next `changed()` call and can exit
cleanly — it is not an unexpected failure.

### Error handling

All channel operations return `std::expected<T, ChannelError>` rather than throwing.
Call `.value()` to throw on error, or check the result explicitly.

```cpp
#include <coro/sync/oneshot.h>
#include <coro/sync/mpsc.h>
#include <coro/sync/watch.h>
```

### oneshot — single value, one sender, one receiver

Use `oneshot` to hand a single result from one task to another.
`send()` is synchronous and can be called from any thread.

```cpp
coro::Coro<void> run() {
    auto [tx, rx] = coro::oneshot::channel<int>();

    // Spawn a task that sends the result back.
    auto h = coro::spawn(coro::co_invoke(
        [tx = std::move(tx)]() mutable -> coro::Coro<void> {
            tx.send(42);
            co_return;
        })).submit();

    auto result = co_await rx;      // std::expected<int, ChannelError>
    std::cout << result.value() << "\n";  // 42
    co_await h;
}
```

If the sender is dropped without calling `send()`, `co_await rx` returns
`std::unexpected(ChannelError::Closed)`.

### mpsc — bounded queue, multiple producers, one consumer

Use `mpsc` for producer/consumer pipelines. The receiver satisfies `Stream<T>`,
so consume it with `next()` in a loop.

```cpp
coro::Coro<void> run() {
    co_await coro::co_invoke([&]() -> coro::Coro<void> {
        auto [tx, rx] = coro::mpsc::channel<int>(/*capacity=*/16);

        // Two producer tasks.
        coro::JoinSet<void> producers;
        for (int i = 0; i < 2; ++i) {
            producers.spawn(coro::co_invoke(
                [tx = tx.clone(), i]() mutable -> coro::Coro<void> {
                    for (int j = 0; j < 3; ++j)
                        co_await tx.send(i * 10 + j);
                }));
        }

        // Consume until all senders are dropped.
        while (auto v = co_await coro::next(rx))
            std::cout << *v << " ";
        std::cout << "\n";

        co_await producers.drain();
    });
}
```

`send()` suspends the producer if the buffer is full, providing natural
backpressure. Use `trySend()` for a non-blocking attempt.

### watch — last-value channel, one sender, many receivers

Use `watch` to broadcast configuration or state that multiple tasks need to
observe. Receivers call `changed()` to wait for the next update, then `borrow()`
to read the current value under a shared lock.

```cpp
coro::Coro<void> run() {
    co_await coro::co_invoke([&]() -> coro::Coro<void> {
        auto [tx, rx] = coro::watch::channel<int>(/*initial=*/0);

        // Spawn a watcher.
        auto h = coro::spawn(coro::co_invoke(
            [rx = std::move(rx)]() mutable -> coro::Coro<void> {
                while (true) {
                    auto r = co_await rx.changed();
                    if (!r) co_return;          // sender dropped — channel closed
                    std::cout << *rx.borrow() << "\n";
                }
            })).submit();

        tx.send(1);
        tx.send(2);
        tx.send(3);
        // Dropping tx closes the channel; the watcher's changed() will return an error.
        co_await h;
    });
}
```

`rx.clone()` creates an independent receiver with its own cursor — useful when
multiple tasks need to track changes independently.

> **Note:** do not hold a `BorrowGuard` across a `co_await` point. Doing so keeps
> the shared read lock held while suspended, which blocks all `send()` calls.

---

## 11. Capturing-lambda pitfall and `co_invoke`

A capturing lambda that returns `Coro<T>` has a subtle use-after-free when used as an
rvalue. The compiler lowers the lambda to an anonymous struct; `operator()` — being a member
function — captures `this` into the coroutine frame. The struct is a temporary and is
destroyed at the end of the full expression, before the coroutine is ever polled.

```cpp
// DANGEROUS — lambda struct destroyed at ';', before first resumption
auto coro = [x]() -> Coro<void> {
    co_await something();
    use(x);          // accesses this->x — 'this' is dangling
}();
co_await coro;       // use-after-free
```

Use `co_invoke` instead. It moves the lambda onto the heap inside a wrapper that keeps it
alive for the coroutine's entire lifetime:

```cpp
#include <coro/co_invoke.h>

// SAFE — lambda kept alive by co_invoke
co_await co_invoke([x]() -> Coro<void> {
    co_await something();
    use(x);    // safe
});

// Also works with spawn:
auto handle = spawn(co_invoke([x]() -> Coro<void> { ... })).submit();
```

`co_invoke` also works with `CoroStream<T>` lambdas.

## 12. Async I/O

The runtime integrates a libuv event loop so that network I/O suspends a coroutine
without blocking any worker thread. Two I/O abstractions are provided: `TcpStream`
for raw TCP and `WsStream`/`WsListener` for WebSocket.

All I/O operations require a running `Runtime` — they are not usable from plain
synchronous code.

### TcpStream — raw TCP

`TcpStream` is a connected, async TCP socket. Connect with `co_await
TcpStream::connect(host, port)`, then read and write using byte spans.

```cpp
#include <coro/io/tcp_stream.h>
#include <coro/runtime/runtime.h>
#include <array>
#include <cstddef>
#include <iostream>
#include <string_view>

coro::Coro<void> run() {
    coro::TcpStream tcp = co_await coro::TcpStream::connect("example.com", 80);

    // Write: span must remain valid until the future resolves.
    std::string_view req = "GET / HTTP/1.0\r\nHost: example.com\r\n\r\n";
    co_await tcp.write({
        reinterpret_cast<const std::byte*>(req.data()), req.size()
    });

    // Read: returns bytes read, or 0 on EOF.
    std::array<std::byte, 4096> buf;
    while (std::size_t n = co_await tcp.read(buf)) {
        std::cout << std::string_view(reinterpret_cast<const char*>(buf.data()), n);
    }
}

int main() {
    coro::Runtime rt;
    rt.block_on(run());
}
```

**Ownership rules:**
- The buffer passed to `read()` must remain valid until the future resolves — do not
  let it go out of scope across a `co_await`.
- The data span passed to `write()` must similarly remain valid until the future
  resolves.
- Dropping a `ReadFuture` or `WriteFuture` mid-flight is not safe in the current
  implementation. Always `co_await` them to completion or use `timeout()` around the
  whole operation.

### WsStream — WebSocket client

`WsStream` is a connected async WebSocket stream. `connect()` parses the URL,
performs the opening handshake, and returns a ready-to-use stream.

```cpp
#include <coro/io/ws_stream.h>
#include <coro/runtime/runtime.h>
#include <iostream>

coro::Coro<void> run() {
    // Connect to a WebSocket server.  ws:// and wss:// (TLS) are both supported.
    coro::WsStream ws = co_await coro::WsStream::connect("ws://localhost:9001/");

    // Send a text frame — string_view overload picks Text opcode automatically.
    co_await ws.send("hello");

    // Receive the echo.
    coro::WsStream::Message reply = co_await ws.receive();
    std::cout << reply.as_text() << "\n";  // "hello"

    // Destructor sends a WebSocket Close frame automatically.
}

int main() {
    coro::Runtime rt;
    rt.block_on(run());
}
```

`receive()` returns a `Message`:

| Field | Type | Description |
|---|---|---|
| `data` | `std::vector<std::byte>` | Raw payload bytes |
| `is_text` | `bool` | `true` for a text frame, `false` for binary |
| `is_final` | `bool` | Always `true` in the default `Full` mode |
| `as_text()` | `std::string_view` | Convenient text view; throws if `is_text` is false |

`send()` has two overloads:

```cpp
co_await ws.send("hello");                        // text frame (string_view)
co_await ws.send(span_of_bytes, OpCode::Binary);  // binary frame
```

Use `timeout()` to guard individual operations against a stalled peer:

```cpp
using namespace std::chrono_literals;

auto result = co_await coro::timeout(2s, ws.receive());
if (result.index() != 0) { /* timed out */ }
coro::WsStream::Message& msg = std::get<0>(result).value;
```

To request specific subprotocols, pass them as a third argument to `connect()`:

```cpp
coro::WsStream ws = co_await coro::WsStream::connect(
    "ws://localhost:9001/", coro::WsStream::FrameMode::Full, {"my-protocol"});
```

### WsListener — WebSocket server

`WsListener` binds a port and accepts incoming WebSocket connections. Each accepted
connection is a `WsStream` with the same API as the client side.

```cpp
#include <coro/io/ws_listener.h>
#include <coro/io/ws_stream.h>
#include <coro/task/join_set.h>
#include <coro/runtime/runtime.h>
#include <iostream>

static coro::Coro<void> handle(coro::WsStream ws) {
    for (;;) {
        coro::WsStream::Message msg = co_await ws.receive();
        if (msg.data.empty()) co_return;  // client closed
        co_await ws.send(msg.data, msg.is_text ? coro::WsStream::OpCode::Text
                                               : coro::WsStream::OpCode::Binary);
    }
}

coro::Coro<void> run_server() {
    coro::WsListener listener = co_await coro::WsListener::bind("127.0.0.1", 9001);

    coro::JoinSet<void> sessions;
    for (int i = 0;; ++i) {
        coro::WsStream ws = co_await listener.accept();
        sessions.spawn(handle(std::move(ws)));
    }
}

int main() {
    coro::Runtime rt;
    rt.block_on(run_server());
}
```

Dropping `WsListener` destroys the server context; active `WsStream`s that were
already accepted are unaffected.

---

## Next steps

- Read the **Design Docs** for a deeper explanation of the `Future`/`Stream` model,
  the executor architecture, and the coroutine scope lifetime guarantees.
- Browse the **API Reference** for the full class and function documentation.
