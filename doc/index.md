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
timer events. The default executor is **work-stealing**: tasks are distributed across worker
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

The following is a common pattern in connection-based server communication: poll two
redundant servers for a long-running result, send keepalives while waiting to detect
disconnections, and enforce an overall deadline. The thread-and-callback version of this
requires four things that have nothing to do with the logic: a state machine to track
which phase you're in, a mutex protecting the shared result, a dedicated timer thread for
keepalives, and a cancellation flag carefully threaded through every layer — with the
constant risk that a blocking call somewhere never checks it. With coroutines, the
structure maps directly to the intent. The three concurrent concerns (`poll_status`,
`poll_status`, `keepalive`) are just three branches of a `select`. The deadline is one
`timeout` wrapper. Cancellation is intrinsic — every `co_await` is already an exit point.

```cpp
#include <coro/coro.h>
#include <coro/runtime/runtime.h>
#include <coro/sync/join.h>
#include <coro/sync/select.h>
#include <coro/sync/sleep.h>
#include <coro/sync/timeout.h>
#include <iostream>

// Placeholder types — substitute coro::TcpStream, a gRPC stub, WsStream, etc.
struct Connection;
struct Result { bool ready; std::string value; };

coro::Coro<Result> poll_status(Connection& c, int request_id);
coro::Coro<void>   ping(Connection& c);

// Waits 500ms, then pings both connections concurrently with a 500ms deadline.
// Throws if either server is unreachable or does not respond in time.
coro::Coro<void> keepalive(Connection& primary, Connection& backup) {
    using namespace std::chrono_literals;
    co_await coro::sleep_for(500ms);
    auto r = co_await coro::timeout(500ms, coro::join(ping(primary), ping(backup)));
    if (r.index() != 0)
        throw std::runtime_error("keepalive timed out");
}

// Polls two redundant servers until one delivers a ready result.
coro::Coro<Result> poll_until_ready(Connection& primary, Connection& backup, int id) {
    using namespace std::chrono_literals;

    while (true) {
        // Race both servers against a keepalive tick.
        // select() drives all three concurrently; first to complete wins.
        auto sel = co_await coro::select(
            poll_status(primary, id),
            poll_status(backup,  id),
            keepalive(primary, backup)  // branch 2: fires after 500ms of silence
        );

        if (sel.index() == 0 || sel.index() == 1) {
            // A server responded — retrieve whichever answered first.
            Result& r = sel.index() == 0
                ? std::get<0>(sel).value
                : std::get<1>(sel).value;

            if (r.ready)
                co_return r;

            // Not ready yet — pause briefly before polling again.
            co_await coro::sleep_for(100ms);
        }
        // sel.index() == 2: keepalive fired, connections verified — loop and poll again.
        // A ping failure or timeout throws, propagating out through select before reaching here.
    }
}

coro::Coro<void> run(Connection& primary, Connection& backup) {
    using namespace std::chrono_literals;

    // Enforce an overall deadline across the entire poll loop.
    // When it fires, both in-flight requests and any pending keepalive drain cleanly.
    auto outcome = co_await coro::timeout(30s, poll_until_ready(primary, backup, 42));

    if (outcome.index() == 0)
        std::cout << std::get<0>(outcome).value.value << "\n";
    else
        std::cout << "timed out\n";
}

int main() {
    coro::Runtime rt;
    Connection primary, backup;
    rt.block_on(run(primary, backup));
}
```

Structured cancellation composes for free at any granularity: spawn `run()` as a task and
drop the `JoinHandle` at any point — every `co_await` in the entire tree (poll loop,
keepalive, timeout) becomes a clean exit point automatically. No tokens to thread through
every layer, no risk of getting stuck at a blocking call that never checks the flag.
For cases where the canceller lives outside the task hierarchy, cooperative cancellation
via `CancellationToken` is coming soon.

## Where to go next

- [Getting Started](getting_started.md) — step-by-step introductory tour of all major features with examples.
- [Library Usage Guidelines](guidelines.md) — [C++ Core Guidelines](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines) style rules for writing correct, safe, and idiomatic code with this library
- [Internal Design Details](architecture.md) — architecture, design decisions, and implementation reference.
