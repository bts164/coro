# Roadmap

Planned and designed work not yet implemented, in rough priority order.

## 1. Cancellation propagation tests

The end-to-end cancellation tests deferred in `test/test_coro_scope.cpp` can now be written — `select` and `timeout` are both implemented. Specifically:

- A cancelled coroutine that has spawned children via `spawn()` must drain those children (waiting for their `PollDropped`) before returning its own `PollDropped`.
- Children receive cancellation via `select`/`timeout` combinators (e.g. a branch that loses a select is cancelled and must drain before the select result is delivered).

These tests require no new infrastructure; they are Phase 3 validation for the interaction between `CoroutineScope`, `SelectFuture`, and `Coro<T>`.

## 2. libuv integration

Currently `SleepFuture` is clock-polling only: the executor is not woken at the deadline; it only fires when the executor happens to re-poll. This needs:

- **Timer waker** in `SleepFuture::poll()`: register a one-shot libuv timer that calls `waker->wake()` when the deadline expires, replacing busy-polling.
- **Event loop in `block_on`**: drive `uv_run(loop, UV_RUN_NOWAIT)` alongside `poll_ready_tasks()` so I/O and timer callbacks are processed.
- **Thread pool startup** in `Runtime`: create libuv loop and worker threads; shut them down in the destructor.

The waker/context design already supports this — leaf futures call `ctx.getWaker()` and store it for the callback to fire.

## 3. Work-stealing executor

A multi-threaded work-stealing executor to replace (or sit alongside) `SingleThreadedExecutor`. Modelled on Tokio's scheduler:

- Per-thread run queues with work-stealing on underload.
- The `Executor` interface is already designed to be pluggable; `Runtime` can select the implementation at construction time (e.g. `num_threads == 1` → single-threaded, otherwise work-stealing).
- Thread-local `t_current_coro` and the `CoroutineScope` mutex are already safe for multi-threaded use.

## 4. Fate of `Synchronize`

`Synchronize` was designed as an explicit structured-concurrency scope: a parent borrows data into child tasks and waits for all of them before returning. Now that `Coro<T>` implicitly drains dropped `JoinHandle`s via `CoroutineScope`, the primary safety motivation for `Synchronize` is partially covered.

Options:
- **Keep as first-class primitive** — still useful when the user wants an explicit, named scope with a clear lifetime boundary, or when non-`Coro` futures need structured grouping.
- **Convenience alias** — `Synchronize` becomes a thin wrapper that makes the pattern explicit without adding new semantics.
- **Remove** — rely entirely on the implicit scope; document the pattern instead.

Decision deferred until multi-threaded use cases are clearer.
