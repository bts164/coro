# CLAUDE.md

This project is going to implement a C++ 20 coroutines library for multi-threaded asynchronous task synchronization and I/O.
It should take inspiration from coroutines in python, goroutines in golang, but expecially it is going to mimick as close
as possible rust's async model and Tokio implementation of it.

## Directory Structure

```
include/coro/          # Public headers — see doc/module_structure.md for placement rules
  coro.h               #   Coro<T> coroutine return type
  coro_stream.h        #   CoroStream<T> async generator return type
  future.h             #   Future concept + NextFuture adapter
  stream.h             #   Stream concept + next() helper
  runtime/             #   Runtime, Executor (tokio::runtime analogue)
  task/                #   JoinHandle, SpawnBuilder (tokio::task analogue)
  sync/                #   Synchronize, StreamHandle, CancellationToken (tokio::sync analogue)
  detail/              #   Internal plumbing + low-level extension points (PollResult, Waker, Context)
src/                   # Implementation source files
test/                  # gtest unit tests
doc/                   # Design documents and feature specs (Markdown)
```

When adding a new header, consult `doc/module_structure.md` to choose the right subfolder.

## Build / Test

Do not build or test the library. The user will manually build, test and then report the results.

Primary target: GCC on Linux (Ubuntu). Clang and MSVC are secondary targets — avoid GCC-specific extensions that would knowingly break them.

Requires C++20. Dependencies are managed with Conan; install them before configuring:

## Core Abstractions

| Abstraction | Description |
|---|---|
| `Future` | C++20 concept mirroring Rust's `Future` trait — a value that produces a result asynchronously |
| `Stream` | C++20 concept mirroring Rust's `Stream` trait — an asynchronous sequence of values |
| `Task` | An owned, scheduled unit of async work (analogous to Rust's `tokio::task`) |
| `Waker` | Notifies the executor that a `Future` is ready to make progress |
| `Executor` / `Runtime` | Drives `Task`s to completion; manages the thread pool and I/O reactor |

The underlying I/O reactor is implemented using **libuv** unless there is a compelling reason to use another library.

The `Executor` should be designed around a pluggable scheduling model. The interface should not assume any particular scheduler. The first implementation is a **single-threaded executor** (simple, deterministic, useful for testing). The second is a **multi-threaded work-stealing scheduler** (similar to Tokio's).

## Conventions

### Document potential or known race conditions with comments

Briefly point out potential or known race conditions inline in the code with comments. Even if you're not 100% sure, err on the side of caution and add
a comment pointing out the potential race condition.

### Care must be taken if spawned futures are not self-contained

`runtime.spawn()` and the free `spawn()` function accept any `Future` or `Stream`, but the
C++ type system cannot enforce that the submitted future does not borrow from the spawning
context (unlike Rust's `'static` bound).
**Spawned futures can still capture references or pointers as arguments, but with some restrictions**,
Otherwise the spawning coroutine may be destroyed while the task is still running resulting
in a use-after-free memory error.

Use `Synchronize` instead when child tasks need to reference data owned by the parent. All
coroutines inherently act like a Synchronize scope eliminating the user from having to explicitly
use `Synchronize`

### Prefer mutexes over atomics

Prefer `std::mutex` over `std::atomic` for synchronizing shared state between threads.
Atomics are easy to get subtly wrong — missed-wakeup races, incorrect ordering
annotations, and forgotten re-checks after storing a value are common failure modes that
are hard to spot in review. A mutex makes the critical section explicit and the protocol
obvious. This follows the same guidance in the Google C++ Style Guide: use atomics only
when profiling demonstrates a real performance need and you can justify the added
complexity. Low-level executor scheduling state (e.g. `SchedulingState` CAS transitions)
is a justified exception — everything else should default to a mutex.

### Prefer Mermaid diagrams over ASCII art

Use Mermaid (`\`\`\`mermaid`) for diagrams in documentation. Mermaid is preferred for
sequence diagrams, state machines, flowcharts, and class diagrams. Fall back to ASCII
only when no Mermaid chart type fits naturally and ASCII would produce a simpler or
clearer result.

### [[nodiscard]] on Future-returning functions

All functions that return a `Future`, `Stream`, `JoinHandle`, `StreamHandle`, or builder
types (`SpawnBuilder`, `StreamSpawnBuilder`) must be marked `[[nodiscard]]`. Discarding
these silently cancels the work, which is almost always a bug.

## Architecture

This project is going to be developed in the following iterative, test driven, three step process

### Phase 1: Gather requirements and document design

This phase should consist of first gathering requirements for what new feature we should be working on,
outlining a high level design for how it will be implemented, and then documenting it in `doc/<feature>.md`.
Documentation can include text descriptions, mermaid class/state/sequence diagrams, and small code snippets
showing how the feature will be used when complete. This phase should not focus on implementation details.

### Phase 2: Stub out code and tests for the feature

In this phase we begin writing and compiling actual code. Write gtest unit tests that must compile, but
do not need to pass or run yet. Unimplemented classes and functions should have stub implementations
(empty or minimal bodies) in their real source files — not separate mock classes. Where it makes sense
to test a feature in isolation, gtest `MOCK_METHOD` mocks are appropriate. Once all tests compile,
proceed to Phase 3.

### Phase 3: Implementation and testing

This phase we will implement the feature and use the developed tests to confirm they work and also
that we have not broken any existing functionality