# CLAUDE.md

## Directory Structure

```
include/coro/          # Public headers — see doc/module_structure.md for placement rules
  coro.h               #   Coro<T> coroutine return type
  coro_stream.h        #   CoroStream<T> async generator return type
  future.h             #   Future concept + NextFuture adapter
  stream.h             #   Stream concept + next() helper
  runtime/             #   Runtime, Executor (tokio::runtime analogue)
  task/                #   JoinHandle, SpawnBuilder (tokio::task analogue)
  sync/                #   JoinSet, StreamHandle, channels, CancellationToken (tokio::sync analogue)
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

### Never use capturing lambda coroutines; pass all data as explicit arguments

A capturing lambda coroutine — one with any non-empty capture list — is always unsafe, even
with value captures (`[=]`). The C++ compiler lowers a lambda to a struct whose `operator()()`
is a member function. Calling `lambda()` to produce the `Coro` does not move the captured data
into the coroutine frame; it calls `operator()` on `this` (the lambda object). The coroutine
frame therefore holds an implicit pointer to the lambda closure. When the lambda is a temporary
(the common case: `block_on([]{ ... }())`), the closure is destroyed at the end of the
full expression — long before the first `co_await` resumes — leaving the frame with a dangling
`this` pointer and undefined behaviour at the first suspension point.

This applies equally to `[&]`, `[=]`, and named captures. The only safe pattern is an empty
capture list `[]` with all external data passed as explicit parameters:

```cpp
// WRONG — closure destroyed before first co_await; UB even with [=]:
rt.block_on([&foo, bar]() -> Coro<void> {
    co_await something();   // foo, bar are dangled
    use(foo, bar);
}());

// CORRECT — foo and bar are parameters; they live in the coroutine frame:
rt.block_on([](Foo& foo, Bar bar) -> Coro<void> {
    co_await something();   // safe
    use(foo, bar);
}(foo, bar));
```

The same rule applies to `spawn()`, `with_context()`, and any other site that accepts a
`Future` produced by a lambda coroutine.

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

### Guidelines titles are actionable; descriptions may reference internals

When adding or editing rules in `doc/guidelines.md`, the rule title must describe an
actionable item from the end user's perspective — what to do or avoid, expressed in terms
of the API they call, not the internal mechanism that makes it unsafe. The description
body may go into internal implementation details (coroutine frame lifetime, `this` pointer
capture, drain mechanics, etc.) where it helps the user understand *why* the rule exists.

### Wrap capturing lambdas in `co_invoke` before passing to `coro::spawn`

`coro::spawn` expects a `Future` or `Stream`, not a callable. A plain lambda is not a
`Future` — passing one directly compiles but produces undefined behaviour at runtime
(the lambda is treated as an already-constructed future object, causing a crash).

Always invoke the lambda first so that `spawn` receives the resulting coroutine:

```cpp
// WRONG — passes the lambda itself as if it were a Future:
coro::spawn([&foo]() -> coro::Coro<void> { ... });

// CORRECT — call the lambda (or use co_invoke) to produce the Coro:
coro::spawn(co_invoke([&foo]() -> coro::Coro<void> { ... }));
// or equivalently:
coro::spawn([&foo]() -> coro::Coro<void> { ... }());
```

### Use the owned-buffer API for async I/O; do not pass spans or raw pointers

`File`, `TcpStream`, and similar async I/O types accept any type satisfying the
`ByteBuffer` concept (`std::string`, `std::vector<std::byte>`, `std::array<std::byte, N>`,
etc.) by **value**. The buffer is moved into the operation and returned with the result,
so its lifetime is tied to the coroutine frame — the type system makes dangling-pointer
bugs impossible.

```cpp
// CORRECT — buffer moved in, returned with byte count:
auto [n, buf] = co_await file.read(std::vector<std::byte>(4096));
buf.resize(n);  // buf now contains exactly the bytes that were read

auto [written, buf2] = co_await file.write(std::move(buf));

// std::string also satisfies ByteBuffer:
auto [n2, text] = co_await file.read(std::string(4096, '\0'));
text.resize(n2);
```

Never work around this by constructing a span and passing it directly — the `read()`
and `write()` methods no longer accept spans. If you are adding a new async I/O method,
use a `ByteBuffer` template parameter rather than `std::span`.

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