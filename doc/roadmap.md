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

## 4. ~~`co_invoke` — safe capturing-lambda coroutines~~ — implemented

### The problem

Any capturing lambda that returns `Coro<T>` has a latent use-after-free bug when used in
the most natural rvalue pattern. The compiler lowers the lambda to an anonymous struct with
captures as members and an `operator()` member function. Because `operator()` is a member
function, the coroutine frame it creates captures `this` — a pointer to the anonymous struct
instance. The struct is a temporary and is destroyed at the end of the full expression, before
the coroutine is ever polled. From that point on, every access to a captured variable reads
through a dangling pointer.

```cpp
// DANGEROUS — lambda struct destroyed at the semicolon,
// before the coroutine frame is ever resumed.
auto coro = [x]() -> Coro<void> {
    co_await something();
    use(x);          // accesses this->x — this is dangling
}();

co_await coro;       // use-after-free on first resumption
```

The single-expression form feels even safer but has the same problem — the lambda is
destroyed after the first suspension point, not after the `co_await` completes:

```cpp
// DANGEROUS — lambda destroyed after the opening brace is reached,
// not after the full co_await completes.
co_await [x]() -> Coro<void> {
    co_await something();  // lambda already dead by here
    use(x);
}();
```

This affects **any** capturing lambda returning `Coro<T>` or `CoroStream<T>`, not just those
used inside `Synchronize`.

### Proposed solution: `co_invoke`

A `co_invoke` helper moves the lambda into a wrapper that is stored alongside the coroutine
frame and shares its lifetime. The wrapper is a `Future<T>` that owns both the lambda object
and the `Coro<T>` it produced, ensuring the lambda is not destroyed until the coroutine
completes.

```cpp
// SAFE — lambda is moved into the co_invoke wrapper and kept alive
// for the full duration of the coroutine.
co_await co_invoke([x]() -> Coro<void> {
    co_await something();
    use(x);    // safe: lambda lives as long as the Coro
});
```

`co_invoke` should also compose correctly with `spawn`:

```cpp
auto handle = spawn(co_invoke([x]() -> Coro<void> {
    co_await something();
    use(x);
})).submit();
```

### Implementation sketch

```cpp
template<typename Lambda>
class CoInvokeFuture {
public:
    using OutputType = /* deduce from Lambda's Coro<T> return type */;

    explicit CoInvokeFuture(Lambda lambda)
        : m_lambda(std::move(lambda))
        , m_coro(m_lambda())   // operator() called on the stored copy — this is stable
    {}

    PollResult<OutputType> poll(detail::Context& ctx) {
        return m_coro.poll(ctx);
    }

private:
    Lambda          m_lambda;  // must be stored BEFORE m_coro (member init order)
    Coro<OutputType> m_coro;
};

template<typename Lambda>
[[nodiscard]] auto co_invoke(Lambda lambda) {
    return CoInvokeFuture<Lambda>(std::move(lambda));
}
```

The critical ordering constraint is that `m_lambda` must be declared before `m_coro` in the
struct so that it is constructed first and destroyed last — `m_coro`'s frame holds a `this`
pointer into `m_lambda`, so `m_lambda` must outlive `m_coro`.

### Deliverables — complete

- `include/coro/co_invoke.h` — `CoInvokeFuture<Lambda>`, `CoInvokeStream<Lambda>`, and
  two `co_invoke` overloads (one for `Future`-returning lambdas, one for `Stream`-returning).
- `test/test_co_invoke.cpp` — value capture, suspension, move safety, `spawn` composition,
  and `CoroStream` lambda tests.
- Warning added to the Getting Started guide with the dangerous pattern and safe fix.

### Interaction with `Synchronize`

`co_invoke` solves the *lambda object lifetime* problem. `Synchronize` solves the *captured
reference lifetime* problem — ensuring that locals captured by reference into a spawned task
are still alive when the task runs. Both are needed: `co_invoke` keeps the lambda alive,
`Synchronize` keeps the referents alive.

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

## 6. `JoinSet<T>` — structured concurrency with explicit result collection

A `tokio::task::JoinSet`-style API that lets a coroutine spawn multiple homogeneous child
tasks and collect their results through an mpsc-style channel.

### Motivation

`Synchronize` drains children implicitly and discards their results. `JoinSet` makes the
lifecycle **explicit** and gives the user control over result consumption:

- Wait for all children (`drain()`).
- Consume results one-by-one as they complete (`next()`), similar to a `Stream`.
- Cancel remaining children early on first error.
- Use multiple `JoinSet`s with `select` for heterogeneous return types.

### Proposed API

```cpp
// Inside a co_invoke lambda (or any coroutine):
co_await co_invoke([&data]() -> Coro<void> {
    JoinSet<int> js;
    for (auto& item : data)
        js.spawn(compute(item));   // homogeneous: all return int

    // Option A: consume results as they arrive
    while (auto result = co_await next(js))
        process(*result);

    // Option B: drain and ignore results (Synchronize equivalent)
    co_await js.drain();
});
```

Composing heterogeneous tasks:
```cpp
JoinSet<int>    int_tasks;
JoinSet<string> str_tasks;
// ... spawn into each ...
// Use select to wait on whichever finishes first:
co_await select(next(int_tasks), next(str_tasks));
```

### Key design decisions

- **Cancel on drop**: dropping a `JoinSet` cancels all remaining children (Tokio semantics).
  This is the safest default in C++ — no silent detachment.
- **Homogeneous `T`**: all spawned tasks must return `T`. Heterogeneous workflows use multiple
  `JoinSet`s. This keeps the channel implementation simple (no type erasure).
- **`JoinSet` satisfies `Stream<T>`**: `next(js)` returns `Future<optional<T>>`, making it
  composable with all existing stream combinators.
- **`JoinSetHandle`** (working name): the consumer side of the channel, returned by
  `JoinSet::handle()` or identical to `JoinSet` itself (TBD).

### Deliverables

- `include/coro/task/join_set.h` — `JoinSet<T>` type
- `doc/join_set.md` — design document (Phase 1)
- `test/test_join_set.cpp` — tests: spawn + drain, spawn + next, cancel on drop, select across two JoinSets
- Update `doc/getting_started.md` and `Synchronize` docs to reference `JoinSet` as the
  preferred pattern for result-collecting structured concurrency.
