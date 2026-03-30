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
    auto h1 = coro::spawn(fetch(1)).submit();
    auto h2 = coro::spawn(fetch(2)).submit();

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

---

## 4. Fan-out with `JoinSet`

When you need to spawn many tasks of the same type and collect their results, `JoinSet<T>`
is cleaner than managing individual `JoinHandle`s. It satisfies `Stream<T>`, so results
arrive in completion order and compose naturally with `next()`.

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

> **Note:** `JoinSet::spawn()` uses `co_invoke` internally to schedule tasks, so it must be
> called from within a `Runtime::block_on()` context. Always wrap fan-out work in `co_invoke`
> to keep lambda captures alive for the full duration (see section 8).

---

## 5. Async generators

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

## 6. Racing futures with `select`

`select()` races two or more futures and returns as soon as one completes.
The result is a `std::variant` of `SelectBranch<N, T>` values identifying which
branch won and carrying its result.

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

## 7. Timeouts

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

## 8. Capturing-lambda pitfall and `co_invoke`

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

## Next steps

- Read the **Design Docs** for a deeper explanation of the `Future`/`Stream` model,
  the executor architecture, and the coroutine scope lifetime guarantees.
- Browse the **API Reference** for the full class and function documentation.
