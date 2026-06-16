<a href="../cheatsheet.pdf">Download / open as PDF</a>

## Headers

```cpp
#include <coro/coro.h>              // Coro<T>
#include <coro/coro_stream.h>       // CoroStream<T>
#include <coro/co_invoke.h>         // co_invoke()
#include <coro/runtime/runtime.h>   // Runtime, spawn(), build_task()
#include <coro/task/join_handle.h>  // JoinHandle<T>
#include <coro/task/join_set.h>     // JoinSet<T>
#include <coro/sync/join.h>         // join()
#include <coro/sync/select.h>       // select()
#include <coro/sync/sleep.h>        // sleep_for()
#include <coro/sync/timeout.h>      // timeout()
#include <coro/sync/mutex.h>        // Mutex
#include <coro/sync/oneshot.h>      // oneshot_channel<T>
#include <coro/sync/mpsc.h>         // mpsc_channel<T>
#include <coro/sync/watch.h>        // watch_channel<T>
#include <coro/sync/event.h>         // Event
#include <coro/io/tcp_stream.h>     // TcpStream
#include <coro/io/tcp_listener.h>   // TcpListener
#include <coro/io/ws_stream.h>      // WsStream
#include <coro/io/ws_listener.h>    // WsListener
#include <coro/io/file.h>           // File
```

## Async functions

```cpp
coro::Coro<int> compute() {
    co_return 42;
}

coro::Coro<void> do_work() {
    co_await something();
}

coro::Coro<void> run() {
    int v = co_await compute();
}
```

## Async generators
```cpp
coro::CoroStream<int> range(int n) {
    for (int i = 0; i < n; ++i)
        co_yield i;
}

coro::Coro<void> consume() {
    coro::CoroStream<int> s = range(5);
    while (std::optional<int> item = co_await coro::next(s))
        use(*item);
}
```

## Runtime entry point

```cpp
// WorkStealingExecutor, hardware_concurrency() threads
coro::Runtime rt;
// WorkStealingExecutor, 4 threads
coro::Runtime rt(4);
// SingleThreadedExecutor — deterministic, good for tests
coro::Runtime rt(1);

int result = rt.block_on(compute());
rt.block_on(do_work());

// Explicit executor selection:
#include <coro/runtime/work_stealing_executor.h>
#include <coro/runtime/single_threaded_executor.h>
coro::Runtime rt(std::in_place_type<coro::WorkStealingExecutor>, 4);
coro::Runtime rt(std::in_place_type<coro::SingleThreadedExecutor>);
```

## Spawn & JoinHandle

```cpp
coro::JoinHandle<int> h = coro::spawn(compute());
int v = co_await h;

// named task — useful for debugging
coro::JoinHandle<int> h =
    coro::build_task().name("my-task").spawn(compute());
// fire and forget
coro::spawn(compute()).detach();

{
    coro::JoinHandle<int> h = coro::spawn(compute());
}  // h goes out of scope → task cancelled and drained
```

## JoinSet — dynamic fan-out

```cpp
// results in completion order
co_await coro::co_invoke([&]() -> coro::Coro<void> {
    coro::JoinSet<int> js;
    for (int i = 0; i < N; ++i)
        js.spawn(compute(i));
    // For JoinSet<void>, next() returns bool (true = completed,
    // false = exhausted) since std::optional<void> is ill-formed.
    while (std::optional<int> result = co_await coro::next(js))
        use(*result);
});

// wait for all tasks; discard results
co_await js.drain();
```

## join — concurrent fixed fan-out

```cpp
// std::tuple<int, std::string>
auto [a, b] = co_await coro::join(fetch_int(), fetch_string());
// std::tuple<int, VoidJoinBranch> — void results become VoidJoinBranch{}
auto [n, _] = co_await coro::join(fetch_int(), side_effect());
```

## select — race futures

```cpp
// first to complete wins; others cancelled
// std::variant<SelectBranch<0,T0>, SelectBranch<1,T1>, ...>
auto sel = co_await coro::select(fast(), slow());
if (sel.index() == 0)
    use(std::get<0>(sel).value);

// coro::ref — borrow without cancelling the losing branch
coro::JoinHandle<int> task = coro::spawn(long_running());
while (true) {
    auto sel = co_await coro::select(
        coro::ref(task), coro::sleep_for(100ms));
    if (sel.index() == 0) { use(std::get<0>(sel).value); break; }
}
```

## timeout & sleep_for

```cpp
using namespace std::chrono_literals;

co_await coro::sleep_for(100ms);

// variant<SelectBranch<0,T>, SelectBranch<1,void>>
// index 0 = result, index 1 = timed out
auto result = co_await coro::timeout(500ms, fetch_data());
if (result.index() == 0)
    use(std::get<0>(result).value);
else
    /* timed out */;
```

## Mutex

```cpp
coro::Mutex mtx;
// suspends if locked; no thread is blocked
coro::MutexGuard guard = co_await mtx.lock();
// guard releases on destruction — safe to co_await while holding
```

## Event — one-shot signal, any thread to coroutine

```cpp
coro::Event ev;
ev.set();              // synchronous, any thread; latch semantics
ev.clear();            // reset for reuse
co_await ev.wait();    // suspends until set(); returns immediately if already set
```

## oneshot — single-use, one value, one sender, one receiver

```cpp
// OneshotSender<int>, OneshotReceiver<int>
auto [tx, rx] = coro::oneshot_channel<int>();
tx.send(42);                        // synchronous, any thread
std::expected<int, ChannelError> r = co_await rx;
```

## mpsc — bounded queue, multiple producers, one consumer

```cpp
// MpscSender<int>, MpscReceiver<int>
auto [tx, rx] = coro::mpsc_channel<int>(/*capacity=*/16);
MpscSender<int> tx2 = tx.clone(); // clone for additional producers
co_await tx.send(v);               // suspends if full
tx.try_send(v);                    // non-blocking attempt
// exits when all senders dropped
while (std::optional<int> v = co_await coro::next(rx))
    use(*v);
// for plain OS threads (not coroutines): blocks the calling thread
std::optional<int> v = rx.blocking_recv();
```

## watch — last-value broadcast, multiple senders, many receivers

```cpp
// WatchSender<int>, WatchReceiver<int>
auto [tx, rx] = coro::watch_channel<int>(/*initial=*/0);
tx.send(42);  // synchronous, never suspends
WatchSender<int> tx2 = tx.clone();   // independent sender
WatchReceiver<int> rx2 = rx.clone(); // independent receiver
// wait for next update; std::expected<void, ChannelError>
std::expected<void, ChannelError> r = co_await rx.changed();
if (!r) co_return;  // sender dropped
// read under shared lock — do NOT hold across co_await
{ WatchBorrowGuard<int> g = rx.borrow(); use(*g); }
// write in place; version++ and receivers notified on drop
{ WatchBorrowMutGuard<int> g = tx.borrow_mut(); *g = 99; }
```

## Capturing-lambda safety

```cpp
// WRONG — closure destroyed before first co_await
coro::spawn([x]() -> coro::Coro<void> {
    co_await f(); use(x);
}());

// CORRECT — co_invoke keeps closure alive for the duration
coro::spawn(coro::co_invoke([x]() -> coro::Coro<void> {
    co_await f(); use(x);
}));

// Borrow outer locals safely with co_invoke scope
int shared = 42;
co_await coro::co_invoke([&]() -> coro::Coro<void> {
    // shared outlives this scope
    coro::JoinHandle<void> h = coro::spawn(worker(&shared));
    co_await h;
});
```

## Structured Cancellation

```cpp
{
    coro::JoinHandle<void> h = coro::spawn(long_running());
// drop → root cancelled → children cancelled recursively
}
```

## spawn_blocking — offload blocking code

```cpp
#include <coro/task/spawn_blocking.h>

// runs on thread pool, not executor
int result = co_await coro::spawn_blocking([]() {
    return expensive_sync_computation();
});
```

## WebSocket I/O

```cpp
// Client
coro::WsStream ws = co_await coro::WsStream::connect("ws://localhost:8080/path");
// WsStream::Message { std::vector<std::byte> data; bool is_text; bool is_final; }
coro::WsStream::Message msg = co_await ws.receive();
co_await ws.send(std::span<const std::byte>(msg.data), /*is_text=*/false);
co_await ws.send("hello");  // text frame

// Server
coro::WsListener listener =
    co_await coro::WsListener::bind("0.0.0.0", 8080);
while (true) {
    coro::WsStream conn = co_await listener.accept();
    coro::spawn(handle(std::move(conn))).detach();
}
```

## TCP I/O

```cpp
// Client
coro::TcpStream stream =
    co_await coro::TcpStream::connect("127.0.0.1", 8080);
// std::pair<size_t, vector<byte>>
auto& [n, buf] = co_await stream.read(std::vector<std::byte>(4096));
buf.resize(n);
co_await stream.write(std::move(buf));

// Server
coro::TcpListener listener =
    co_await coro::TcpListener::bind("0.0.0.0", 8080);
// std::optional<TcpStream>
while (auto conn = co_await coro::next(listener))
    coro::spawn(handle(*conn)).detach();
```

## File I/O

```cpp
coro::File f =
    co_await coro::File::open("data.txt", coro::FileMode::Read);
// std::pair<size_t, vector<byte>>
auto& [n, buf] = co_await f.read(std::vector<std::byte>(4096));
buf.resize(n);

coro::File out = co_await coro::File::open("out.txt",
    coro::FileMode::Write | coro::FileMode::Create |
    coro::FileMode::Truncate);
co_await out.write(std::move(buf));

auto& [n2, buf2] =
    co_await f.read_at(std::vector<std::byte>(4096), /*offset=*/512);

// _exact variants loop until the buffer is full (or EOF):
auto& [n3, buf3] = co_await f.read_exact(std::vector<std::byte>(4096));
auto& [n4, buf4] = co_await f.read_at_exact(std::vector<std::byte>(4096), /*offset=*/512);
co_await out.write_exact(std::move(buf));
co_await out.write_at_exact(std::move(buf2), /*offset=*/512);
```
