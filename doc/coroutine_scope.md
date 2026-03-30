# Coroutine Scope — Implicit Structured Concurrency

## Problem

`Synchronize` was designed to make it safe for spawned child tasks to hold references to
data owned by the parent coroutine. It guarantees that all children complete before the
parent's `co_await Synchronize(...)` returns, so the data stays alive for as long as any
child needs it.

This guarantee breaks when the `Synchronize` future itself is cancelled — for example, when
it is one branch of a `select` or `timeout` that loses. In that case:

1. The losing branch is dropped, tearing down the `Synchronize` future and its body coroutine.
2. The spawned child tasks are still running in the executor.
3. Those children hold references to data that may now be freed.

This is a use-after-free waiting to happen, and it cannot be caught at compile time because
C++ has no lifetime borrow checker.

The root cause is deeper than `Synchronize`: any future combinator that can abandon a branch
creates this hazard for any coroutine in that branch that has spawned tasks holding borrowed
references.

## Proposed Design — Every Coroutine Is an Implicit Scope

### Core idea

A coroutine frame owning referenced objects cannot be safely freed until all tasks spawned
with references have completed. Rather than requiring the programmer to use an explicit
`Synchronize` block, we make this guarantee automatic: every coroutine implicitly tracks its
spawned children and defers frame destruction until they finish.

Cancellation changes semantics in two ways:

- **The coroutine body stops executing.** A cancelled coroutine does not resume when polled;
  its frame is destroyed directly via `handle.destroy()`, running all local destructors in
  LIFO order.
- **The `Coro<T>` future stays alive until children drain.** Even after the frame is
  destroyed, the `Coro<T>` wrapper keeps returning `PollPending` until all registered child
  tasks have completed, then returns `PollDropped`.

This mirrors how `std::thread::scope` works for threads: destruction blocks until all scoped
threads finish, even if the scope is unwound by an exception.

### `PollDropped` — a new poll state

`PollResult<T>` gains a fourth state alongside `PollReady(value)`, `PollError(exception)`,
and `PollPending`:

| State | Meaning |
|---|---|
| `PollReady(value)` | completed normally with a value |
| `PollError(exception)` | completed with an exception |
| `PollPending` | not yet done, waker registered |
| `PollDropped` | cancelled and fully drained — safe to discard |

`PollDropped` propagates up through the caller chain the same way a value or error does.
Each layer that receives `PollDropped` from a child treats it as a clean termination signal:
no value to unwrap, no exception to rethrow, just acknowledgement that the branch has
finished draining. Combinators (select, timeout) use `PollDropped` from a cancelled branch
as the signal that cleanup is complete and the held result can be delivered to the caller.

### Thread-local current coroutine

During `poll()`, each coroutine registers itself in a thread-local slot `t_current_coro`
(analogous to the existing `current_runtime()` mechanism). Any code running on that thread
during the poll — including the coroutine body and any destructors triggered by it — can
read this slot to identify which coroutine is the current "scope owner."

### Registration mechanism

When a `JoinHandle` is destroyed, its destructor:
1. Sets `cancelled = true` on the child `TaskState` (existing behaviour).
2. Reads `t_current_coro`.
3. Adds the `TaskState` to that coroutine's pending child list.

For the normal (non-cancelled) path, the coroutine does not stall at the point of JoinHandle
destruction. It continues executing until it reaches the next natural suspension point:
a `co_await`, `co_yield`, or `co_return`. At that point, before proceeding, it checks its
pending child list. If any children are still running, it returns `PollPending` instead of
suspending or completing. The `Coro<T>` future — and all locals still in scope at that
suspension point — remains alive while the drain proceeds. When the last pending child
finishes it wakes the coroutine, which re-checks the list and resumes normal behaviour.

This is the async equivalent of `std::async`'s `std::future` blocking in its destructor:
rather than blocking the calling thread, we delay the next suspension point.

**Only tasks that are not `co_await`ed to completion need tracking.** A `JoinHandle` that is
fully awaited is destroyed after the task is already done — the registration is a no-op and
nothing is added to the pending list.

**`detach()` opts out.** `JoinHandle::detach()` clears `m_state` before the destructor body
runs, so the registration step is skipped entirely. Detached tasks are fire-and-forget and
do not block the coroutine.

### Cancellation propagation

When a coroutine's `poll()` is called with the cancellation flag set, it does **not** resume
the coroutine handle. Instead it:

1. Sets `t_current_coro` to this coroutine (as in every poll).
2. Calls `handle.destroy()`, which destructs all locals in LIFO order.
3. Each `JoinHandle` destructor fires while `t_current_coro` is active, cancels its child
   task, and registers the child's `TaskState` in this coroutine's pending list.
4. If the pending list is non-empty, returns `PollPending`.
5. When the last child completes it wakes the coroutine; the next poll sees an empty pending
   list and returns `PollDropped`.

Because cancelled futures are never dropped — they are always polled to `PollDropped` —
`t_current_coro` is guaranteed to be active during frame destruction. There is no path that
tears down a frame outside of a `poll()` call.

Cancellation propagates down the task tree automatically. Each child receives its cancelled
flag before its next poll, goes through the same destroy-and-drain sequence, and returns
`PollDropped` when its own children have cleared. The entire subtree drains without any
coroutine ever resuming after cancellation.

### How combinators (select, timeout) participate

A combinator that cancels a branch does not drop it. Instead it:

1. Marks the branch as cancelled.
2. Continues polling the cancelled branch alongside normal work.
3. Returns `PollDropped` to its caller only once all cancelled branches have themselves
   returned `PollDropped` (i.e., fully drained).

The combinator may have its winning result in hand before the drain completes; it holds that
result internally and delivers it to the caller only after cleanup is done.

### Protection guarantees and limitations

The mechanism protects locals that are **still in scope at the next suspension point**.
It does not protect objects that are destroyed between the `JoinHandle` drop and that
suspension point. If a child task holds a reference to an object that falls out of scope in
that window, the reference becomes dangling before the drain completes.

The `co_return` case is a notable exception: by the time `co_return` is reached, the
coroutine's locals have already been destroyed in LIFO order. The mechanism keeps the
`Coro<T>` future alive past `co_return` to drain children, but the referenced data is gone.
**`Synchronize` is the explicit solution for this case** — it adds a suspend point while
locals are still in scope, ensuring the drain completes before any referenced data is
destroyed:

```cpp
// Unsafe — local_data is destroyed before the child finishes
Coro<void> unsafe_example() {
    int local_data = 42;
    auto h = spawn(worker(&local_data)).submit();
    co_return;  // local_data destroyed here; drain wait is too late
}

// Fragile — the co_await acts as a drain point only if no exception is thrown
// between the JoinHandle drop and the co_await. An uncaught exception would
// unwind the stack and destroy local_data before the drain begins.
Coro<void> fragile_example() {
    int local_data = 42;
    auto h = spawn(worker(&local_data)).submit();
    // ...
    co_await some_other_work();  // drain completes here if we reach this point
    co_return;
}

// Safe — Synchronize provides a guaranteed suspend point while local_data is alive
Coro<void> safe_example() {
    int local_data = 42;
    co_await Synchronize([&](Synchronize& sync) -> Coro<void> {
        sync.spawn(worker(&local_data)).submit();
        co_return;
    });
    // local_data still alive here; Synchronize ensured worker finished
}
```

### Non-coroutine futures

A non-coroutine future only needs to actively participate in this mechanism if it directly
calls `spawn()` itself. If it doesn't spawn, the only relevant case is polling a child
`Coro<T>` that spawns tasks — and in that case the child coroutine manages its own drain
and only returns `PollDropped` once its frame is safe to free. The non-coroutine parent
propagates this transparently by continuing to poll until it sees `PollDropped`, which is
what any well-written future combinator already does.

The one requirement on non-coroutine futures that wrap other futures: they must not drop a
child early in their cancellation or error-handling paths. A future that drops its child
before it has drained would short-circuit the guarantee. Such futures need to treat a
cancelled child's `PollDropped` as the drain completion signal, not a normal result.

Non-coroutine futures that call `spawn()` directly would need an explicit mechanism —
registering with the current coroutine via `t_current_coro`, or managing their own pending
list. In practice this is expected to be rare: the natural place to spawn tasks is inside a
coroutine body, not inside a hand-written future.

**Comparison with Tokio:** Tokio does not have this problem because `tokio::spawn()` requires
`Send + 'static` — spawned tasks must own all their data, making it structurally impossible
for a spawned task to hold a reference into the spawning context. The type system enforces
the guarantee at compile time. The third-party `tokio_scoped` crate approaches what we are
building, but achieves safety by blocking the scope exit synchronously rather than via async
draining. Our implicit scope mechanism fills the gap that Rust closes with the borrow
checker, trading a compile-time guarantee for a runtime one.

### Interaction with explicit `Synchronize`

With every coroutine acting as an implicit scope, `Synchronize` is no longer the mechanism
that makes borrowed references safe. Its role shifts:

- As an **explicit mid-coroutine drain point** — `co_await Synchronize(...)` still makes
  sense when you want to wait for a batch of children at a specific point during execution
  (e.g., to collect their results or handle their exceptions) rather than only at the end.
- As a **safe spawn scope** for the `co_return` case — when locals must outlive a spawn that
  reaches `co_return` before a natural suspension point, `Synchronize` is the explicit opt-in.
- As a **named scope** that documents intent — even if the implicit guarantee makes it
  redundant for safety, an explicit scope communicates structure to the reader.

Whether `Synchronize` survives as a distinct primitive or is absorbed into the general
coroutine machinery is an open design question.

## Open Questions

1. **Fate of `Synchronize`.** Given the implicit scope guarantee, does `Synchronize` remain
   a first-class primitive, become a convenience alias, or get removed?
