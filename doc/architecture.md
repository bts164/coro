# Architecture

A detailed reference for the library's design, internals, and implementation patterns.
For a compact summary see `doc/overview.md`.

---

## Table of Contents

1. [Motivation and Goals](#motivation-and-goals)
2. [Core Abstractions](#core-abstractions)
3. [Poll Model](#poll-model)
4. [Runtime and Executors](#runtime-and-executors)
5. [Coroutine Types](#coroutine-types)
6. [Task Lifecycle](#task-lifecycle)
7. [Cancellation and Structured Concurrency](#cancellation-and-structured-concurrency)
8. [I/O Reactor](#io-reactor)
9. [Blocking Thread Pool](#blocking-thread-pool)
10. [Channels](#channels)
11. [Combinators](#combinators)
12. [Error Handling Policy](#error-handling-policy)
13. [Threading and Synchronization](#threading-and-synchronization)
14. [Header Layout](#header-layout)

---

## Motivation and Goals

The library brings Rust's async/await model to C++20 without requiring the borrow checker.
It takes direct inspiration from [Tokio](https://tokio.rs/): poll-based futures, a
work-stealing executor, structured concurrency via scoped tasks, and first-class async I/O
through an event loop.

Where Tokio enforces safety at compile time with `'static + Send` bounds, this library
enforces equivalent guarantees at runtime through the coroutine scope mechanism. The trade
is a compile-time guarantee for a runtime one — unavoidable given C++'s object model.

Primary target is GCC on Linux (Ubuntu); Clang and MSVC are secondary. Requires C++20.
Dependencies are managed with Conan.

---

## Core Abstractions

| Abstraction | C++ type | Rust analogue |
|---|---|---|
| Async value | `Future` concept | `Future` trait |
| Async sequence | `Stream` concept | `Stream` trait |
| Coroutine return type | `Coro<T>` | `async fn` / `impl Future` |
| Async generator | `CoroStream<T>` | `async-stream` / `impl Stream` |
| Scheduled work unit | `Task<T>` | `tokio::task::JoinHandle` |
| Result ownership | `JoinHandle<T>` | `JoinHandle<T>` |
| Executor wakeup | `Waker` | `Waker` |
| Poll context | `Context` | `Context` |
| Runtime | `Runtime` | `tokio::runtime::Runtime` |

### `Future` concept

```cpp
template<typename F>
concept Future = requires(F f, Context& ctx) {
    typename F::OutputType;
    { f.poll(ctx) } -> std::same_as<PollResult<typename F::OutputType>>;
};
```

Any type with a `poll(Context&)` method returning `PollResult<T>` is a future. The concept
is structural — no inheritance required.

### `Stream` concept

```cpp
template<typename S>
concept Stream = requires(S s, Context& ctx) {
    typename S::ItemType;
    { s.poll_next(ctx) } -> std::same_as<PollResult<typename S::ItemType>>;
};
```

A stream is a future that produces a sequence of items. `PollReady(value)` is the next
item; `PollDropped` signals exhaustion.

---

## Poll Model

Every async operation produces a `PollResult<T>`:

```cpp
template<typename T>
class PollResult {
    using Result = std::expected<T, std::exception_ptr>;
    std::variant<PendingTag, Result, DroppedTag> m_state;
    // ...
};
```

The `std::expected<T, std::exception_ptr>` arm unifies the ready-value and error cases in
a single variant slot. This avoids the structural constraint violation that would arise from
`std::variant<PendingTag, T, std::exception_ptr, DroppedTag>` when `T = std::exception_ptr`
(duplicate types are ill-formed).

| State | `m_state` holds | Meaning |
|---|---|---|
| `PollReady(v)` | `Result` with `has_value() == true` | Completed normally |
| `PollError(e)` | `Result` with `has_value() == false` | Completed with exception |
| `PollPending` | `PendingTag` | Not ready; waker registered |
| `PollDropped` | `DroppedTag` | Cancelled and fully drained |

`PollDropped` is the key addition over Rust's three-state model. It propagates up through
the caller chain as the signal that a cancelled branch has finished draining all its
children — safe to discard.

### Waker and Context

`Context` carries a `Waker` into each `poll()` call. The future stores the waker and,
when the operation completes (on an I/O callback thread, a blocking thread, or another
task's completion), calls `waker->wake()`. This enqueues the owning `Task` into the
executor's injection queue, where it will be re-polled.

`Waker` holds a `shared_ptr<Task>` internally. Calling `wake()` on a waker for a dead
task is a safe no-op — the `shared_ptr` reference count keeps the Task control block
alive long enough for the wake call to complete, then it is freed.

---

## Runtime and Executors

`Runtime` is the top-level object. It owns:
- An `Executor` (pluggable; selected by thread count)
- An `IoService` (libuv event loop on a dedicated I/O thread)
- A `BlockingPool` (thread pool for blocking work)

```cpp
Runtime rt;           // work-stealing, hardware_concurrency threads
Runtime rt(1);        // single-threaded
Runtime rt(4);        // work-stealing, 4 threads

rt.block_on(my_coro());  // drives the top-level coroutine to completion
```

Thread-locals `t_current_runtime`, `t_current_io_service`, and `t_worker_index` are set
on each worker thread at startup so that `spawn()`, `sleep_for()`, and `spawn_blocking()`
can access runtime services without passing them through call stacks.

### Single-threaded executor

Simple round-robin queue. Deterministic and useful for unit tests.

### Work-sharing executor

Shared injection queue protected by a mutex. Multiple worker threads drain it cooperatively.
Lower latency than single-threaded; simpler than work-stealing. Used as a stepping stone
in the implementation.

### Work-stealing executor

The primary production executor. Modelled on Tokio's scheduler:

- Each worker has a fixed-capacity `WorkStealingDeque` (local run queue).
- A shared `InjectionQueue` accepts tasks from non-worker threads (e.g. waker calls from
  the I/O thread or blocking threads).
- Each poll cycle, a worker:
  1. Drains up to a budget (default 64) tasks from its local queue.
  2. Checks the injection queue (drains up to budget tasks, distributing to other workers).
  3. Attempts to steal from a randomly chosen peer's deque.
  4. Parks on a `std::binary_semaphore` if no work is found.
- Tasks are woken via `waker->wake()` which calls `executor->enqueue(task)` — thread-safe
  injection from any thread.

#### `SchedulingState` CAS machine

Each `Task` has an atomic `SchedulingState`:

```
Idle           — parked, not in any queue
Running        — currently being polled by a worker
Notified       — wake() called while Idle; about to be enqueued
RunningAndNotified — wake() called while Running; re-enqueue after poll
Done           — completed; no further state transitions
```

CAS transitions prevent double-enqueue (a task appearing in two queues simultaneously)
and ensure a wake that arrives during a poll is not lost.

---

## Coroutine Types

### `Coro<T>`

The primary coroutine return type. Implements `Future<T>`. The compiler generates a
coroutine frame for any function returning `Coro<T>` that uses `co_await` or `co_return`.

`Coro<T>` wraps a `shared_ptr<TaskState<T>>` which holds:
- The `coroutine_handle<>` to the suspended frame
- The `cancelled` flag
- The `CoroutineScope` pending-children list
- The result (set on completion)

`poll()` on `Coro<T>`:
1. Sets `t_current_coro` to this coroutine's state.
2. If `cancelled`: calls `handle.destroy()`, registers children in the pending list,
   returns `PollPending` (or `PollDropped` if the pending list is empty).
3. Otherwise: resumes the coroutine handle; returns the appropriate `PollResult` based
   on suspension or completion.

### `CoroStream<T>`

An async generator — a coroutine that `co_yield`s values. Implements `Stream<T>`.
`poll_next()` resumes the generator until the next `co_yield` or `co_return`.

### `co_invoke(lambda)`

A convenience wrapper that constructs a `Coro<T>` from a callable without naming a
separate coroutine function. The lambda's lifetime is managed inside the `Coro<T>` frame.
This is important for lambda captures: the `Coro<T>` returned by `co_invoke` keeps the
lambda alive for as long as the coroutine runs, making reference captures safe within
the lambda body.

---

## Task Lifecycle

### Spawning

```cpp
// Returns JoinHandle<T> — owns the result and can cancel
JoinHandle<int> h = spawn(compute());

// Fire and forget
spawn(background_work()).detach();

// Named task (for debugging) — use build_task() builder
JoinHandle<int> h2 = build_task().name("my-task").spawn(compute());
```

`spawn()` immediately schedules the task on the runtime and returns a `JoinHandle<T>`.
Use `build_task()` when you need to set a name or buffer size. `.detach()` on the handle
drops interest in the result; the task runs to completion independently.

`JoinHandle<T>` satisfies `Future<T>`. `co_await handle` suspends until the task
completes, then returns the result (or rethrows the exception).

### Handle lifecycle and cancellation

Dropping a `JoinHandle` without calling `.detach()` or `co_await`ing it:
1. Sets `TaskState::cancelled = true`.
2. Reads `t_current_coro` — if inside a coroutine poll, registers the `TaskState` as a
   pending child of that coroutine (the coroutine scope mechanism).
3. Wakes the task via `waker->wake()` so it is re-polled through the `PollDropped` path.

`.detach()` clears the internal `shared_ptr<TaskState>` before the destructor body,
skipping all of the above. Detached tasks are fire-and-forget.

---

## Cancellation and Structured Concurrency

### The core problem

C++ coroutine frames are heap-allocated. The only way to run destructors for locals is
to *resume* the coroutine. Simply dropping the `shared_ptr<Task>` frees the control block
but leaves the frame's locals alive without running their destructors — a resource leak,
or worse, a use-after-free if child tasks hold references into that frame.

### Solution: `PollDropped` + coroutine scope

Cancellation is delivered as a poll signal, not by freeing memory:

1. The task's `cancelled` flag is set.
2. On the next `poll()`, the coroutine detects this and calls `handle.destroy()`, which
   runs all local destructors in LIFO order.
3. Each `JoinHandle` destructor fires during this destruction, cancels its child task,
   and registers it in the coroutine's pending-children list.
4. The coroutine returns `PollPending` until all pending children return `PollDropped`.
5. When the last child drains, the coroutine returns `PollDropped` to its caller.

This mirrors `std::thread::scope` for threads: destruction blocks until all scoped
threads complete. Here the "blocking" is async — the `Coro<T>` future stays alive,
returning `PollPending`, while children drain.

### `t_current_coro` thread-local

During any `poll()` call, `t_current_coro` is set to the current coroutine's `TaskState`.
This allows `JoinHandle` destructors — which may fire from within a coroutine body, from
local variable destructors, or from `handle.destroy()` during cancellation — to find the
enclosing scope without any explicit scope parameter.

### `JoinSet<T>`

`JoinSet<T>` provides explicit structured concurrency for homogeneous child tasks:

```cpp
JoinSet<int> js;
js.spawn(compute(1));
js.spawn(compute(2));
js.spawn(compute(3));

// Consume results in completion order:
while (auto result = co_await next(js))
    use(*result);

// Or discard all results:
co_await js.drain();
```

Internally, `JoinSetSharedState<T>` is shared between the `JoinSet`, all `JoinSetTask`
wrappers, and any live drain future via `shared_ptr`. It holds:
- A result queue (`variant<T, exception_ptr>` for non-void, `exception_ptr` for void)
- `pending_count` — number of tasks still running
- A consumer waker for `next()`/`drain()` waiters
- `pending_handles` — `std::list<JoinHandle<void>>` for running tasks
- `done_handles` — `std::list<JoinHandle<void>>` for finished tasks, swept at the next
  call to `spawn()`, `poll_next()`, or `drain()` (outside the lock, to avoid
  lock-ordering issues with `JoinHandle` destructors)

`JoinSet<T>` (non-void) satisfies `Stream<T>` and composes with `select`.

Dropping a `JoinSet` cancels all pending children. The enclosing `CoroutineScope` ensures
they drain before the parent frame is freed.

### Combinators and cancellation

`select(f1, f2, ...)` and `timeout(dur, f)` cancel their losing branches. They do not
drop them — they mark them cancelled and continue polling until each returns `PollDropped`.
Only then is the winning result delivered to the caller.

This adds drain latency compared to Tokio (which can drop instantly due to the borrow
checker), but is required for correctness in C++.

---

## I/O Reactor

### `IoService`

libuv is not thread-safe: nearly all API calls must come from the thread that owns the
event loop. `IoService` solves this with a dedicated I/O thread:

```
Worker thread                      I/O thread (owns uv_loop_t)
─────────────────────────────      ────────────────────────────────
SleepFuture::poll():
  allocate shared State
  push StartRequest to queue ───►  io_async_cb fires:
  uv_async_send(&m_async)            process_queue():
                                       req->execute(&m_uv_loop)
                                         uv_timer_init(...)
                                         uv_timer_start(...)

                                   ... deadline expires ...
                                   timer_cb:
                                     state->waker->wake() ────────► executor->enqueue(task)
```

`uv_async_t` is the only thread-safe libuv primitive. `IoService::submit()` pushes an
`IoRequest` onto a mutex-protected queue and calls `uv_async_send()`. The I/O thread's
`io_async_cb` drains the queue and calls `execute()` on each request.

Multiple `uv_async_send()` calls before the callback fires are coalesced — the callback
fires at least once but the queue drain processes all pending requests.

### `IoRequest` command pattern

`IoRequest` is an abstract base with a single `execute(uv_loop_t*)` virtual method.
`IoService` has zero knowledge of concrete request types. Each I/O subsystem owns its
request types privately:

- `SleepFuture` privately defines `StartRequest` and `CancelRequest`
- `TcpStream` privately defines its connect/read/write requests
- `WsStream` privately defines its connect/send/close requests

This keeps `io_service.h` minimal and each subsystem fully self-contained.

### `SleepFuture` / `sleep_for()`

Millisecond resolution (libuv timer granularity). Deadline is converted with
`std::chrono::ceil<milliseconds>` so the timer never fires before the deadline.

Each `SleepFuture` holds a `shared_ptr<State>` shared with the I/O thread. The state
contains a `std::atomic<std::shared_ptr<Waker>>` (C++20 atomic shared_ptr) so the worker
thread can store a new waker on re-poll concurrently with the I/O thread reading it in
`timer_cb` — no mutex needed on this hot path.

Cancellation: the destructor submits a `CancelRequest`. Both `timer_cb` and
`CancelRequest::execute()` claim `uv_close()` via `fired.exchange(true)` — whichever
wins owns the close; the other is a no-op.

### `TcpStream`

Wraps `uv_tcp_t`. Async connect, read, and write futures each hold a `shared_ptr` to a
shared connection state. The connect future submits a `TcpConnectRequest`; read submits
`TcpReadRequest`; write submits `TcpWriteRequest`. Callbacks on the I/O thread store
results and call `waker->wake()`.

### `WsStream` / `WsListener`

Built on [libwebsockets](https://libwebsockets.org/) with `LWS_SERVER_OPTION_LIBUV` so
lws registers all its handles on the existing `uv_loop_t` — no extra thread.

The `lws_context*` is owned by `IoService`, created on the I/O thread at startup and
destroyed in `stop()`. All lws operations (connect, send, close) are submitted as
`IoRequest` commands.

A single `protocol_cb` C function dispatches all events (`ESTABLISHED`, `RECEIVE`,
`WRITEABLE`, `CLOSED`, `CONNECTION_ERROR`) to the appropriate sub-state in
`coro::detail::ws::ConnectionState`. Multiple futures (`ConnectFuture`, `ReceiveFuture`,
`SendFuture`) share `ConnectionState` via `shared_ptr`.

Writing requires write-readiness: `SendFuture` enqueues a `SendSubState*` and requests
`lws_callback_on_writable()`; lws fires `WRITEABLE` when ready; `protocol_cb` calls
`lws_write()` and wakes the future. This is one extra suspension point versus `TcpStream`
but required by the lws API.

### Shutdown ordering

```
Runtime::~Runtime():
  1. m_executor.reset()      // join workers — no more waker->wake() calls
  2. m_io_service.stop()     // signal I/O thread, join it
  3. uv_loop_close()
```

`m_io_service` is declared before `m_executor` in `Runtime` so it is destroyed after
(C++ reverse-declaration-order destruction). This guarantees the executor is gone before
the I/O loop is closed.

---

## Blocking Thread Pool

`BlockingPool` provides a thread pool for synchronous, potentially-blocking work that
must not run on executor worker threads (which must never block).

```cpp
int result = co_await coro::spawn_blocking([]() -> int {
    return expensive_cpu_work();  // runs on blocking pool thread
});
```

- Pool grows on demand up to a configurable maximum (default 512, matching Tokio).
- Idle threads time out after a keep-alive period (default 10s) and exit.
- Threads are detached at creation; the pool tracks `total_threads` and `idle_threads`
  under a mutex to know when shutdown is complete.
- Each call allocates a `shared_ptr<BlockingState<T>>` shared between the `BlockingHandle`
  and the pool thread. `BlockingState` holds a mutex, a condition variable (for
  `blocking_get()`), the waker, and the result as
  `std::optional<std::expected<T, std::exception_ptr>>`.
- Dropping a `BlockingHandle` before the callable returns **detaches** — the thread runs
  to completion and discards the result. Waiting is not safe because blocking threads
  cannot be cooperatively cancelled.
- Worker threads call `set_current_runtime(m_runtime)` on startup so that code running
  inside a `spawn_blocking` callable can itself call `spawn_blocking` or `spawn`.

---

## Channels

All three implemented channel variants live in `include/coro/sync/`. They share common
design principles: `std::expected` for fallible operations, intrusive waiters (no
allocations beyond the channel itself), and RAII handles with reference counting.

### `oneshot`

Single producer, single consumer, single value. The sender is synchronous; the receiver
satisfies `Future<T>`.

Shared state holds a `std::optional<T>` slot, `sender_alive`, `receiver_alive`, and a
single waker. No intrusive list needed — at most one waiter at a time.

`OneshotSender::send(T)` returns `std::expected<void, T>` — on failure (receiver already
dropped) the unsent value is returned to the caller.

### `mpsc`

Multi-producer, single consumer, bounded ring buffer with backpressure.

The ring buffer is allocated once at construction (fixed capacity). Senders are cloneable;
each clone is a separate object sharing the same `MpscShared<T>` via `shared_ptr`.

Waiter nodes live in the coroutine frames of suspended futures — no heap allocation for
waiters. `SenderNode` (linked into `sender_waiters`) holds the waker and the unsent value.
`ReceiverNode` (a single `std::optional` in the shared state) holds the waker and a
destination pointer into the receiver's frame.

**Zero-copy fast paths:**
- Sender finds a waiting receiver → value moves directly from sender argument to receiver
  frame, bypassing the ring buffer.
- Receiver finds suspended senders with an empty buffer → value moves directly from sender
  frame to receiver, bypassing the ring buffer.

Cancellation: a suspended `SendFuture` or `RecvFuture` destructor re-acquires the channel
mutex and unlinks its intrusive node. Safe because the destructor holds a `shared_ptr` to
the channel state.

### `watch`

Single producer, multiple consumers, latest-value semantics. Send never blocks.

`WatchShared<T>` uses two separate locks:
- `std::shared_mutex value_mutex` — shared for `borrow()`, exclusive for `send()`
- `std::mutex waker_mutex` — guards the receiver waker list only

Separating them means a long-held `BorrowGuard` (shared read lock on `value_mutex`) does
not block `changed()` from registering its waker (which only needs `waker_mutex`).

`borrow()` returns a `BorrowGuard<T>` — a lightweight RAII handle that holds the shared
read lock via `operator*` and `operator->`. The lock releases on guard destruction.
**Do not hold a `BorrowGuard` across a `co_await` point** — doing so holds the read lock
while suspended, blocking all future `send()` calls.

Each receiver stores its own `last_seen` version number. `changed()` suspends if
`last_seen == current_version`; returns immediately if a newer value has been sent.

---

## Combinators

### `select(f1, f2, ...)`

```cpp
auto result = co_await select(timeout(5s), read_packet(sock), recv_signal());
// result is std::variant<SelectBranch<0,TimeoutResult>, SelectBranch<1,Packet>, SelectBranch<2,Signal>>
```

`SelectFuture<Fs...>` polls all branches in round-robin order (advancing `m_poll_start`
each tick for fairness). On the first `PollReady` or `PollError`:
1. The winning result is stored internally.
2. Each losing branch that satisfies `Cancellable` (`Coro<T>`, `CoroStream<T>`) is
   cancelled and polled until it returns `PollDropped`.
3. Non-`Cancellable` futures are dropped immediately.
4. Only once all losing branches have returned `PollDropped` is the winning result
   delivered to the caller.

`SelectBranch<N, T>` tagging ensures the result variant is well-formed even when multiple
branches share the same `OutputType` (including `void`).

### `timeout(duration, future)`

Thin wrapper: `select(sleep_for(duration), std::forward<F>(future))`. Returns
`SelectBranch<0, void>` if the timeout wins, `SelectBranch<1, T>` if the future wins.

### `next(stream)` / `JoinSet` as `Stream`

`next(s)` wraps a `Stream` in a `NextFuture` that calls `poll_next()` and returns
`std::optional<T>` — `nullopt` on exhaustion. `JoinSet<T>` (non-void) exposes `ItemType`
and `poll_next()`, so it satisfies `Stream<T>` and works with `next()`, `select`, and
all future stream combinators.

---

## Error Handling Policy

Fallible operations return `std::expected<T, E>` rather than throwing:

```cpp
auto r = co_await rx.recv();
if (!r) { /* channel closed */ return; }
T value = *r;

// Or let it throw:
T value = (co_await rx.recv()).value();
```

`PollResult` uses `std::expected<T, std::exception_ptr>` internally. Exception-based
errors from coroutine bodies are captured as `std::exception_ptr` and stored as
`PollError`; `co_await`ing the `JoinHandle` rethrows them.

`trySend` returns `std::expected<void, TrySendError<T>>` where `TrySendError<T>` carries
both the failure reason (`Full` or `Disconnected`) and the unsent value, so move-only
types are never silently dropped.

---

## Threading and Synchronization

**General rule: prefer `std::mutex` over `std::atomic`.**
Atomics are used only where a mutex would introduce a lock-ordering deadlock, or where
profiling justifies the complexity:

| Location | Mechanism | Reason |
|---|---|---|
| `SchedulingState` | `std::atomic` + CAS | Hot path; mutex would serialize all wakeups |
| `SleepFuture::State::waker` | `std::atomic<shared_ptr<Waker>>` | Written by worker, read by I/O thread; no shared lock available |
| `BlockingState` | `std::mutex` | Low contention; protocol clarity outweighs cost |
| Channel shared state | `std::mutex` | Multiple fields updated together; mutex makes invariants obvious |
| `IoService` queue | `std::mutex` | Drain requires moving the entire queue; mutex simplest |
| `JoinSetSharedState` | `std::mutex` | List splice + counter + waker update must be atomic together |

**Known concurrency concerns are documented inline** with comments in the source. When in
doubt about whether a pattern is safe, a comment is added even if no race has been
observed.

**`[[nodiscard]]` on all future-returning functions.** Discarding a `JoinHandle`,
`BlockingHandle`, `SpawnBuilder`, `SelectFuture`, etc. silently cancels work. The
annotation turns silent bugs into compile-time warnings.

---

## Header Layout

```
include/coro/
  coro.h                    Coro<T> — primary coroutine return type
  coro_stream.h             CoroStream<T> — async generator
  future.h                  Future concept; NextFuture adapter
  stream.h                  Stream concept; next() free function

  runtime/
    runtime.h               Runtime — owns executor, IoService, BlockingPool
    executor.h              Executor interface; executor factory
    io_service.h            IoService, IoRequest, current_io_service()

  task/
    join_handle.h           JoinHandle<T>, SpawnBuilder
    join_set.h              JoinSet<T>
    spawn_blocking.h        BlockingHandle<T>, BlockingPool, spawn_blocking()

  sync/
    sleep.h                 SleepFuture, sleep_for()
    oneshot.h               oneshot::channel<T>
    mpsc.h                  mpsc::channel<T>
    watch.h                 watch::channel<T>

  io/
    tcp_stream.h            TcpStream
    ws_stream.h             WsStream, WsListener

  detail/
    poll_result.h           PollResult<T>
    waker.h                 Waker, Context
    task_state.h            TaskState<T>, SchedulingState
    coroutine_scope.h       CoroutineScope, t_current_coro
    intrusive_list.h        IntrusiveList, IntrusiveListNode

src/
  runtime.cpp
  executor.cpp
  io_service.cpp
  blocking_pool.cpp
  sleep.cpp
  ws_stream.cpp
  ...

test/                       gtest unit tests — one file per module
doc/                        Design documents (Markdown)
```

When adding a new header, consult `doc/module_structure.md` for placement rules.
