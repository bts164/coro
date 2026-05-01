# Roadmap

Planned work not yet implemented, in rough priority order.

## Single-thread mode: `block_on` drives the uv loop directly

Allow the `Runtime` to run libuv, libwebsockets, and all user coroutine tasks on a
single thread — including the calling thread in `block_on` — with no background uv
thread at all.

**Motivation:** embedded targets, deterministic testing, and applications where the OS
scheduler overhead of a second thread is undesirable. Matches the `tokio::runtime::Builder::new_current_thread()` model.

**Design sketch:**

- `SingleThreadedUvExecutor` gains a "no-thread" construction mode. The uv loop and
  lws context are initialized on first call to `run_until()` rather than in a spawned
  thread.
- A `run_until(predicate)` method (or equivalent) drives `io_thread_loop` on the
  calling thread, stopping when `predicate()` returns true (e.g. `state.terminated`).
- `Runtime::block_on` in this mode calls `run_until(state.terminated)` directly instead
  of scheduling a task and calling `wait_for_completion` (which would deadlock — the
  loop is not running).
- The `uv_async_t` doorbell is retained but becomes a no-op wake source since all
  enqueue calls now originate on the same thread.
- A new `Runtime` constructor overload (or factory) opts into this mode:
  ```cpp
  auto rt = Runtime::single_threaded(); // no background thread
  rt.block_on([](auto...) -> Coro<void> { ... }());
  ```

**Key invariant:** `SingleThreadedUvExecutor` must detect whether it owns the calling
thread (i.e. `run_until` is on the stack) and short-circuit `enqueue` to push directly
to `m_ready` without the `uv_async_send` cross-thread wake, avoiding a redundant
doorbell interrupt on every task wake.

## Migrate error-returning futures to `std::expected`

The library's error handling policy is `std::expected<T, E>` as the default, with
`.value()` as the exception-throwing escape hatch. Currently `JoinHandle` and
`PollResult` use `std::exception_ptr` and rethrow on `await_resume`. These should be
updated:

- `JoinHandle<T>::await_resume()` should return `std::expected<T, std::exception_ptr>`
  instead of rethrowing unconditionally.
- `PollResult<T>` error state should be reexamined in light of this policy.
- Any other futures or combinators that can fail (e.g. `timeout`) should return
  `std::expected` rather than throwing.

This is a breaking API change; coordinate with the channel implementation work since
channels establish the pattern.

## C++20 compatibility (`std::expected` shim)

The library targets C++20 but uses `std::expected<T, E>` (a C++23 addition) for fallible
operations. To support strict C++20 targets:

- Introduce a thin `include/coro/detail/expected.h` that either aliases `std::expected`
  (C++23) or aliases `tl::expected` / a hand-rolled drop-in (C++20).
- All library code uses `coro::detail::expected<T, E>` instead of `std::expected<T, E>`
  directly, so the switch is a one-line change per site.
- `tl::expected` (Conan package `tl-expected`) is the preferred fallback — it is a
  single header, well-tested, and API-compatible with the C++23 standard.

## Document: C++ vs. Rust cancellation model

Write a design document (`doc/cancellation_model.md`) explaining why C++ coroutines cannot
be cancelled by simply dropping the task, and what consequences this has throughout the
library's design. This is one of the most fundamental differences from Tokio and should be
documented prominently. Key points to cover:

- **Why Rust can drop:** a Rust `Future` is a plain value; dropping it at any `await`
  point is safe because the compiler synthesizes `Drop` for all locals in scope and the
  borrow checker guarantees no dangling references. Cancellation in Tokio is literally
  "stop holding the `JoinHandle`" — the task's memory is freed immediately.
- **Why C++ cannot:** a C++ coroutine frame is heap-allocated by the compiler. The only
  way to run destructors for locals and release resources is to *resume* the coroutine and
  let it unwind. Dropping the `shared_ptr<Task>` mid-suspension destroys the frame
  without running any destructors — a resource leak or, if child tasks hold references to
  parent-owned data, a use-after-free.
- **The `PollDropped` contract:** cancellation is delivered as a poll signal
  (`PollDropped`), not an exception. The coroutine must resume, observe the signal,
  propagate it to any child tasks it has spawned (waiting for their `PollDropped` before
  continuing), and then return its own `PollDropped`. Only after this full drain is the
  task's memory safe to free.
- **Impact on executor design:** a cancelled task in `Idle` state must be re-enqueued
  (via `waker->wake()` after setting `cancelled`) so it is polled through the `PollDropped`
  path. The executor cannot simply discard it.
- **Impact on `CoroutineScope` / `JoinSet`:** these primitives exist partly *because* of
  this constraint — they provide a structured way to ensure all child tasks are drained
  before the parent scope exits, which is mandatory rather than optional.
- **Impact on `select` / `timeout`:** the losing branch of a `select` or an expired
  `timeout` must be cancelled and fully drained before the result is delivered to the
  caller. This adds latency that Tokio does not have.
- **User-facing implication:** spawned tasks that capture references to parent-owned data
  are safe *only* within a scope that guarantees the parent outlives all children
  (`CoroutineScope`, `JoinSet`). This is the C++ analogue of Rust's `'static` bound on
  `tokio::spawn`.

## Cancellation propagation tests

The end-to-end cancellation tests deferred in `test/test_coro_scope.cpp` can now be written — `select` and `timeout` are both implemented. Specifically:

- A cancelled coroutine that has spawned children via `spawn()` must drain those children (waiting for their `PollDropped`) before returning its own `PollDropped`.
- Children receive cancellation via `select`/`timeout` combinators (e.g. a branch that loses a select is cancelled and must drain before the select result is delivered).

These tests require no new infrastructure; they are Phase 3 validation for the interaction between `CoroutineScope`, `SelectFuture`, and `Coro<T>`.


## Channel — `broadcast`

`mpsc`, `oneshot`, and `watch` are implemented in `include/coro/sync/`. The remaining
variant is `broadcast`:

A channel where every active receiver sees every message. Unlike `watch`, no values are
dropped as long as the slowest receiver keeps up; a receiver that falls too far behind
receives a lag error indicating missed messages.

```cpp
auto [tx, rx] = coro::broadcast::channel<int>(/*capacity=*/64);

// Clone rx to give each subscriber an independent cursor:
auto rx2 = rx.clone();

// Producer:
co_await tx.send(1);
co_await tx.send(2);

// Each receiver sees all messages sent after its creation:
while (auto item = co_await coro::next(rx))
    use(*item);
```

- `BroadcastSender<T>` — `send(T)` returns `Future<void>`; suspends if all receiver
  buffers are full (slowest receiver applies backpressure).
- `BroadcastReceiver<T>` — satisfies `Stream<T>`; yields messages in send order.
  If a receiver falls behind by more than `capacity` messages it receives a lag error
  on the next `next()` call. Cloneable.
- Use case: event buses, log fanout, pub/sub within a single runtime.

Lives in `include/coro/sync/channel.h`.

## Async `Mutex<T>`

`std::mutex` blocks the OS thread, starving the executor. `Mutex<T>` suspends the *task*
until the lock is free, yielding the thread back to the executor in the meantime.

```cpp
coro::Mutex<std::vector<int>> shared;

coro::Coro<void> append(int value) {
    auto guard = co_await shared.lock();  // suspends if contended; never blocks thread
    guard->push_back(value);
}   // guard released here
```

- `Mutex<T>` owns `T` and exposes it only through the guard, enforcing lock discipline.
- `lock()` returns `Future<MutexGuard<T>>`; the guard releases on destruction.
- Fair queuing: waiting tasks are woken in FIFO order.

Lives in `include/coro/sync/mutex.h`.

## Stream combinators

The `Stream` concept and `next()` are implemented but there are no combinators. Without
them, every stream consumer must write a boilerplate `while (auto item = co_await next(s))`
loop. Modelled on Rust's `StreamExt` / `Iterator`:

- **`map(stream, fn)`** — transform each item: `Stream<T>` → `Stream<U>`
- **`filter(stream, pred)`** — drop items that don't satisfy a predicate
- **`take(stream, n)`** — yield at most `n` items then signal exhaustion
- **`chain(s1, s2)`** — concatenate two streams of the same item type
- **`flat_map(stream, fn)`** — map each item to a stream and flatten one level

All combinators are lazy (zero-cost wrappers satisfying `Stream`) and live in
`include/coro/stream/` (new subdirectory). Each has a corresponding free function
returning a `[[nodiscard]]` adaptor type.

## `AbortHandle`

Currently the only way to cancel a spawned task is to drop its `JoinHandle`, which also
gives up the ability to observe the result. `AbortHandle` decouples cancellation from
result collection:

```cpp
auto [handle, abort] = coro::spawn(task()).submit_with_abort();
abort.abort();          // request cancellation
co_await handle;        // still await completion (receives PollDropped / exception)
```

- `AbortHandle` — a lightweight cancellation token tied to one task; `abort()` marks the
  underlying `TaskState::cancelled`.
- `JoinHandle` — unchanged; still the only way to await the result.
- `SpawnBuilder::submit_with_abort()` returns `pair<JoinHandle<T>, AbortHandle>`.

Lives in `include/coro/task/abort_handle.h`.

## libuv I/O primitives

`TcpStream`/`TcpListener`, `WsStream`/`WsListener`, and `File` are implemented in `include/coro/io/`. Remaining:

- **`UdpSocket`** — async send/recv.
- **DNS resolution** — `resolve(hostname)` returning `Future<IpAddress>`.
- **Process** — Child process management including support for signals, child process I/O, and parent-child IPC

Each wraps the corresponding libuv handle, storing a `Waker` in the callback and waking
the task when the operation completes.

## Logging

The library currently has no structured logging. Executor internals, task lifecycle
events, and fatal invariant violations (e.g. unexpected CAS failures on `SchedulingState`)
need a way to emit diagnostics without coupling to a specific logging framework.

Design goals:

- **Zero-cost when disabled:** logging calls compile away entirely when the log level is
  below the threshold. No heap allocation, no format string evaluation.
- **Pluggable sink:** the library does not own stderr. Users wire in their own sink
  (spdlog, std::print, a custom callback) via a single registration point.
- **Structured levels:** at minimum `TRACE`, `DEBUG`, `INFO`, `WARN`, `ERROR`, `FATAL`.
  `FATAL` logs and then calls `std::terminate()` — used for executor invariant violations
  where continuing would produce undefined behaviour.
- **Task identity:** log messages emitted from within a task should optionally include a
  task ID so scheduler traces can be correlated across threads.

Suggested events to instrument once logging exists:

- Task created / scheduled / polled / completed / cancelled
- `SchedulingState` transitions (at `TRACE` level)
- CAS failures that indicate bugs (`FATAL`)
- `IoService` timer wakeup firing
- Worker thread start / stop
- Injection queue drain counts (task budget enforcement)
