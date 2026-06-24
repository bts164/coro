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


## Channel — multi-sender watch variants (under consideration)

Tokio's `watch::Sender<T>` is cloneable — it uses an atomic `ref_count_tx` alongside
the `Arc` refcount, and the channel closes when the last sender clone is dropped (see
`tokio/src/sync/watch.rs`). This library's `WatchSender<T>` is currently move-only;
making it cloneable to match Tokio is straightforward using the existing
`WatchShared<T>` infrastructure with a sender ref-count alongside `receiver_count`.

Two related patterns have been identified that would benefit from cloneable senders:

**Multiple independent writers, passive receivers.** Several tasks publish updates to
the same watched value. With a cloneable sender each task holds its own clone; the
channel closes when all clones are dropped. The tradeoffs (silent overwrites, no
feedback to the losing writer, ambiguous `changed()` attribution) are real but the
same ones Tokio accepts.

**Symmetric peers — tasks that both read and write.** When a task holds both a sender
and a receiver, its own writes leave the receiver stale, so `changed()` fires
immediately for its own update rather than waiting for another writer. This is
arguably a distinct semantic from `watch` and may warrant a separate channel type
(tentatively `coro::peer::channel<T>`) where each participant holds combined
sender/receiver capability and own writes automatically advance `last_seen`.

The peer channel and cloneable sender interact — cloneable sender may be sufficient
for the peer use case, or the two may need to be addressed together. Resolve together.

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

## `void` support for `mpsc` and `CoroStream`

`JoinSet<void>` already works correctly — it uses `std::conditional_t<std::is_void_v<T>, bool, std::optional<T>>` for its stream item type and has no internal value storage. `mpsc_channel<void>` and `CoroStream<void>` do not compile today because both store `T` values internally.

For `mpsc<void>` the fix requires: replacing `std::deque<T>` with a count, removing the value field from `MpscSenderNode<T>`, changing `send(T)` / `try_send(T)` to zero-argument forms, and threading `if constexpr` or a `ValueOrUnit` type alias through `RecvFuture::poll()` and `_tryPromoteSender()`. Not difficult, just touching many sites.

For `CoroStream<void>` the additional wrinkle is that `co_yield` requires an expression in C++ — `CoroStream<void>` would need `co_yield` with no argument (a new zero-argument `yield_value()` overload in the promise) and a deliberate decision about what the user writes at the call site (likely just `co_yield;` if the compiler supports it, or `co_yield coro::unit` with a unit type).

The use case for both is real: multi-producer event notification where the signal itself is the message and no value needs to be carried.

## `JoinSet` — completed result accumulation

`JoinSet` removes a task from its internal task list when it completes, but the result is
moved into a completion queue and held there until consumed via `next()`. In a long-running
server that spawns a task per connection and never drains the set, completed results
accumulate indefinitely — a slow memory leak proportional to total connections served.

The right fix is unclear and needs more consideration before committing to an API:

- **Auto-drain mode** — a construction flag or member call (e.g. `js.autoDrain()`) that
  silently discards `void` results and drops non-void results on completion, so the queue
  never grows. Simple, but removes the ability to observe results later.
- **Capacity limit with back-pressure** — cap the completion queue; new tasks cannot be
  spawned until the caller drains at least one result. Correct but changes the spawning API.
- **Explicit `discard()` / `try_next()`** — a non-suspending poll that drains whatever is
  ready without blocking, called opportunistically by the spawner between spawns.

Until resolved, callers using `JoinSet` in fire-and-forget patterns (e.g. one task per
connection) should periodically call `next()` or `drain()` to keep the completion queue
bounded, or use `spawn(...).detach()` instead if results are not needed at all.

## `JoinSet` — `CoroStream<T>` producer support

Extend `JoinSet<T>` to accept `CoroStream<T>` producers alongside `Coro<T>` tasks,
with `next()` forwarding individual `co_yield` values from any producer as they arrive.
This makes `JoinSet` a heterogeneous set: a single set can hold a mix of one-shot tasks
and streaming generators, all with their lifetimes managed together.

```cpp
auto js = JoinSet<Result>{};
js.spawn(fetch_metadata(id));          // Coro<Result>     — one result at completion
js.spawn(search_backend_a(query));     // CoroStream<Result> — yields results as found
js.spawn(search_backend_b(query));     // CoroStream<Result> — yields results as found
while (auto r = co_await js.next())
    display(*r);                       // results from all three, interleaved as ready
```

From a user perspective this is identical to a dedicated "merged stream" type — the
distinction is purely in how the implementation manages yields vs. completions. The
heterogeneous capability is the key advantage over a stream-only type: tasks that
produce a single terminal value (`Coro<T>`) and tasks that stream multiple values
(`CoroStream<T>`) can coexist in the same set without the user needing two separate
abstractions.

This fills the "multiple producers, bundled task management" cell in the
`CoroStream` vs `mpsc` decision matrix and is the high-level alternative to manually
wiring producer tasks to a shared `MpscSender`.

## `CoroStream` — buffered concurrent producer

`CoroStream<T>` is pull-based: the generator resumes only when the consumer calls
`next()`, so producer and consumer stay in step with no explicit backpressure needed.
For cases where producing the next item is expensive enough that it should overlap with
consumption of the current one, a `buffered(N)` adapter is needed.

```cpp
// Generator runs up to N items ahead of the consumer.
auto stream = coro::buffered(search(query), /*capacity=*/16);
while (auto result = co_await coro::next(stream))
    display(*result);
```

**Design:** `buffered(stream, N)` spawns the generator as a task that drains
`co_yield` values into an `MpscSender<T>` of capacity N, then returns a wrapper type
that owns the `JoinHandle` (producer task) and exposes `MpscReceiver<T>` as a
`Stream<T>`. Cancellation is automatic — dropping the wrapper drops the handle
(cancels the producer) and the receiver (the producer's next `send()` returns an error,
causing a clean exit).

The `StreamSpawnBuilder` path (`build_stream().buffer(16).spawn(...)`) is an
alternative API surface worth considering alongside the free-function adapter.

**Effort:** low-to-medium. All primitives exist (mpsc, task spawning, `Stream`
concept). The work is the bridge task, the wrapper type, and cancellation edge-case
testing — particularly ensuring the producer exits promptly when the consumer is
dropped while the buffer is full.

## `PollStream` — coroutine-based decoder

`PollStream` currently requires a decoder class implementing an explicit state machine
— it receives available bytes, parses as much as it can, and signals whether more data
is needed. This interface is expressive but requires the author to manage parser state
manually.

A coroutine is a compiler-generated state machine, so there is a natural
correspondence: a coroutine-based decoder that suspends at a `co_await need_bytes(N)`
point when it needs more data and resumes when they arrive would express the same logic
as sequential code. The key design constraint is that the decoder coroutine must resume
on the I/O (libuv) thread directly — bypassing the executor scheduler — to preserve
the low-overhead property that motivates `PollStream` in the first place. The libuv
thread would act as the executor for the decoder coroutine.

This requires working out:
- How the decoder coroutine suspends and signals "I need N more bytes"
- How the libuv callback resumes it without going through the task queue
- Whether this replaces the existing decoder class interface or sits alongside it

No implementation until the design is resolved. See the buffered protocol reading
entry in `doc/patterns.md` for user-facing context.

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

## `CancellationSource` / `CancellationToken`

A cancellation primitive for cases where tasks are spawned independently and cannot
share a common owner — for example, tasks registered from multiple call sites that all
need to observe the same shutdown signal.

```cpp
auto [source, token] = coro::cancellation_source();

// Pass token to any number of independently spawned tasks:
coro::spawn(worker_a(token.clone())).detach();
coro::spawn(worker_b(token.clone())).detach();

// Signal all of them simultaneously:
source.cancel();
```

Inside a task, `token.is_cancelled()` can be checked at any point, and
`co_await token.cancelled()` suspends until the token fires — both without requiring
the task to be in a parent–child ownership relationship with the canceller.

This is the idiomatic solution for cross-cutting cancellation that does not map cleanly
to a task tree. Without it, the only alternative is manually threading a watch channel
carrying a shutdown flag, which reimplements cancellation by hand and should be avoided.

This primitive also subsumes the previously considered `AbortHandle` design. The only
thing `AbortHandle` added over dropping a `JoinHandle` was the ability to cancel a task
while still holding the handle to observe its result. That is equally achievable by
passing a `CancellationToken` to the task at spawn time and cancelling the token while
retaining the `JoinHandle` — no separate type needed. `SpawnBuilder` should accept an
optional token via something like `build_task().with_cancel(token).spawn(...)`.

Modelled on Tokio's `CancellationToken` (`tokio-util` crate). Lives in
`include/coro/sync/cancellation_token.h`.

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
