# coro

A C++20 coroutines library for multi-threaded asynchronous task synchronization and I/O,
inspired by Rust's async model and the Tokio runtime.

## Key features

- **`Coro<T>`** — async function return type; compose with `co_await` like any other future.
- **`CoroStream<T>`** — async generator; emit values with `co_yield`, consume with `next()`.
- **`spawn()` / `JoinHandle`** — schedule background tasks; await their results or cancel them.
- **`JoinSet<T>`** — fan out many tasks of the same type; collect results in completion order.
- **`join()`** — run a fixed set of heterogeneous futures concurrently; wait for all.
- **`select()`** — race futures; return the result of whichever completes first.
- **`timeout()`** — race any future against a deadline.
- **`sleep_for()`** — suspend a coroutine for a duration without blocking a thread.
- **Multi-threaded work-stealing executor** — tasks are distributed across worker threads
  automatically; idle workers steal from busy workers before parking.

## Quick example

```cpp
#include <coro/coro.h>
#include <coro/runtime/runtime.h>
#include <iostream>

coro::Coro<int> compute(int x) { co_return x * x; }

coro::Coro<void> run() {
    // Run two coroutines concurrently and wait for both.
    auto [a, b] = co_await coro::join(compute(3), compute(4));
    std::cout << a << " " << b << "\n";  // 9 16
}

int main() {
    coro::Runtime rt;
    rt.block_on(run());
}
```

## Documentation

- [Getting Started](getting_started.md) — step-by-step guide with examples for all major features.
- [API Reference](coro/classes.md) — generated documentation for all public types.
- [Internal Design Details](task_and_executor.md) — architecture and design documents.
