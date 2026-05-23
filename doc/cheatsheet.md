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
#include <coro/sync/oneshot.h>      // oneshot::channel<T>
#include <coro/sync/mpsc.h>         // mpsc::channel<T>
#include <coro/sync/watch.h>        // watch::channel<T>
#include <coro/io/tcp_stream.h>     // TcpStream
#include <coro/io/tcp_listener.h>   // TcpListener
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
    auto s = range(5);
    while (auto item = co_await coro::next(s))
        use(*item);
}
```

## Runtime entry point

```cpp
coro::Runtime rt;    // WorkStealingExecutor, hardware_concurrency() threads
coro::Runtime rt(4); // WorkStealingExecutor, 4 threads
coro::Runtime rt(1); // SingleThreadedExecutor — deterministic, good for tests

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
auto h = coro::spawn(compute());
int v = co_await h;

auto h = coro::build_task().name("my-task").spawn(compute()); // named, for debugging
coro::spawn(compute()).detach();                               // fire and forget

{
    auto h = coro::spawn(compute());
}  // h goes out of scope → task cancelled and drained
```

## JoinSet — dynamic fan-out

```cpp
// results in completion order
co_await coro::co_invoke([&]() -> coro::Coro<void> {
    coro::JoinSet<int> js;
    for (int i = 0; i < N; ++i)
        js.spawn(compute(i));
    while (auto result = co_await coro::next(js))
        use(*result);
});

co_await coro::co_invoke([&]() -> coro::Coro<void> {
    coro::JoinSet<void> js;
    js.spawn(task_a());
    js.spawn(task_b());
    co_await js.drain();
});
```

## join — concurrent fixed fan-out

```cpp
auto [a, b] = co_await coro::join(fetch_int(), fetch_string());
auto [n, _] = co_await coro::join(fetch_int(), side_effect()); // void → VoidJoinBranch{}
```

## select — race futures

```cpp
// first to complete wins; others cancelled
auto sel = co_await coro::select(fast(), slow());
// sel is std::variant<SelectBranch<0,T0>, SelectBranch<1,T1>, ...>
if (sel.index() == 0)
    use(std::get<0>(sel).value);

// coro::ref — borrow without cancelling the losing branch
coro::JoinHandle<int> task = coro::spawn(long_running());
while (true) {
    auto sel = co_await coro::select(coro::ref(task), coro::sleep_for(100ms));
    if (sel.index() == 0) { use(std::get<0>(sel).value); break; }
}
```

## timeout & sleep_for

```cpp
using namespace std::chrono_literals;

co_await coro::sleep_for(100ms);

// equivalent to select(fetch_data(), sleep_for(500ms))
auto result = co_await coro::timeout(500ms, fetch_data());
if (result.index() == 0)
    use(std::get<0>(result).value);
else
    /* timed out */;
```

## Mutex

```cpp
coro::Mutex mtx;
auto guard = co_await mtx.lock(); // suspends if locked; no thread is blocked
// guard releases on destruction — safe to co_await while holding
```

## Channels

```cpp
// oneshot — single value, one sender, one receiver
auto [tx, rx] = coro::oneshot::channel<int>();
tx.send(42);                              // synchronous, any thread
auto r = co_await rx;                     // std::expected<int, ChannelError>

// mpsc — bounded queue, multiple producers, one consumer
auto [tx, rx] = coro::mpsc::channel<int>(/*capacity=*/16);
auto tx2 = tx.clone();                    // clone for additional producers
co_await tx.send(v);                      // suspends if full
tx.trySend(v);                            // non-blocking attempt
while (auto v = co_await coro::next(rx))  // exits when all senders dropped
    use(*v);

// watch — last-value broadcast, one sender, many receivers
auto [tx, rx] = coro::watch::channel<int>(/*initial=*/0);
tx.send(42);                              // synchronous, never suspends
auto rx2 = rx.clone();                    // independent receiver
auto r = co_await rx.changed();           // wait for next update
if (!r) co_return;                        // sender dropped
{ auto g = rx.borrow(); use(*g); }        // read under shared lock
// do NOT hold BorrowGuard across co_await
```

## Capturing-lambda safety

```cpp
// WRONG — closure destroyed before first co_await
coro::spawn([x]() -> coro::Coro<void> { co_await f(); use(x); }());

// CORRECT — co_invoke keeps closure alive for the duration
coro::spawn(coro::co_invoke([x]() -> coro::Coro<void> {
    co_await f(); use(x);
}));

// Borrow outer locals safely with co_invoke scope
int shared = 42;
co_await coro::co_invoke([&]() -> coro::Coro<void> {
    auto h = coro::spawn(worker(&shared)); // shared outlives this scope
    co_await h;
});
```

## Cancellation

```cpp
{
    auto h = coro::spawn(long_running());
} // drop → root cancelled → children cancelled recursively; no tokens needed
```

## spawn_blocking — offload blocking code

```cpp
#include <coro/task/spawn_blocking.h>

int result = co_await coro::spawn_blocking([]() {
    return expensive_sync_computation(); // runs on thread pool, not executor
});
```

## TCP I/O

```cpp
// Client
auto stream = co_await coro::TcpStream::connect("127.0.0.1", 8080);
auto [n, buf] = co_await stream.read(std::vector<std::byte>(4096));
buf.resize(n);
co_await stream.write(std::move(buf));

// Server
auto listener = co_await coro::TcpListener::bind("0.0.0.0", 8080);
while (auto conn = co_await coro::next(listener))
    coro::spawn(handle(*conn)).detach();
```

## File I/O

```cpp
auto f = co_await coro::File::open("data.txt", coro::FileMode::Read);
auto [n, buf] = co_await f.read(std::vector<std::byte>(4096));
buf.resize(n);

auto out = co_await coro::File::open(
    "out.txt", coro::FileMode::Write | coro::FileMode::Create | coro::FileMode::Truncate);
co_await out.write(std::move(buf));

auto [n2, buf2] = co_await f.read_at(std::vector<std::byte>(4096), /*offset=*/512);
auto [n3, buf3] = co_await f.read(std::vector<std::byte>(4096), /*exact=*/true);
```
