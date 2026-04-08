# Library Overview

A C++20 coroutines library for multi-threaded asynchronous task synchronization and I/O,
modelled closely on Rust's async model and the Tokio runtime.

---

## Core Model

The library follows Rust's poll-based async model. Every async operation is a **Future** —
a value with a `poll()` method that returns one of four states:

| State | Meaning |
|---|---|
| `PollReady(value)` | Completed with a value |
| `PollError(exception)` | Completed with an exception |
| `PollPending` | Not ready; waker registered |
| `PollDropped` | Cancelled and fully drained |

A **Waker** notifies the executor when a future is ready to make progress. The **Runtime**
owns the executor, the I/O reactor (`IoService`), and the blocking thread pool
(`BlockingPool`). Coroutines are expressed as `Coro<T>`, async generators as
`CoroStream<T>`.

---

## What Is Implemented

### Runtime & Scheduling
- **Single-threaded executor** — deterministic, used for testing
- **Work-sharing executor** — shared injection queue, multiple worker threads
- **Work-stealing executor** — per-worker `WorkStealingDeque` with CAS-based
  `SchedulingState` transitions; Tokio-style injection budget enforcement
- `Runtime` selects executor by thread count at construction; `block_on()` drives the
  top-level coroutine to completion

### I/O Reactor
- **`IoService`** — owns a `uv_loop_t` on a dedicated I/O thread; worker threads submit
  `IoRequest` commands via a thread-safe queue and a `uv_async_t` doorbell
- **`SleepFuture` / `sleep_for()`** — millisecond-resolution one-shot timers via libuv
- **`TcpStream`** — async connect, read, write
- **`WsStream` / `WsListener`** — async WebSocket client and server via libwebsockets
  sharing the libuv event loop; full and partial frame modes, TLS, subprotocol negotiation

### Task Primitives
- **`spawn()` / `SpawnBuilder`** — schedule a `Coro<T>` on the runtime; returns a
  `JoinHandle<T>`; `.detach()` for fire-and-forget
- **`co_invoke()`** — run a lambda as a coroutine in the current scope
- **`spawn_blocking()`** — run a blocking callable on a dedicated `BlockingPool` thread;
  returns `BlockingHandle<T>`; fire-and-forget on drop

### Structured Concurrency
- **Coroutine scope** — every coroutine implicitly tracks child tasks; frame destruction
  is deferred until all non-detached children return `PollDropped`
- **`JoinSet<T>`** — spawn homogeneous child tasks, collect results in completion order
  via `next()`, or discard via `drain()`; satisfies `Stream<T>` for non-void T;
  cancel-on-drop with scope-guaranteed drain
- **`Synchronize`** — deprecated; use `co_invoke` + `JoinSet::drain()` instead

### Combinators
- **`select(f1, f2, ...)`** — first-ready-wins; cancels and drains losing branches before
  delivering result; `SelectBranch<N, T>` tagging handles homogeneous types
- **`timeout(duration, future)`** — wraps `select` + `sleep_for`
- **`next(stream)`** — advance any `Stream<T>` by one item

### Channels (`include/coro/sync/`)
- **`oneshot`** — single value; synchronous send, async receive
- **`mpsc`** — bounded ring buffer; cloneable sender, single `Stream<T>` receiver;
  intrusive waiter nodes; zero-copy direct-handoff paths
- **`watch`** — latest-value; synchronous overwrite; `changed()` + `borrow()` (returns
  `BorrowGuard<T>` holding a shared read lock); cloneable receiver

All channels use `std::expected<T, ChannelError>` for fallible operations; `trySend`
returns `std::expected<void, TrySendError<T>>` so the caller recovers unsent values on
failure.

---

## Key Design Decisions

**Cancellation via `PollDropped`, not drop.** C++ coroutine frames must be resumed to run
destructors; cancellation is delivered as a poll signal so the task can drain its children
before the frame is freed.

**Implicit structured concurrency.** The `t_current_coro` thread-local lets `JoinHandle`
destructors register pending children with the enclosing coroutine automatically — no
explicit scope object required in the common case.

**Mutex over atomics.** Shared state uses `std::mutex` by default. Atomics are reserved
for the `SchedulingState` CAS machine and a handful of documented cross-thread waker
stores where a mutex would introduce lock-ordering issues.

**`IoService` command pattern.** Worker threads never touch libuv directly. Each I/O
operation is an `IoRequest` subclass executed on the I/O thread; `IoService` is
completely ignorant of concrete request types, keeping each I/O subsystem self-contained.

**`std::expected` error policy.** Fallible operations return `std::expected<T, E>` rather
than throwing. `.value()` is the exception-throwing escape hatch for callers that prefer it.

---

## Header Layout

```
include/coro/
  coro.h               Coro<T> coroutine return type
  coro_stream.h        CoroStream<T> async generator
  future.h             Future concept + NextFuture adapter
  stream.h             Stream concept + next() helper
  runtime/             Runtime, Executor, IoService, BlockingPool
  task/                JoinHandle, SpawnBuilder, spawn_blocking
  sync/                JoinSet, Synchronize (deprecated), channels, sleep
  io/                  TcpStream, WsStream, WsListener
  detail/              PollResult, Waker, Context, internal plumbing
```

---

## Planned Work (Roadmap)

| Item | Notes |
|---|---|
| Migrate to `std::expected` returns | `JoinHandle`, `timeout`, other fallible futures |
| C++20 compat shim | `coro::detail::expected` aliasing `std::expected` or `tl::expected` |
| Remove `Synchronize` | Headers, tests, and doc references |
| `broadcast` channel | All-values multi-consumer; lower priority |
| Async `Mutex<T>` | Task-suspending lock; FIFO fairness |
| Stream combinators | `map`, `filter`, `take`, `chain`, `flat_map` |
| `AbortHandle` | Decouple cancellation from result collection |
| libuv I/O: `TcpListener`, `UdpSocket`, `File`, DNS | Remaining I/O surface |
| Structured logging | Pluggable sink, zero-cost when disabled, task identity |
| Cancellation model doc | C++ vs. Rust deep-dive (`doc/cancellation_model.md`) |
| Cancellation propagation tests | End-to-end `select`/`timeout` + `CoroutineScope` |
