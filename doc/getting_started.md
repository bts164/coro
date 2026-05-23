# Getting Started

This guide walks through the core concepts of the library with working examples. It is
organized in six groups: the coroutine keywords (`co_return`, `co_await`, `co_yield`),
the runtime and executor, spawning parallel tasks, thread-safe communication and
synchronization, `JoinSet` for dynamic fan-out, and the concurrent combinators
(`join`, `select`, `timeout`).

## Setup

The library requires C++20 and uses CMake. The recommended and supported
method for managing dependencies is with [Conan](https://conan.io). Other
package managers or manual installs should also work.

CMake locates dependencies through `find_package()`, which searches standard platform
locations automatically for common packages (e.g. GTest ships its own CMake config). For
less common packages you can point CMake at a manually installed prefix via
`CMAKE_PREFIX_PATH`, or write a
[CMake package config file](https://cmake.org/cmake/help/latest/command/find_package.html).
Conan is simply the most convenient way to generate these config files consistently across
platforms.

### Building with Conan

```bash
conan install . --build=missing -s:h build_type=Release
cmake --preset conan-release
cd build/Release && make
```

**`--build=missing`** (shorthand: `-b=missing`) — optional. Instructs Conan to build any
dependency that does not have a pre-built binary available, rather than exiting with an
error.

**`-s:h build_type=Release`** — optional. Selects the build type. Valid values are
`Release`, `Debug`, and `RelWithDebInfo`. Defaults to `Release` if omitted. The CMake
preset and build directory must match:

| Build type | CMake preset | Build directory |
|---|---|---|
| `Release` | `conan-release` | `build/Release` |
| `Debug` | `conan-debug` | `build/Debug` |
| `RelWithDebInfo` | `conan-relwithdebinfo` | `build/RelWithDebInfo` |

Common headers:

```cpp
// Core coroutine types
#include <coro/coro.h>                    // Coro<T> — async function return type
#include <coro/coro_stream.h>             // CoroStream<T> — async generator return type
#include <coro/future.h>                  // Future/Cancellable concepts, FutureRef, coro::ref()
#include <coro/stream.h>                  // Stream concept, coro::next()
#include <coro/co_invoke.h>               // co_invoke() — safe capturing-lambda coroutines

// Runtime
#include <coro/runtime/runtime.h>         // Runtime, spawn(), build_task()

// Tasks
#include <coro/task/join_handle.h>        // JoinHandle<T>
#include <coro/task/join_set.h>           // JoinSet<T>
#include <coro/task/spawn_blocking.h>     // spawn_blocking()
#include <coro/task/spawn_on.h>           // spawn_on(), with_context()

// Sync primitives
#include <coro/sync/select.h>             // select()
#include <coro/sync/join.h>               // join()
#include <coro/sync/sleep.h>              // sleep_for()
#include <coro/sync/timeout.h>            // timeout()
#include <coro/sync/event.h>              // Event — single-waiter set/wait primitive
#include <coro/sync/mutex.h>              // Mutex — async mutex
#include <coro/sync/stream_handle.h>      // StreamHandle<T>

// Channels
#include <coro/sync/oneshot.h>            // oneshot::channel<T>
#include <coro/sync/mpsc.h>               // mpsc::channel<T>
#include <coro/sync/watch.h>              // watch::channel<T>

// I/O
#include <coro/io/file.h>                 // File — async file I/O
#include <coro/io/tcp_stream.h>           // TcpStream — async TCP
#include <coro/io/tcp_listener.h>         // TcpListener — TCP accept loop
#include <coro/io/ws_stream.h>            // WsStream — async WebSocket client
#include <coro/io/ws_listener.h>          // WsListener — WebSocket server
#include <coro/io/poll_stream.h>          // PollStream — character-device / fd streaming
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

## 3. Async generators

`CoroStream<T>` introduces the third coroutine keyword: `co_yield`. Where `co_return`
produces a single value and exits, `co_yield` emits a value and suspends — the generator
resumes from the next `co_yield` when the consumer asks for another item. Consume it with
`co_await coro::next(stream)` in a loop; `next()` returns `std::nullopt` when the
generator is exhausted.

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

A generator can also `co_await` futures internally, suspending the stream until the
awaited future resolves. The `next()` pattern also appears with `JoinSet` and `mpsc`
channels later in this guide — any type satisfying `Stream<T>` is consumed the same way.

---

## 4. The Runtime and Executor

`Runtime::block_on()` is the bridge between synchronous and async code. It takes a
single root coroutine, drives it to completion on the executor, and returns its result
to the caller. Everything else — spawning tasks, awaiting I/O, sleeping — happens from
inside that root coroutine.

```cpp
int main() {
    coro::Runtime rt;
    return rt.block_on(async_main());  // blocks until async_main() completes
}
```

### Choosing an executor

The executor determines how tasks are scheduled across threads. Three are available:

| Executor | Threads | Use case |
|---|---|---|
| `WorkStealingExecutor` | N (default: `hardware_concurrency()`) | Production default — tasks distributed across threads automatically |
| `SingleThreadedExecutor` | 1 | Tests, deterministic environments, single-core targets |
| `WorkSharingExecutor` | N | Rarely needed — see below |

The `Runtime` constructor selects the executor based on the thread count argument:

```cpp
coro::Runtime rt;       // WorkStealingExecutor, hardware_concurrency() threads
coro::Runtime rt(4);    // WorkStealingExecutor, 4 threads
coro::Runtime rt(1);    // SingleThreadedExecutor
```

For explicit control over executor type, use `std::in_place_type`:

```cpp
#include <coro/runtime/work_stealing_executor.h>
#include <coro/runtime/single_threaded_executor.h>
#include <coro/runtime/work_sharing_executor.h>

coro::Runtime rt(std::in_place_type<coro::WorkStealingExecutor>, 4);
coro::Runtime rt(std::in_place_type<coro::SingleThreadedExecutor>);
coro::Runtime rt(std::in_place_type<coro::WorkSharingExecutor>, 4);
```

**Work-stealing** is the right default for most applications. Tasks are distributed
across worker threads; when a thread exhausts its local queue it steals tasks from
other threads, keeping all cores busy without manual load balancing.

**Single-threaded** is ideal for tests and deterministic environments. All coroutines
run on the calling thread — no synchronization is needed for shared state between
coroutines, and execution order is reproducible.

**Work-sharing** predates work-stealing in this library and exists because it was
simpler to implement initially. It uses a single global FIFO queue protected by a mutex, which
becomes a contention bottleneck under any significant task load. It is occasionally
useful when debugging to help isolate whether a bug is specific to the work-stealing
scheduler, but work-stealing should be preferred in virtually every other situation.
Only reach for it if you understand the trade-offs and have a concrete reason.

---

## 5. Spawning parallel tasks

`spawn()` creates a **parallel** task: the executor schedules it independently and, on a
multi-threaded runtime, may run it on a different thread at the same time as the spawning
task. A task can migrate between threads between suspensions, but it never executes on
more than one thread at once. The returned `JoinHandle` is a `Future` — `co_await` it to
retrieve the result.

```cpp
#include <coro/coro.h>
#include <coro/runtime/runtime.h>

coro::Coro<int> fetch(int id) {
    co_return id * 10;
}

coro::Coro<void> run() {
    // Spawn both tasks before awaiting either — they may run in parallel.
    // .name() is optional; it tags the task for debugging.
    auto h1 = coro::build_task().name("fetch-1").spawn(fetch(1));
    auto h2 = coro::build_task().name("fetch-2").spawn(fetch(2));

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
    auto handle = coro::spawn(fetch(1));
    // handle goes out of scope here — task is cancelled and drained automatically
    co_return;
}
```

To let the task run without cancelling or waiting for it, call `detach()`:

```cpp
coro::spawn(fetch(1)).detach();  // fire and forget
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
    auto h = coro::spawn(worker(&local_data));
    co_return;  // local_data destroyed here; child may still be running
}
```

To close this gap, every `Coro<T>` implicitly tracks its spawned children and defers
frame destruction until they have drained. This catches most cases automatically — but
there is one gap: at `co_return`, locals are destroyed before the implicit drain wait
begins, so a child that references a local can still see dangling memory.

**The safe patterns are:**

1. **Pass owned data** — move values into the child instead of capturing references.
2. **Use `co_invoke` to create a nested scope** — `co_invoke` introduces a new coroutine
   scope inside the current one. Placing the `JoinHandle` (or `JoinSet`) inside that
   nested scope — while the referenced variable stays in the outer frame — guarantees
   that the outer frame outlives the spawned task. When `co_invoke` completes, the inner
   scope has fully drained; the outer locals are still alive throughout.

```cpp
coro::Coro<void> safe_example() {
    int shared_value = 42;  // lives in the outer coroutine frame

    co_await coro::co_invoke([&]() -> coro::Coro<void> {
        // JoinHandle lives here, in the inner scope.
        // shared_value is in the outer frame, so it outlives this entire co_invoke.
        auto h = coro::spawn(worker(&shared_value));
        co_await h;  // inner scope drains before co_invoke returns
    });
    // shared_value still alive here
}
```

`JoinSet` works equally well when you need to spawn multiple tasks — the point is not
which handle type you use, but that the handle lives in the inner scope while the
referenced variable lives in the outer one:

```cpp
co_await coro::co_invoke([&]() -> coro::Coro<void> {
    coro::JoinSet<void> js;
    js.spawn(worker_a(&shared_value));
    js.spawn(worker_b(&shared_value));
    co_await js.drain();
});
```

See the [Coroutine Scope design document](coroutine_scope.md) for a full explanation of
the implicit scope mechanism, its limits, and how it compares to Rust's `'static` bound.

---

## 6. Thread-safe communication and synchronization

When tasks run in parallel they need to safely exchange data or coordinate access to
shared state. This section covers the primitives designed for exactly that — reaching
for these instead of raw shared variables eliminates whole classes of data races.

### Channels

> *"Do not communicate by sharing memory; instead, share memory by communicating."*
> — Go team

Channels transfer ownership of data between tasks so only one task holds it at a time.
Three variants are provided:

| Variant | Producers | Consumers | Notes |
|---|---|---|---|
| `oneshot` | 1 | 1 | Single value; send is synchronous |
| `mpsc`    | N (cloneable sender) | 1 | Bounded buffer; backpressured send |
| `watch`   | 1 | N (cloneable receiver) | Last-value-wins; send never suspends |

> *NOTE*: Tokio also provides a broadcast channel that is not implemented yet, but likely will be added in the near future

#### RAII handles and disconnection

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

#### Error handling

All channel operations return `std::expected<T, ChannelError>` rather than throwing.
Call `.value()` to throw on error, or check the result explicitly.

```cpp
#include <coro/sync/oneshot.h>
#include <coro/sync/mpsc.h>
#include <coro/sync/watch.h>
```

#### oneshot — single value, one sender, one receiver

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
        }));

    auto result = co_await rx;      // std::expected<int, ChannelError>
    std::cout << result.value() << "\n";  // 42
    co_await h;
}
```

If the sender is dropped without calling `send()`, `co_await rx` returns
`std::unexpected(ChannelError::Closed)`.

#### mpsc — bounded queue, multiple producers, one consumer

Use `mpsc` for producer/consumer pipelines. The receiver satisfies `Stream<T>`,
so consume it with `next()` in a loop.

```cpp
coro::Coro<void> run() {
    co_await coro::co_invoke([&]() -> coro::Coro<void> {
        auto [tx, rx] = coro::mpsc::channel<int>(/*capacity=*/16);

        // Spawn two producers — each holds a copy of the sender.
        // When both complete, their senders are dropped, closing the channel.
        auto h1 = coro::spawn(coro::co_invoke(
            [tx = tx.clone()]() mutable -> coro::Coro<void> {
                for (int j = 0; j < 3; ++j)
                    co_await tx.send(j);
            }));
        auto h2 = coro::spawn(coro::co_invoke(
            [tx = std::move(tx)]() mutable -> coro::Coro<void> {
                for (int j = 0; j < 3; ++j)
                    co_await tx.send(10 + j);
            }));

        // Consume until all senders are dropped.
        while (auto v = co_await coro::next(rx))
            std::cout << *v << " ";
        std::cout << "\n";

        co_await h1;
        co_await h2;
    });
}
```

`send()` suspends the producer if the buffer is full, providing natural
backpressure. Use `trySend()` for a non-blocking attempt.

#### watch — last-value channel, one sender, many receivers

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
            }));

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

### Mutex

When you genuinely need multiple tasks to share mutable state rather than transfer
ownership, prefer `coro::Mutex` over `std::mutex`. `co_await mtx.lock()` suspends the
calling coroutine if the lock is already held — the executor thread is freed immediately
to run other tasks, so no thread is ever blocked waiting for the lock. The returned guard
releases the lock when it goes out of scope.

```cpp
#include <coro/sync/mutex.h>

coro::Coro<void> increment(coro::Mutex& mtx, int& counter) {
    auto guard = co_await mtx.lock();   // suspends if locked; no thread is blocked
    ++counter;
}                                       // guard destructor releases the lock

coro::Coro<void> run() {
    co_await coro::co_invoke([&]() -> coro::Coro<void> {
        coro::Mutex mtx;
        int counter = 0;
        auto h1 = coro::spawn(increment(mtx, counter));
        auto h2 = coro::spawn(increment(mtx, counter));
        co_await h1;
        co_await h2;
        std::cout << counter << "\n";  // 2
    });
}
```

It is safe to hold a `LockGuard` across a `co_await` point — the lock remains held while
the coroutine is suspended and any tasks waiting for it will themselves suspend without
blocking their threads. Keep critical sections short to minimise contention.

---

## 7. Fan-out with `JoinSet`

`JoinSet<T>` combines the best of `spawn` and channels: it fans out many parallel tasks
of the same type and delivers their results as a `Stream<T>` in completion order, with
built-in cancellation and error propagation. It is cleaner than managing individual
`JoinHandle`s when the task count is dynamic.

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
> `co_invoke` to keep the captures alive for the full duration of the `JoinSet` (see section 9).

---

## 8. Concurrent combinators

**Concurrent** means multiple operations can make progress without waiting for each other
to complete — no ordering guarantees on when each starts or finishes. **Parallel** extends
this to include simultaneous execution on multiple cores.

`spawn()` creates parallel tasks. The combinators in this section — `join`, `select`,
`timeout`, and `sleep_for` — are concurrent but not parallel: they drive multiple futures
within the current task, advancing one at a time. No two branches ever execute
simultaneously, even on a multi-threaded executor.

### Joining futures — `join()`

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

### Suspending — `sleep_for()`

`sleep_for()` suspends the current coroutine for a duration without blocking any worker
thread. Other tasks continue to run on the executor while a coroutine sleeps.

```cpp
#include <coro/sync/sleep.h>
#include <chrono>

coro::Coro<void> run() {
    using namespace std::chrono_literals;
    co_await coro::sleep_for(100ms);
}
```

### Racing futures — `select()`

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

#### Keeping a future alive across a losing branch — `coro::ref()`

By default `select()` takes its futures by value and cancels any branch that loses. Sometimes
you want the underlying work to keep running regardless of which branch wins — for example,
polling a long-running task while also watching for an external event.

`coro::ref(f)` wraps any lvalue future in a non-owning `FutureRef<F>` that delegates `poll()`
to `f` without taking ownership. When the `FutureRef` branch loses, only the wrapper is
discarded; the underlying future is untouched and can be used again.

```cpp
#include <coro/coro.h>
#include <coro/sync/select.h>
#include <coro/sync/sleep.h>
#include <chrono>
#include <iostream>

using namespace std::chrono_literals;

coro::Coro<void> run() {
    // Task is spawned once and reused across every select round.
    coro::JoinHandle<int> task = coro::spawn(long_running_work());

    while (true) {
        // sleep_for fires every 100 ms so we can do periodic work.
        // coro::ref(task) borrows the handle — the task keeps running if the
        // timer branch wins.
        auto sel = co_await coro::select(coro::ref(task), coro::sleep_for(100ms));

        if (std::holds_alternative<coro::SelectBranch<0, int>>(sel)) {
            // Task completed first.
            int result = std::get<coro::SelectBranch<0, int>>(sel).value;
            std::cout << "result: " << result << "\n";
            break;
        }

        // Timer fired — task still running.  Do periodic work, then loop.
        check_progress();
    }
}
```

**Key points:**

- `coro::ref()` only accepts lvalues. You must store the future in a named variable before
  wrapping it — `coro::ref(spawn(f()))` is a compile error because `spawn(f())` is an rvalue
  that would be immediately destroyed.
- `FutureRef` is non-copyable. It is meant to be created, passed to `select` (or similar),
  and discarded within the same expression.
- If the `coro::ref(f)` branch wins and delivers a result, the result is moved out of `f`.
  Do not await `f` again — it is logically consumed even though it was not moved.
- `FutureRef` is never `Cancellable`, regardless of whether `F` is. When the ref branch
  loses, `select()` simply discards the wrapper without calling `cancel()` on the underlying
  future — leaving it running for the next round. The tradeoff is that any waker `f`
  registered during the losing poll remains live and may fire spuriously, causing one extra
  scheduler wake-up. This is harmless: `poll()` is required to tolerate spurious calls.

### Timeouts — `timeout()`

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

## 9. Capturing-lambda pitfall and `co_invoke`

A capturing lambda that returns `Coro<T>` has a subtle use-after-free when used as an
rvalue. The compiler lowers the lambda to an anonymous struct; `operator()` — being a member
function — captures `this` into the coroutine frame. The struct is a temporary and is
destroyed at the end of the full expression, before the coroutine is ever polled.

See [Library Usage Guidelines](guidelines.md) for a full detailed explaination of this pitfall and related patterns to avoid.

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
auto handle = spawn(co_invoke([x]() -> Coro<void> { ... }));
```

`co_invoke` also works with `CoroStream<T>` lambdas.

## 10. Running blocking code with `spawn_blocking`

Some work is inherently blocking — legacy library calls, CPU-intensive computation,
or synchronous file I/O. Calling these directly from a coroutine would park an executor
worker thread for the duration, starving all other tasks on that thread.

`spawn_blocking()` submits the callable to a dedicated `BlockingPool` thread. The executor
thread is released immediately and can pick up other tasks while the blocking work runs.
The result is returned as a `BlockingHandle<T>`, which is a `Future` you can `co_await`.

```cpp
#include <coro/task/spawn_blocking.h>
#include <coro/runtime/runtime.h>
#include <thread>
#include <chrono>

coro::Coro<void> run() {
    using namespace std::chrono_literals;

    // The executor thread is free while this sleeps on the blocking pool.
    int result = co_await coro::spawn_blocking([] {
        std::this_thread::sleep_for(100ms);  // blocking — fine on the pool
        return 42;
    });
    std::cout << result << "\n";  // 42
}

int main() {
    coro::Runtime rt;
    rt.block_on(run());
}
```

Exception propagation works the same as with any other future:

```cpp
co_await coro::spawn_blocking([]() -> int {
    throw std::runtime_error("oops");
});  // exception propagates to the awaiting coroutine
```

**Ownership rules:** the callable must own all its data — do not capture references or
pointers into the spawning coroutine's locals. The blocking thread may outlive the
spawning coroutine if the `BlockingHandle` is dropped without awaiting.

The `BlockingPool` grows lazily up to a configurable cap (default: 512 threads) and
shrinks threads back after a keep-alive idle period.

---

## 11. Async I/O

The runtime integrates a libuv event loop so that I/O suspends a coroutine without
blocking any worker thread. Several I/O abstractions are provided, including `File` for
async file I/O, `TcpStream` for raw TCP, and `WsStream`/`WsListener` for WebSocket.

All I/O operations require a running `Runtime` — they are not usable from plain
synchronous code.

### File — async file I/O

`File` provides async read and write operations backed by libuv's thread pool. From the
coroutine's perspective it behaves identically to network I/O — the coroutine suspends
and the executor thread is freed while the operation runs in the background.

```cpp
#include <coro/io/file.h>

coro::Coro<void> run() {
    // Open for reading
    auto f = co_await coro::File::open("data.txt", coro::FileMode::Read);
    auto [n, buf] = co_await f.read(std::vector<std::byte>(4096));
    buf.resize(n);

    // Open for writing (create or truncate)
    auto out = co_await coro::File::open(
        "output.txt", coro::FileMode::Write | coro::FileMode::Create | coro::FileMode::Truncate);
    co_await out.write(std::move(buf));
}
```

`read()` and `write()` follow the same owned-buffer contract as `TcpStream` — the buffer
is moved in and returned with the byte count, so lifetime is tied to the coroutine frame.
Pass `exact = true` to loop until the full buffer is filled or written. Random-access
variants `read_at()` and `write_at()` accept an additional byte offset.

### TcpStream — raw TCP

`TcpStream` is a connected, async TCP socket. Connect with `co_await
TcpStream::connect(host, port)`, then read and write using owned buffers.

```cpp
#include <coro/io/tcp_stream.h>
#include <coro/runtime/runtime.h>
#include <iostream>
#include <string>
#include <vector>
#include <cstddef>

coro::Coro<void> run() {
    coro::TcpStream tcp = co_await coro::TcpStream::connect("example.com", 80);

    // Write: pass buffer by value; it is returned after the write completes.
    auto [written, req] = co_await tcp.write(
        std::string("GET / HTTP/1.0\r\nHost: example.com\r\n\r\n"));

    // Read: pass buffer by value; returned with the byte count.
    // Returns 0 bytes on EOF.
    while (true) {
        auto [n, buf] = co_await tcp.read(std::vector<std::byte>(4096));
        if (n == 0) break;
        std::cout << std::string_view(reinterpret_cast<const char*>(buf.data()), n);
    }
}

int main() {
    coro::Runtime rt;
    rt.block_on(run());
}
```

`read()` and `write()` accept any type satisfying `ByteBuffer` — `std::string`,
`std::vector<std::byte>`, `std::array<std::byte, N>`, etc. The buffer is moved into
the operation and returned with the result, so its lifetime is tied to the coroutine
frame and dangling-pointer bugs are impossible at the type-system level.

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

- Read the [Library Usage Guidelines](guidelines.md) for rules on writing correct, safe,
  and idiomatic code with this library.
- Read the [Internal Design Details](architecture.md) for a deeper explanation of the
  `Future`/`Stream` model, the executor architecture, and the coroutine scope lifetime
  guarantees.
- Browse the [Examples](examples.md) for self-contained programs covering common patterns.
