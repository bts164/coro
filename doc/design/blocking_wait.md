# `blocking_wait` / `blocking_next` — driving a `Future`/`Stream` from a blocking thread

!!! note "Implemented"
    `blocking_wait` lives in `coro/future.h`, `blocking_next` in `coro/stream.h`, both
    guarded `#ifndef CORO_PICO`. Tests: `test/test_future.cpp` (`BlockingWaitTest`) and
    `test/test_stream.cpp` (`BlockingNextTest`).

## Motivation

Any OS thread that already has `current_runtime()` set — today, that means a
[`spawn_blocking`](spawn_blocking.md) pool thread, and in the future any thread that has
called the proposed `Runtime::enter()` (see [task_and_executor.md](task_and_executor.md)'s
"Runtime" section) — is a valid place to synchronously drive a `Future` or `Stream` to
completion. There is currently no generic way to do this. The nested-`Runtime` pattern (see
[spawn_blocking.md](spawn_blocking.md)'s "Running a Future on a blocking thread") gives a
blocking-pool callable a fully independent async environment — its own executor, its own
`IoDriver`, complete isolation. That isolation is the *point* when it's needed, but it is
also unavoidable overhead when it is not: a compute loop that just wants to drain a
`Stream<T>` (an `MpscReceiver<T>`, or a `CoroStream<T>` async generator) one item at a time
has no need for a second reactor — it already has one available via the ambient thread-local
context.

`blocking_wait`/`blocking_next` are the lightweight alternative: a single-future driver with
no executor, no task allocation, and no ready queue — it polls exactly one `Future` (or, via
`next()`, one `Stream` item) to completion on the calling thread, reusing whatever runtime
context is already ambient there. This is the same role Tokio's `Handle::block_on` plays
relative to spinning up a second `tokio::Runtime`.

Although the concrete use in mind today is a `spawn_blocking` compute loop generalized over
`Stream<T>` (see the example below), `blocking_wait`/`blocking_next` are not
`spawn_blocking`-specific: the goal is a primitive usable from any thread with an active
runtime context, which is why this design lives in its own document rather than folded into
`spawn_blocking.md`.

## API

```cpp
namespace coro {

// Polls `future` to completion on the calling thread, blocking between polls.
// Returns the future's value, or rethrows its exception.
template<Future F>
typename F::OutputType blocking_wait(F future);

// Equivalent to blocking_wait(coro::next(stream)) — pulls exactly one item.
// Returns nullopt (or false, for void streams) once the stream is exhausted —
// the same detail::StreamItem<T> mapping next()/NextFuture use.
template<Stream S>
detail::StreamItem<typename S::ItemType> blocking_next(S& stream);

} // namespace coro
```

Typical use: a `spawn_blocking` compute loop generalized over any `Stream<T>` source —

```cpp
template<Stream S>
void run_compute_loop(S stream) {
    while (auto item = coro::blocking_next(stream)) {
        process(*item);
    }
}
```

This works identically whether `S` is an `MpscReceiver<T>` (which already has a
purpose-built `blocking_recv()` — `blocking_next` is the generic version of the same idea)
or a `CoroStream<T>` async generator, which has no blocking-consumption path of its own
today.

## Design

`blocking_wait` builds a minimal, self-contained `Waker` for the duration of the call — a
mutex, a condition variable, and a "ready" flag — wraps it in a `Context`, and loop-polls:

```cpp
template<Future F>
typename F::OutputType blocking_wait(F future) {
    auto waker = detail::make_rc<detail::BlockingWaker>();  // owns mutex + cv + flag
    detail::Context ctx(waker->clone());
    for (;;) {
        auto r = future.poll(ctx);
        if (!r.isPending()) {
            r.rethrowIfError();
            if constexpr (std::is_void_v<typename F::OutputType>) return;
            else return std::move(r).value();
        }
        waker->wait_for_wake();  // condvar-wait; BlockingWaker::wake() sets the flag and notifies
    }
}
```

This needs nothing from `Executor` or `Runtime` — `Context`/`Waker` are already just an
abstract notification interface (see
[waker_and_context_propagation.md](waker_and_context_propagation.md)), so a condvar-backed
`Waker` is a complete, correct implementation on its own. `blocking_wait` never touches the
ready queue, the owned-task list, or the I/O driver directly. This is the design's central
property: it has no dependency on where it's called from, which is what makes it usable
beyond `spawn_blocking` threads specifically — anywhere `current_runtime()` is valid works,
present or future call sites alike.

## Excluded on `CORO_PICO` — for now

`blocking_wait`/`blocking_next` are compiled out entirely under `CORO_PICO` (`#ifndef
CORO_PICO` around both, in `future.h`/`stream.h`; `detail/blocking_waker.h` is guarded the
same way). This is **not** a judgment that parking-until-woken has no use case on Pico —
it's that the current backend, `detail::BlockingWaker`, is built on `std::mutex`/
`std::condition_variable`, which require an OS thread scheduler that bare-metal RP2040
builds don't have. The exclusion tracks what the implementation can support today, not a
claim about the primitive itself.

A concrete use case the exclusion rules out: RP2040/RP2350 are dual-core, so core 1 could
run a blocking compute loop while core 0 runs the `CurrentThreadExecutor` loop — core 1
parking via `blocking_wait` until core 0 (or an ISR) has work for it is the same shape as
the desktop `spawn_blocking`-thread case, just with cores instead of OS threads. Supporting
it would mean a second `Waker` backend selected under `CORO_PICO` — not condvar-based, but
built on the same **SIO inter-processor FIFO doorbell** (`sio_hw->fifo_wr` →
`SIO_IRQ_PROC0`) already sketched out as the cross-core wake mechanism for the `__wfi()`
idle-executor TODO in [pico_port.md](pico_port.md#busy-poll-loop-no-cpu-idle-wfi) (see
"Dual-core implementation (requires doorbell)"). That backend has its own race to get right — the
check-then-sleep atomicity the WFI TODO already calls out — which is why this is being
deferred as its own design discussion rather than folded into this exclusion silently.

## Contract: an active `Runtime` is required for reactor-touching futures

`blocking_wait`/`blocking_next` do not set up `current_runtime()` themselves — they inherit
whatever the calling thread already has. Concretely:

- On a `spawn_blocking` thread: it is already set (see
  [spawn_blocking.md](spawn_blocking.md)'s "Thread-local runtime context"), so any future —
  including one that awaits a timer, a socket, or spawns child tasks — can be driven with
  `blocking_wait` with no extra setup.
- On a thread with no active `Runtime` at all (a thread the application created itself, not
  via `spawn_blocking`): `current_runtime()` throws `std::runtime_error` when called, so a
  future that never touches it (pure in-memory work — draining an `MpscReceiver`, awaiting
  a `oneshot`, a hand-written `CoroStream` that only ever `co_await`s channel `recv()`s)
  still works, but one that does throws through `blocking_wait` exactly as it would through
  `co_await` anywhere else without a `Runtime`. Use the proposed `Runtime::enter()` (see
  [task_and_executor.md](task_and_executor.md)'s "Runtime" section) to give such a thread
  valid context first.

`blocking_wait` deliberately does not paper over this by silently constructing a throwaway
`Runtime`/reactor on first use — that would hide a real cost (a second event loop) behind an
innocuous-looking function call, the same reason the nested-`Runtime` pattern in
`spawn_blocking.md` requires writing out `coro::Runtime inner(1)` explicitly at the call
site.

## Comparison with alternatives

| Approach | Pros | Cons |
|---|---|---|
| `blocking_wait`/`blocking_next` (this doc) | No second reactor/executor allocation; works with any `Future`/`Stream`; usable from any thread with an active runtime context | Requires an already-active `Runtime` context (ambient on `spawn_blocking` threads, or via the proposed `Runtime::enter()`) for futures that touch the reactor |
| Nested `Runtime` + `block_on` (see [spawn_blocking.md](spawn_blocking.md)) | Fully isolated reactor; no ambient-context requirement | Full second executor + `IoDriver` (epoll fd, eventfd) allocated per call |
| `MpscReceiver::blocking_recv()` | Already implemented, purpose-built | Bespoke to one channel type — `blocking_next` is the generic version of the same idea |

## Placement

`blocking_wait` belongs in `coro/future.h`, next to the `Future` concept itself.
`future.h` already holds the pure poll-based machinery that has no dependency on any
specific task type or executor (`Cancellable`, `Ref`) — `blocking_wait` fits the same
shape: it operates only through `Context`/`Waker`, nothing else.

`blocking_next` belongs in `coro/stream.h`, next to `next()`/`NextFuture`, which it
mirrors directly — `blocking_next(stream)` is `blocking_wait(next(stream))` with a
stream-shaped return type, the same relationship `next()` already has to `Future`.

`coro/task` (alongside `spawn_blocking` and `fiber`) and `coro/sync` were considered and
ruled out: both `blocking_wait` and `blocking_next` are meant to be usable independent of
any specific task type, and neither is a synchronization primitive between tasks — they're
polling drivers, which is what makes `future.h`/`stream.h` the correct fit.
