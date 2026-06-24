# Signal Handling

## Overview

This document describes the design for async OS signal delivery (`SIGINT`, `SIGTERM`,
`SIGHUP`, etc.) built on libuv's `uv_signal_t`. It follows the same pattern as the
existing libuv I/O handles (`TcpListener` in particular): the handle is registered on
the uv thread via `with_context`, and a shared state struct bridges the uv callback to
a polling `Future` or `Stream`.

Two variants are provided, mirroring Tokio's `ctrl_c()` / `signal(SignalKind)` split
(see `doc/roadmap.md`):

- **`coro::signal(int signum)`** — a one-shot `Future<void>` that resolves on the next
  delivery of `signum`. Suitable for shutdown triggers where only the first occurrence
  matters.
- **`coro::signal_stream(std::initializer_list<int> signums)`** — a `Stream<SignalEvent>`
  that yields once per "batch" of deliveries of a watched signal that arrived since the
  last poll, coalesced into a single `{signum, count}` event rather than one wake per
  delivery. Suitable for reload-on-`SIGHUP` and similar repeat-signal use cases, or for
  watching several signals through one object.

```cpp
// Shutdown on SIGINT or SIGTERM, whichever arrives first:
co_await coro::select(coro::signal(SIGINT), coro::signal(SIGTERM));
begin_shutdown();

// Combined OS signal + internal shutdown channel:
co_await coro::select(coro::signal(SIGINT), shutdown_rx.changed());

// Reload config on every SIGHUP, exit cleanly on SIGINT/SIGTERM. If three SIGHUPs
// arrive in a burst before this loop gets back around to polling, item.count == 3
// for that one wake rather than three separate wakes:
auto reload = coro::signal_stream({SIGHUP});
auto shutdown = coro::signal(SIGINT);
while (true) {
    auto sel = co_await coro::select(coro::ref(reload), coro::ref(shutdown));
    if (auto* item = std::get_if<coro::SelectBranch<0, std::optional<coro::SignalEvent>>>(&sel)) {
        reload_config();  // item->value->count tells you how many SIGHUPs coalesced into this wake
        continue;
    }
    break;  // shutdown branch won
}
```

---

## Goals

- Provide a one-shot `Future<void>` (`coro::signal`) and a repeating `Stream<int>`
  (`coro::signal_stream`) over the same underlying libuv mechanism.
- Follow the established I/O handle pattern: registration happens on the uv thread via
  `with_context`/`spawn_on`; the consumer-facing `poll()`/`poll_next()` touches only
  shared, mutex-protected memory — no libuv calls outside the uv thread.
- Support watching multiple signal numbers from a single `signal_stream` instance.
- Clean, race-free teardown when the `Future`/`Stream` is dropped before (or after) a
  signal arrives, matching `TcpListener`'s destructor pattern.
- Surface a failing `uv_signal_start` (e.g. an invalid signal number) to the consumer as
  a `PollError` on the first `poll()`/`poll_next()` call, rather than silently dropping it
  — registration runs detached, so there is no `co_await`able call site to throw from.

## Non-Goals (Future Work)

- **Signal masking / blocking** — libuv's `uv_signal_t` only supports catching signals,
  not blocking delivery to the rest of the process. Not in scope.
- **Synchronous signal-safe handling** — this is firmly in the "deliver to a coroutine"
  camp; nothing here is async-signal-safe in the POSIX sense (libuv itself handles the
  signal-safety concerns internally via a self-pipe).
- **Recovering signals the kernel coalesced before libuv saw them** — for standard
  (non-realtime) signals, repeat deliveries that arrive while the previous one is still
  pending are merged by the kernel into a single notification, and that information is
  gone before it reaches user space at all. `SignalEvent::count` is therefore a lower
  bound, never an exact count, for those signal numbers. See "Delivery counting is a
  deliberate coalesce, not a per-delivery wake" below.
- **Per-thread signal scoping** — `uv_signal_t` is tied to the loop, not a thread; this
  matches existing libuv semantics and is not something this library changes.

---

## User-Facing API

```cpp
#include <coro/io/signal.h>

// One-shot: resolves once, on the next delivery of SIGINT.
co_await coro::signal(SIGINT);

// Stream: yields a coalesced {signum, count} event for every distinct signal
// (SIGHUP or SIGUSR1) that has fired at least once since the last poll.
auto sigs = coro::signal_stream({SIGHUP, SIGUSR1});
while (auto event = co_await coro::next(sigs)) {
    handle(event->signum, event->count);
}
```

`SignalEvent` is the item type yielded by `SignalStream`:

```cpp
struct SignalEvent {
    int      signum;
    uint64_t count;   // see "Delivery counting" below — a guaranteed lower bound,
                       // never an overcount, of how many times signum fired
};
```

`coro::signal(signum)` returns a `JoinHandle<void>`-shaped one-shot future (same shape
as `TcpListener::bind`/`accept`); dropping it before the signal arrives cancels the uv
registration cleanly. It deliberately carries no count — only "has it fired yet"
matters for the one-shot case.

`coro::signal_stream(signums)` returns a `SignalStream`, which satisfies the `Stream`
concept (`ItemType = SignalEvent`). It never naturally exhausts — `poll_next()` only
ever returns `Pending` or `Ready(some(event))` — exhaustion (`Ready(nullopt)`) only
happens if the stream is explicitly closed (see Teardown below). In practice, most
callers just let the `Stream` live for the program's duration.

---

## Architecture

### Why two different mechanisms for setup vs. consumption

Like `TcpListener`, registering a `uv_signal_t` (`uv_signal_init` / `uv_signal_start` /
`uv_signal_stop` / `uv_close`) **must** run on the uv thread. But once registered,
*consuming* delivered signals does not need to touch libuv at all — it only needs to
read/write a small piece of shared, mutex-protected state. This mirrors `coro::Event`:
the producer (here, the uv callback) and consumer (here, `poll`/`poll_next`) communicate
purely through a mutex-guarded struct; no executor hop is needed per signal.

So:
- **Setup/teardown** (`uv_signal_init`/`start`/`stop`/`close`) — runs once via
  `with_context(uv_executor, ...)`, exactly like `TcpListener::bind`.
- **Per-event consumption** (`poll`/`poll_next`) — direct, lock-protected access to
  shared state, exactly like `Event::WaitFuture::poll()`. No `with_context` round-trip
  per signal.

### Shared state

```cpp
struct SignalState {
    detail::Mutex                  mutex;
    std::deque<SignalEvent>        pending;      // one coalesced entry per distinct signum
                                                  // with an unconsumed delivery, FIFO by
                                                  // order of first occurrence since drain
    detail::Rc<detail::Waker>      waker;        // consumer's waker, if parked
    bool                           closed = false;
    int                            setup_error = 0; // 0 = ok, else a uv error code from
                                                     // uv_signal_start; surfaced as PollError
    std::deque<uv_signal_t>        handles;      // one per watched signum; stable addresses
};
```

`pending` holds **at most one entry per watched signal number** at any time — a second
delivery of a signum already queued increments that entry's `count` in place rather than
appending a new entry. This is the coalescing behaviour: `signal_cb` firing three times
for `SIGHUP` before the consumer polls produces one `{SIGHUP, 3}` entry and one wake
call, not three entries and three wakes. A burst of `SIGHUP` followed by `SIGUSR1`
produces two entries (one per distinct signum), each independently coalesced.

`handles` uses `std::deque` rather than `std::vector` so element addresses stay stable
across growth — handle addresses are captured in `handle.data` as soon as each
`uv_signal_t` is initialized, before the loop moves on to the next requested signum, so
a reallocating container would invalidate an already-captured address.

`setup_error` records a failing `uv_signal_start` call (effectively only possible with an
invalid signal number). Setup runs detached — `signal()`/`signal_stream()` return
immediately, before registration completes — so there is no `co_await`-able call site to
throw from; the error is stashed here instead and surfaced as a `PollError` the first time
the consumer calls `poll()`/`poll_next()`.

Both `coro::signal` (one signal, one delivery) and `SignalStream` (N signals, unbounded
deliveries) share this same struct shape — `coro::signal`'s `SignalFuture` is simply a
`SignalStream` reduced to one watched signal number and a "first item only" `poll()`.

### `signal_cb` — uv thread only

```cpp
void signal_cb(uv_signal_t* handle, int signum) {
    auto* state = static_cast<std::shared_ptr<SignalState>*>(handle->data)->get();
    detail::Rc<detail::Waker> waker;
    {
        std::lock_guard lock(state->mutex);
        if (state->closed) return;
        // Coalesce: if signum already has an unconsumed entry, bump its count
        // in place instead of queuing a second entry. Linear scan is fine — the
        // number of distinct watched signals is always small (a handful at most).
        auto it = std::find_if(state->pending.begin(), state->pending.end(),
                                [signum](const SignalEvent& e) { return e.signum == signum; });
        if (it != state->pending.end()) {
            ++it->count;
        } else {
            state->pending.push_back(SignalEvent{signum, 1});
        }
        waker = std::exchange(state->waker, nullptr);
    }
    if (waker) waker->wake();
}
```

Exactly one `signal_cb` is shared across all watched `uv_signal_t` handles in a given
`SignalState` — `handle->data` (a heap-allocated `shared_ptr<SignalState>` wrapper,
matching the `TcpListener::ListenHandle` pattern) is all the callback needs; it does not
need to know which handle fired since `signum` is passed directly by libuv.

### `poll_next()` — any thread, no libuv calls

```cpp
PollResult<std::optional<SignalEvent>> SignalStream::poll_next(detail::Context& ctx) {
    std::lock_guard lock(m_state->mutex);
    if (m_state->setup_error != 0) {
        return PollError(/* wraps setup_error as a std::system_error */);
    }
    if (!m_state->pending.empty()) {
        SignalEvent event = m_state->pending.front();
        m_state->pending.pop_front();
        return std::optional<SignalEvent>(event);
    }
    if (m_state->closed) {
        return std::optional<SignalEvent>(std::nullopt);
    }
    m_state->waker = ctx.getWaker();
    return PollPending;
}
```

This is identical in shape to `ChannelStream::poll_next` in
[future_and_stream.md](future_and_stream.md) — `SignalStream` is, structurally, a
single-producer multi-signal channel where the "producer" is libuv rather than another
coroutine.

`SignalFuture::poll()` (the one-shot variant backing `coro::signal`) shares the exact same
`SignalState` shape as `SignalStream` — including the `pending` deque — rather than a
separate boolean flag. It checks `setup_error` the same way, and treats "has `pending`
acquired any entry yet" as "delivered," since for a single watched signum the deque never
holds more than one entry:

```cpp
PollResult<void> SignalFuture::poll(detail::Context& ctx) {
    std::lock_guard lock(m_state->mutex);
    if (m_state->setup_error != 0) {
        return PollError(/* wraps setup_error as a std::system_error */);
    }
    if (!m_state->pending.empty()) return PollReady;
    m_state->waker = ctx.getWaker();
    return PollPending;
}
```

### Delivery counting is a deliberate coalesce, not a per-delivery wake

Earlier drafts of this design queued one entry per `signal_cb` invocation, so a burst of
three `SIGHUP`s before the consumer polled would produce three separate wakes and three
yielded items. **This is intentionally not what the implementation does.** There is no
reason to wake the consumer once per delivery when the consumer cannot distinguish "woken
3 times" from "woken once, told the count was 3" — the latter is strictly more efficient
and loses no information *that we could have given the consumer anyway* (see below for
what's lost upstream, which neither design recovers). `signal_cb` therefore coalesces
same-signum deliveries into a single queued `SignalEvent{signum, count}` and triggers
**one** wake per distinct signum that transitions from "not pending" to "pending,"
regardless of how many times it actually fired in that window.

`coro::signal()` (the one-shot `Future`) does not need a count at all — it only resolves
once, on the first delivery, and is torn down immediately after.

!!! warning "WARNING: `count` is a guaranteed lower bound, never an exact count and never an overcount"
    Two distinct layers can each lose information about how many times a signal was
    *actually* sent, and `SignalEvent::count` can only reflect what survives both:

    1. **Kernel → libuv.** For standard (non-realtime) POSIX signals, the kernel does
       not queue repeat instances of the same signal number — if a signal is sent again
       while the previous instance is still pending (e.g. blocked during handler
       execution, the default unless `SA_NODEFER` is set), the kernel collapses the two
       into a single pending notification. `signal_cb` only ever sees what the kernel
       actually delivers, so `kill(pid, SIGHUP)` called three times in quick succession
       may invoke `signal_cb` only once or twice. This loss happens before any of our
       code runs and is undetectable from user space.
    2. **libuv → our queue.** We *additionally* coalesce on purpose, as described above:
       multiple `signal_cb` invocations for the same signum between polls are merged
       into one queued event with an incremented `count`. Unlike layer 1, this loss is
       intentional and the information (the count) is preserved exactly across this
       layer — coalescing here loses no information that layer 1 didn't already risk
       losing.

    The net guarantee, stated precisely: **the first occurrence of a signal is always
    counted** (the kernel never drops the very first pending instance, and we never drop
    a queued entry). After that, `SignalEvent::count` is **a lower bound** on how many
    times the signal was actually triggered — the real number may be *higher* than
    `count` (due to kernel-level coalescing in layer 1) but is **never lower**. Treat
    `count` as "at least this many times," not "exactly this many times."

    **Real-time signals (`SIGRTMIN`..`SIGRTMAX`) avoid layer-1 loss** — POSIX guarantees
    the kernel queues each one separately, so for those signal numbers `count` reflects
    every `kill()`/`sigqueue()` call exactly (layer 2 coalescing still applies, but
    nothing was lost before it, so the sum is exact).

Layer 1 is a property of POSIX signal semantics, not a limitation of this library —
any signal-handling layer (including raw `sigaction` in C) is subject to the same
kernel-level coalescing for standard signals. Layer 2 is this library's own design
choice, made because it is strictly better than the per-delivery-wake alternative with
no additional information loss.

!!! tip "TODO: make layer-2 coalescing optional"
    `poll_next()` could be changed to decrement `pending.front().count` and only
    `pop_front()` once it reaches zero, yielding one `SignalEvent{signum, 1}` per actual
    `signal_cb` invocation instead of one coalesced batch — `signal_cb` itself would not
    need to change at all, since it already coalesces under the mutex regardless of how
    the consumer drains. This could be exposed as a `SignalStream` mode (e.g. a
    `CoalesceMode::Batch` vs. `CoalesceMode::PerOccurrence` template/constructor
    parameter). Not done in this iteration — deferred because it would need its own
    test coverage and the batch-coalesced behavior already covers the motivating use
    cases (shutdown trigger, reload-on-signal).

### Setup — `coro::signal` / `coro::signal_stream`

Both entry points share one `register_handles` coroutine that loops `uv_signal_init`/
`uv_signal_start` over the requested signal numbers, all sharing one `signal_cb` and one
`SignalState`. Registration is scheduled via `with_context(exec, ...)` and immediately
`.detach()`ed — `signal()`/`signal_stream()` return the `Future`/`Stream` to the caller
right away, without waiting for registration to finish on the uv thread:

```cpp
Coro<void> register_handles(std::shared_ptr<SignalState> state,
                            SingleThreadedUvExecutor& exec,
                            std::vector<int> signums) {
    for (int signum : signums) {
        state->handles.emplace_back();
        uv_signal_t& h = state->handles.back();
        uv_signal_init(exec.loop(), &h);
        h.data = new std::shared_ptr<SignalState>(state);
        if (int r = uv_signal_start(&h, signal_cb, signum); r != 0) {
            // Record the failure under the mutex and wake any parked consumer —
            // poll()/poll_next() surface it as a PollError on their next call.
            std::lock_guard lock(state->mutex);
            state->setup_error = r;
            break;
        }
    }
    co_return;
}

[[nodiscard]] SignalFuture coro::signal(int signum) {
    auto state = std::make_shared<SignalState>();
    auto& exec = current_uv_executor();
    with_context(exec, register_handles(state, exec, {signum})).detach();
    return SignalFuture(std::move(state), &exec);
}
```

Because setup is detached rather than awaited, `SignalFuture`/`SignalStream` do not hold
a `JoinHandle` for the registration task at all — there is nothing to keep alive once
`register_handles` has populated `state->handles`; `poll()`/`poll_next()` only ever touch
`state` directly, under the mutex, regardless of whether registration has completed yet
(a poll before registration finishes simply observes an empty `pending` and parks, exactly
as if no signal had arrived yet).

`signal_stream` is the same function with more than one requested signum.

### Teardown

Mirrors `TcpListener::~TcpListener()`: schedule a `with_context` coroutine that marks
`closed = true`, wakes any parked waker (with `Ready(nullopt)` for the stream, or simply
abandoning the wait for the one-shot future — its `JoinHandle`-like wrapper is being
destroyed anyway so nobody observes the result), then `uv_signal_stop` + `uv_close` each
handle, deleting the heap `shared_ptr` wrapper from `handle->data` just before
`uv_close`, exactly as `TcpListener` does.

```mermaid
sequenceDiagram
    participant App as Caller (compute executor)
    participant SS as SignalStream
    participant UVE as SingleThreadedUvExecutor
    participant UV as libuv

    App->>SS: coro::signal_stream({SIGHUP, SIGUSR1})
    SS->>UVE: with_context: uv_signal_init/start x2
    UVE->>UV: uv_signal_start(&h1, signal_cb, SIGHUP)
    UVE->>UV: uv_signal_start(&h2, signal_cb, SIGUSR1)

    App->>SS: co_await next(stream)
    SS-->>App: PollPending (waker stored in SignalState)

    Note over UV: process receives SIGHUP three times in a burst
    UV->>UVE: signal_cb(h1, SIGHUP) #1 — on uv thread
    UVE->>SS: pending = [{SIGHUP, 1}]; take + wake stored waker
    UV->>UVE: signal_cb(h1, SIGHUP) #2 — on uv thread
    UVE->>SS: pending = [{SIGHUP, 2}]; no waker to wake (already consumed)
    UV->>UVE: signal_cb(h1, SIGHUP) #3 — on uv thread
    UVE->>SS: pending = [{SIGHUP, 3}]; no waker to wake (already consumed)
    App->>SS: poll_next() → Ready({SIGHUP, 3}) — one wake, one item, count=3

    App->>SS: ~SignalStream() (drop)
    SS->>UVE: with_context: closed=true; uv_signal_stop/close x2
    UVE->>UV: uv_close(&h1, ...) / uv_close(&h2, ...)
```

---

## Race Conditions

**`SignalState::pending` / `waker` concurrent read/write** — `signal_cb` (uv thread)
pushes onto `pending` and reads/clears `waker`; `poll_next()` (consumer's executor
thread) reads `pending` and writes `waker`. Resolved by `detail::Mutex` guarding both
fields together, following the project convention of preferring mutexes over atomics
(see `CLAUDE.md`) and the same pattern as `Event`.

**Lost wakeup between `pending.empty()` check and waker registration** — exactly the
same RACE CONDITION NOTE as `Event::WaitFuture::poll()`: the waker must be stored
*inside* the same critical section as the `pending`/`closed` check, otherwise a
`signal_cb` firing between the check and the store would push to `pending` and find no
waker to wake, and the consumer would suspend having missed the wakeup. Resolved by
performing the check-and-register atomically under `m_state->mutex`.

**Destruction while a signal_cb is in flight** — `closed` is read at the top of
`signal_cb` under the mutex; once `closed = true` is set (also under the mutex) by the
teardown coroutine, any subsequent `signal_cb` invocation is a safe no-op. Since
`uv_signal_stop`/`uv_close` also run on the uv thread (the same thread `signal_cb` runs
on), there is no concurrent-callback window once teardown begins — this is simpler than
`TcpListener`'s `accept_notify` cancellation because there is no "one in-flight Future
to cancel with an error code", only a queue to stop feeding.

**`uv_close` callback firing after `SignalState` destruction** — same as every other
libuv handle in this codebase: a heap-allocated `shared_ptr<SignalState>` wrapper is
stored in each `handle->data`, deleted just before `uv_close` is issued for that handle,
keeping the close callback's pointer access (if any) safe. Since teardown doesn't need
the close callback to do anything beyond freeing libuv resources (no consumer is woken
by it — `closed` was already set), this can use a no-op close callback in the simplest
case, or follow `TcpListener`'s `co_await wait(result)` pattern if the teardown
coroutine wants to wait for `uv_close` to actually finish before returning.

---

## Module Placement

`include/coro/io/signal.h` / `src/io/signal.cpp` — alongside `tcp_listener.h`, since
`SignalStream`/`SignalFuture` are built directly on `SingleThreadedUvExecutor` and
`with_context`, following the same registration-then-poll shape as the other `io/`
types, even though the resource being watched (OS signals) isn't a network socket.

```cpp
namespace coro {

[[nodiscard]] SignalFuture signal(int signum);
[[nodiscard]] SignalStream signal_stream(std::initializer_list<int> signums);

} // namespace coro
```

---

## Open Questions

- ~~**Should `coro::signal` share an implementation with `SignalStream`?**~~ Resolved:
  implemented as two sibling types sharing the same `SignalState`/`signal_cb`, with
  `SignalFuture::poll()` simply checking `pending.empty()` instead of draining a
  per-item count. No `NextFuture<SignalStream>` indirection needed.
- ~~**Multiple independent watchers for the same signal number**~~ Resolved: covered by
  `SignalFutureTest.MultipleIndependentWatchersOfSameSignal` in `test/io/test_signal.cpp`
  — two separate `coro::signal(SIGUSR1)` calls each get their own delivery, as expected
  from libuv's per-handle semantics.

---

## Status

Implemented — `include/coro/io/signal.h` / `src/io/signal.cpp`, tested in
`test/io/test_signal.cpp`. See `doc/roadmap.md` ("Signal handling") for the original
scope note; this document remains the canonical design reference.
