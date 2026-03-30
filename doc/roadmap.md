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

## 4. ~~`co_invoke` — safe capturing-lambda coroutines~~ — complete

Implemented in `include/coro/co_invoke.h`. See the Doxygen comment on `co_invoke()` for
the full problem description and usage examples. Tests in `test/test_co_invoke.cpp`.

## 5. ~~Fate of `Synchronize`~~ — resolved: remove after `JoinSet` lands

Once `JoinSet` (item 6) is implemented and its tests pass, `Synchronize` will be removed.

**Why `Synchronize` becomes redundant:**

- `co_invoke` already handles the lambda object lifetime problem (`Synchronize`'s primary
  safety rationale).
- `JoinSet::drain()` provides an explicit mid-coroutine drain point with the same guarantee.
- `JoinSet` is strictly more capable — it can collect results, cancel early on error, and
  compose with `select`. `Synchronize` can only drain-and-discard.

Maintaining two APIs for the same pattern creates unnecessary "which one do I use?" friction
for new users. `Synchronize` will not be deprecated first — since the library is pre-1.0,
it will simply be deleted once `JoinSet` covers all its use cases.

**Migration path** (`Synchronize` → `co_invoke` + `JoinSet`):

```cpp
// Before
co_await Synchronize([&](Synchronize& sync) -> Coro<void> {
    sync.spawn(worker(local_data));
});

// After
co_await co_invoke([&]() -> Coro<void> {
    JoinSet<void> js;
    js.spawn(worker(local_data));
    co_await js.drain();
});
```

## 6. ~~`JoinSet<T>` — structured concurrency with explicit result collection~~ — complete

Implemented in `include/coro/task/join_set.h`. See `doc/join_set.md` for the full design.
Tests in `test/test_join_set.cpp`.
