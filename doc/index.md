# coro

A C++20 coroutines library for multi-threaded asynchronous task synchronization and I/O,
heavily inspired by Rust's async model and the Tokio runtime.

## What are coroutines

Readers familiar with `async`/`await` in Python, JavaScript, Rust, Kotlin, or Go's goroutines
will recognize the pattern immediately. This library brings the same model to C++ using the
coroutine support introduced in C++20.

A coroutine is a function that can **suspend itself mid-execution** — pausing at a `co_await`
expression while it waits for some event — and then **resume from exactly where it left off**
when that event fires. From the caller's perspective it looks and reads like ordinary sequential
code where the suspension and resumption happen transparently and efficiently under the hood.
The result is concurrent code whose execution flow is as easy to reason about as sequential code.

```cpp
coro::Coro<std::string> fetch(std::string url) {
    TcpStream conn = co_await TcpStream::connect(url, 80);  // suspends here
    co_await conn.write(make_request(url));                  // suspends here
    auto response = co_await conn.read(buffer);              // suspends here
    co_return parse(response);
}
```

### Coroutines vs. threads

The crucial difference from OS threads is what happens while a coroutine waits:

| | OS Thread | Coroutine |
|---|---|---|
| Waiting for I/O | Thread **blocks** — OS parks it, stack stays allocated | Coroutine **suspends** — returns control to the runtime, stack frame is saved on the heap |
| Switching cost | OS kernel context switch (~microseconds) | Function call (~nanoseconds) |
| Memory per unit | ~1–8 MB stack (OS default) | A few hundred bytes on the heap |
| Scheduling | Preemptive — OS can interrupt at any time | Cooperative — suspends only at `co_await` points |
| Shared state | Needs locks for shared data | Single-threaded coroutines need no locks; multi-threaded ones still do |

Because each suspended coroutine costs only its heap-allocated frame rather than a full OS
stack, a single process can run **hundreds of thousands of concurrent coroutines** with the
same resources that would support only a few thousand threads.

### The runtime and executor

Coroutines do not run themselves — they need a **runtime** to drive them. The runtime's
**executor** runs on one or more OS threads and is responsible for scheduling and resuming
coroutines on those threads when the events they are waiting on fire. When all coroutines
are suspended waiting for I/O or timers, the executor sleeps and the I/O reactor wakes it
when something is ready.

This library's `Runtime` manages the executor and a libuv I/O reactor for network and
timer events. The executor is **work-stealing**: tasks are distributed across worker
threads automatically, and idle workers steal tasks from busy workers before going to
sleep — keeping all cores utilized without manual load balancing.

`Runtime::block_on()` is the entry point from synchronous code into the async world. It
runs a single root coroutine to completion on the runtime, blocking the calling thread
until it finishes. Everything else — spawning tasks, awaiting I/O, sleeping — happens
from inside that root coroutine.

```cpp
coro::Coro<int> async_main(int argc, char* argv[]) {
    // Inside block_on: co_await, spawn(), I/O, timers are all available here.
    ...
    co_return 0;
}

int main(int argc, char* argv[]) {
    coro::Runtime rt;
    return rt.block_on(async_main(argc, argv));
}
```
### Tasks

A **task** is a coroutine scheduled on the executor — the async equivalent of an OS
thread. Work within a single task is always sequential: each `co_await` completes before
the next line runs. Work in *different* tasks can run in parallel: on a multi-threaded
executor, two tasks may execute simultaneously on different worker threads, just as two OS
threads would. Spawning a task is therefore how you introduce true parallelism, not just
concurrency.

### Structured concurrency

Spawning background tasks without tracking them leads to the same problems as
fire-and-forget threads: leaked work, swallowed errors, and lifetimes that are hard to
reason about. This library encourages **structured concurrency** — every spawned task is
owned by a handle that lives in the enclosing scope, so tasks cannot outlive the code
that created them.

**`JoinHandle<T>`** — similar to the `std::future` returned by `std::async`, owns a single
background task scheduled on the executor. The task runs concurrently with the spawning
coroutine. Awaiting the handle waits for completion and retrieves the result. Dropping the
handle without awaiting it cancels the task.

```cpp
auto handle = coro::spawn(compute(42)).submit();
// ... do other work while compute() runs in the background
int result = co_await handle;
```

**`JoinSet<T>`** — owns a dynamic collection of tasks all returning the same type.
Tasks can be spawned at any time. Results are delivered in completion order.
Dropping the `JoinSet` cancels all outstanding tasks.

```cpp
JoinSet<int> js;
for (int i = 0; i < 10; ++i)
    js.spawn(compute(i));

// Collect results as they complete (order not guaranteed):
int sum = 0;
while (auto result = co_await js.next())
    sum += *result;
```

**`join()` / `select()`** — compose a fixed, heterogeneous set of futures within the
current task, without spawning new tasks on the executor. `join()` runs all futures
concurrently and returns a tuple of their results when every one has completed.
`select()` returns as soon as the first future completes, cancelling the rest.

```cpp
// Wait for both to finish:
auto [a, b] = co_await coro::join(fetch("url1"), fetch("url2"));

// Return whichever finishes first:
auto winner = co_await coro::select(fetch("url1"), fetch("url2"));
```

## Key features

- **`Coro<T>`** — async function return type; compose with `co_await` like any other future.
- **`CoroStream<T>`** — async generator; emit values with `co_yield`, consume with `next()`.
- **`spawn()` / `JoinHandle`** — schedule background tasks; await their results or cancel them.
- **`JoinSet<T>`** — fan out many tasks of the same type; collect results in completion order.
- **`join()`** — run a fixed set of heterogeneous futures concurrently; wait for all.
- **`select()`** — race futures; return the result of whichever completes first.
- **`timeout()`** — race any future against a deadline.
- **`sleep_for()`** — suspend a coroutine for a duration without blocking a thread.
- **Channels** — `oneshot`, `mpsc`, and `watch` channels for inter-task communication.
- **`spawn_blocking()`** — run blocking code on a dedicated thread pool without starving the executor.
- **Multi-threaded work-stealing executor** — tasks are distributed across worker threads
  automatically; idle workers steal from busy workers before parking.

## Quick example

`CoroStream<T>` is an async generator — a coroutine that produces a sequence of values
on demand, suspending between each one. This example defines an infinite Fibonacci
sequence as a stream and prints the first 10 values from four sequences with different
starting points as a CSV table.

```cpp
#include <coro/coro.h>
#include <coro/coro_stream.h>
#include <coro/runtime/runtime.h>
#include <iostream>

// An async generator that yields an infinite Fibonacci sequence.
// co_yield suspends the coroutine and delivers a value to the caller;
// execution resumes from after the co_yield on the next call to next().
coro::CoroStream<int> fibonacci(int x0, int x1) {
    co_yield x0;
    co_yield x1;
    while (true) {
        int xn = x0 + x1;
        co_yield xn;
        x0 = x1;
        x1 = xn;
    }
}

// An async coroutine that drives four streams concurrently.
// CoroStream<T> is lazy — fibonacci() does no work until next() is called.
coro::Coro<void> run() {
    coro::CoroStream<int> fib0 = fibonacci(0, 1);  // 0, 1, 1, 2, 3, 5 ...
    coro::CoroStream<int> fib1 = fibonacci(1, 2);  // 1, 2, 3, 5, 8 ...
    coro::CoroStream<int> fib2 = fibonacci(2, 3);  // 2, 3, 5, 8, 13 ...
    coro::CoroStream<int> fib3 = fibonacci(3, 4);  // 3, 4, 7, 11, 18 ...

    for (size_t i = 0; i < 10; ++i) {
        // co_await coro::next(stream) advances the stream by one step,
        // suspending until the next value is ready.
        std::cout
            << (co_await coro::next(fib0)).value() << ","
            << (co_await coro::next(fib1)).value() << ","
            << (co_await coro::next(fib2)).value() << ","
            << (co_await coro::next(fib3)).value() << "\n";
    }
}

int main() {
    coro::Runtime rt;
    rt.block_on(run());  // runs the coroutine to completion on the runtime
}
```

Output:
```
0,1,2,3
1,2,3,4
1,3,5,7
2,5,8,11
3,8,13,18
5,13,21,29
8,21,34,47
13,34,55,76
21,55,89,123
34,89,144,199
```

## Where to go next

- [Getting Started](getting_started.md) — step-by-step guide with examples for all major features.
- [Internal Design Details](task_and_executor.md) — architecture and design documents.
