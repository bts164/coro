# GpioPin — async GPIO edge/level events (RP2040)

`coro::pico::hal::GpioPin` (`include/coro/pico/hal/gpio.h` /
`src/pico/hal/gpio.cpp`) is an RAII, per-pin async GPIO primitive supporting
edge waits, level waits, and a lossless edge stream.

## Problem

RP2040 has one shared GPIO IRQ per core, but individual pins are
independently addressable — the same shape of problem
`coro::pico::hal::AsyncDmaTransfer` already solved properly for DMA
channels. A per-pin async primitive needs to dispatch a firing IRQ to the
right waiter without restricting callers to a single live instance.

It also needs to get two easy-to-misjudge race conditions right once,
inside the primitive, rather than leaving every caller to reason about them
independently: waiting for a level that may already be satisfied when the
wait begins (not just a future transition into it), and arming an interrupt
strictly before whatever synchronous action might produce the event it's
waiting for.

## Design

A `coro::pico::hal::GpioPin` type, in `include/coro/pico/hal/gpio.h` /
`src/pico/hal/gpio.cpp`, following `dma.h`'s precedent directly:

- RAII: constructor takes a pin number (and direction/pull config), registers
  itself in a **pin-indexed dispatch table** of `GpioPin*` (a fixed-size
  array — RP2040 has only 30 usable GPIOs, so no dynamic allocation is
  needed, the same shape as `AsyncDmaTransfer`'s channel-indexed table), and
  installs the shared `gpio_irq_handler` once (on first construction) via
  `gpio_set_irq_enabled_with_callback`. Destructor disables the IRQ for its
  pin and clears its dispatch table slot.
- No more "only one instance" caveat — any number of `GpioPin`s can coexist,
  each waiting on its own pin, same as multiple `AsyncDmaTransfer`s waiting on
  their own channels. The constructor throws `std::logic_error` if the
  requested pin is already claimed by another live `GpioPin`, so a double
  claim fails loudly instead of silently clobbering the dispatch table entry.
  This check only guards against two `GpioPin`s on the same pin — it cannot
  detect, and does not attempt to detect, a pin already driven by an
  unrelated peripheral configured directly via the SDK (SPI, PWM, ADC,
  UART, ...), since the Pico SDK has no cross-peripheral pin-ownership
  registry to check against. Avoiding that kind of conflict remains the
  caller's responsibility.
- Any number of `GpioPin`s can also be waited on concurrently by multiple
  coroutines on the *same* instance — e.g. two coroutines both
  `co_await`-ing `wait_for_edge()` on one pin. This is supported for free by
  the executor's ISR poll registration (see `doc/design/isr_safety.md`'s
  "Multiple waiters" section: each waiter registers itself with the executor
  keyed by its own object identity, so any number of waiters can share one
  underlying flag/count). `wait_for_edge()`/`wait_for_level()` are backed by
  `IsrEvent`-shaped broadcast semantics, so every concurrent waiter sees the
  same edge/level; `edges()` is backed by `IsrSemaphore`, so each concurrent
  `co_await` on the stream claims one drained count rather than all of them
  seeing the same edge. One consequence of the broadcast semantics: waiters
  with genuinely different interests on one instance (e.g. one waiting for a
  rising edge, another for falling) can spuriously wake each other, since
  there is no per-mask routing — see the `NOTE(race)` comment in
  `GpioPin::notify_irq()`.
- `s_dispatch[]` (the file-scope pin → `GpioPin*` table `gpio_irq_handler()`
  uses to route a firing IRQ) is guarded by a dedicated striped spin lock,
  since nothing restricts `GpioPin` construction/destruction to a single
  core — an unguarded plain pointer read/write across cores would be a
  genuine data race. The lock is released, however, before `notify_irq()` is
  called on the looked-up pointer, because holding one striped spin lock
  while another is acquired inside `notify_irq()` (via `m_dir_lock`/
  `IsrEvent`/`IsrSemaphore`) risks a same-core self-deadlock if the two
  happen to alias the same underlying hardware lock — the striped pool's
  contract forbids nesting them. This leaves a narrow window where a
  `GpioPin` destructed on another core between the lookup and the call is a
  use-after-free. Closing it fully would need a dedicated (non-striped) lock
  held for the whole dispatch call, or reference-counting `GpioPin` — not
  implemented, since this project's intended usage does not construct or
  destruct `GpioPin`s from a core other than the one servicing their IRQs.
  See the `NOTE(race)` comments on `gpio_irq_handler()` and
  `GpioPin::~GpioPin()` in `src/pico/hal/gpio.cpp`.
- Debouncing is not built into `edges()` (or any other method) — a caller
  that needs it applies the same level-based release check it would today,
  outside `GpioPin`. `wait_for_level()` doesn't change this: it solves the
  missed-level race, not contact bounce.
- `wait_for_level()` (and its `wait_for_high()`/`wait_for_low()` shorthands)
  are level primitives, not edge primitives dressed up to look like one: RP2040's
  GPIO IRQ block supports `GPIO_IRQ_LEVEL_LOW`/`GPIO_IRQ_LEVEL_HIGH` alongside
  the edge triggers natively, so "the level is already what I want" is not a
  case the caller ever has to detect and special-case — the IRQ itself either
  fires immediately (level already satisfied) or fires when the level is
  reached, with no gap between "check" and "arm" for a caller to fall into.
  Read-then-conditionally-arm-an-edge is exactly the sequence
  `wait_for_level()` performs internally, proven correct once instead of
  re-derived at each call site that needs to wait on a level rather than a
  transition.
- **`edges()` is lossless, not a convenience loop around `wait_for_edge()`.**
  Repeatedly `co_await`-ing `wait_for_edge()` only ever sees the *next* edge —
  any that occur while the caller isn't currently waiting are dropped, same
  as today's hand-rolled `IsrEvent`-based code. `edges()` instead queues
  every matching edge as it happens and drains them in order, so a consumer
  that falls behind still eventually sees all of them rather than silently
  missing whichever occurred while it was busy. This requires a different
  underlying primitive — `coro::IsrSemaphore` (a saturating ISR-side counter,
  see `doc/design/isr_safety.md`) rather than `IsrEvent`'s single flag, since
  a flag by definition can't represent "more than one pending." For
  `Edge::Any`, the direction of each drained item doesn't need to be stored
  by the ISR at all: edges necessarily alternate polarity, so the sequence
  of directions is fully recoverable from the pending count's parity plus
  the one bit of level state remembered as of the most recent edge —
  `IsrSemaphore` itself stays generic (a bare count), and this reconstruction
  is `GpioPin`'s own logic on top of it.

```cpp
class GpioPin {
public:
    GpioPin(uint pin, Direction dir, Pull pull = Pull::None);
    ~GpioPin();

    // Suspends until the pin transitions to `edge`'s polarity — a
    // transition that must still occur after this call, regardless of the
    // pin's level right now. If the pin is already at the target level,
    // this does NOT return immediately: it waits for the pin to leave that
    // level and then return to it. Edge::Any resolves on either transition.
    //
    // Arms the IRQ synchronously, before this call returns -- not on first
    // co_await. See "Eager arming" below.
    [[nodiscard]] GpioEdgeFuture wait_for_edge(Edge edge);

    // First checks the pin's current level directly (a plain read, no ISR
    // involved) and resolves immediately if it already matches `level` --
    // no interrupt is ever armed for this case. Only if it doesn't match
    // does this fall through to arming a wait, at which point it behaves
    // exactly like wait_for_edge() for the corresponding polarity --
    // internally the same arm-then-suspend helper, parameterized by
    // trigger polarity (see implementation note below).
    // wait_for_level() and wait_for_edge() therefore only differ in the
    // "already there" case.
    [[nodiscard]] GpioEdgeFuture wait_for_level(bool level);
    [[nodiscard]] GpioEdgeFuture wait_for_high() { return wait_for_level(true); }
    [[nodiscard]] GpioEdgeFuture wait_for_low()  { return wait_for_level(false); }

    // Async generator of every edge that occurs, in order — including ones
    // that occur while nobody is co_await-ing the stream. Backed by
    // coro::IsrSemaphore (see doc/design/isr_safety.md), not IsrEvent: the
    // ISR increments a saturating count on each matching edge, and each
    // stream item drains one count. This differs from wait_for_edge() in a
    // loop, which only sees the next edge and drops everything in between.
    [[nodiscard]] CoroStream<Edge> edges(Edge edge);   // Edge::Any included

    bool read() const;   // direct level read, no async involved
    void write(bool level);
};
```

## Eager arming: `GpioEdgeFuture` replaces `Coro<void>`

`wait_for_edge()`/`wait_for_level()`/`wait_for_high()`/`wait_for_low()` return
a custom future type, `GpioEdgeFuture`, rather than a lazily-started
`Coro<void>`.

The problem `GpioEdgeFuture` solves: some callers need the IRQ to be armed
*before* triggering some other, non-coroutine action that might produce the
event they're waiting for — e.g. issuing a strobe or command that can
complete and produce the event before a caller ever gets back around to
awaiting it. A lazily-started coroutine can't give that guarantee: none of
its body — including the `gpio_set_irq_enabled()` call — runs until the
caller `co_await`s the returned handle, which is necessarily after the
triggering action already happened. A synchronous "arm" call paired with a
separate lazy "wait" call could close that race, but only by handing the
caller two calls to sequence correctly instead of one, with no way for the
type system to catch a caller that forgets the second call after arming the
first.

`GpioEdgeFuture`'s constructor runs synchronously, as part of the
`wait_for_edge()`/`wait_for_level()` call itself: it captures the `IsrEvent`
epoch baseline and calls `gpio_set_irq_enabled()` before the function
returns, not on first poll. The caller can therefore:

```cpp
auto irq_falling = m_irq_pin.wait_for_edge(Edge::Falling);  // armed now
trigger_event();                                            // trigger the event
co_await irq_falling;                                       // defer the wait
```

with no separate token to thread through and no way to forget the second
call — there is no second call. The common case, `co_await
m_irq_pin.wait_for_low();`, still works exactly as before: a `GpioEdgeFuture`
temporary is just as `co_await`-able as the `Coro<void>` it replaces.

!!! note "How this compares to Rust's embedded-hal-async"
    Researched as a design reference before committing to this shape. Rust's
    `async fn` is lazy in the same way `Coro<void>` is — none of an `async
    fn`'s body runs until its returned future is first polled, so the public
    `embedded_hal_async::digital::Wait` trait cannot itself expose "arm at
    call time, defer the await." Embassy's own RP2040 HAL (`embassy_rp::gpio`)
    solves the identical problem internally: its `InputFuture::new()` is a
    plain, non-`async` constructor that arms the interrupt synchronously —
    structurally the same move `GpioEdgeFuture`'s constructor makes — but it
    is a private implementation detail wrapped by an `async fn` before being
    exposed via the trait, so callers of the public Rust interface don't get
    the deferred-await option. There is no documented idiomatic
    embedded-hal-async pattern for this; `GpioEdgeFuture` exposes directly,
    as a public capability, what Embassy only uses internally.

!!! warning "WARNING: `GpioEdgeFuture` holds a non-owning reference to its `GpioPin` — it must not outlive it"
    `GpioEdgeFuture` stores a pointer back into the `GpioPin` (and its
    `IsrEvent`) it was created from; it does not extend that object's
    lifetime in any way. The intended, and only supported, usage pattern is
    a local stack variable created and consumed in the same scope as the
    `GpioPin` — as in the example above. Because both live
    on the same stack frame, normal C++ destruction order (including on
    exception unwinding) tears down the future before the `GpioPin` it
    points into, so correct code never has to reason about this explicitly.
    Storing a `GpioEdgeFuture` somewhere that could outlive its `GpioPin` —
    a member variable, a heap allocation, a container — is undefined
    behavior, exactly as it would be for any other non-owning reference into
    an object the caller controls the lifetime of.

    !!! tip "TODO: debug-only lifetime assertion"
        Not yet implemented. A cheap, release-mode-free safety net worth
        adding: an outstanding-future counter on `GpioPin`, incremented in
        `GpioEdgeFuture`'s constructor and decremented in its destructor,
        with `~GpioPin()` asserting the count is zero. Catches a violation
        of the above contract in debug/test builds without requiring shared
        ownership.

## Refcounted disarm

Each of RP2040's four independent GPIO IRQ trigger bits (`GPIO_IRQ_LEVEL_LOW`,
`GPIO_IRQ_LEVEL_HIGH`, `GPIO_IRQ_EDGE_FALL`, `GPIO_IRQ_EDGE_RISE`) is
refcounted per `GpioPin`, not just armed-and-left-on:

- `GpioEdgeFuture`'s constructor increments the refcount for every bit in the
  mask it needs, enabling any bit whose count transitions 0 → 1 via
  `gpio_set_irq_enabled()`.
- Its destructor decrements those same counts, disabling any bit that drops
  back to 0 — unconditionally, whether the future completed normally, was
  destroyed mid-wait, or (for `wait_for_level()`'s already-satisfied fast
  path, see below) was never armed at all, in which case there is nothing to
  release.

This matters because `wait_for_edge()`/`wait_for_level()` broadcast off one
shared `IsrEvent` per pin (see "Multiple waiters" above), so more than one
`GpioEdgeFuture` — potentially arming different bits — can be in flight on
the same `GpioPin` concurrently. Disarming unconditionally on any single
future's destruction, without a refcount, could switch off a bit a sibling
future still needs.

!!! note "NOTE: why unconditional disarm-on-idle, not \"leave it armed forever\""
    Leaving every armed bit enabled for the rest of the pin's lifetime once
    first armed, and disabling everything only in `~GpioPin()`, would not be
    a leak in the sense of outliving the `GpioPin` — the destructor
    unconditionally clears all four bits regardless of refcount, so nothing
    survives the pin itself — but it would mean a bit could stay latched
    enabled long after every waiter interested in it is gone, which is a
    foot-gun for whoever next touches that pin (a spurious wake on unrelated
    code, or a mask silently already enabled from a previous, unrelated
    wait). Refcounted disarm avoids that at negligible cost: read the
    RP2040 SDK's `gpio_set_irq_enabled()` (`hardware_gpio/gpio.c`) and each
    enable/disable call is exactly two plain register stores — one to
    acknowledge (`gpio_acknowledge_irq()`), one to the `INTE` register's
    hardware atomic set/clear alias address (`hw_set_bits()`/
    `hw_clear_bits()`) — no critical section, no IRQ masking, no loop. There
    is no meaningful cost asymmetry that would favor leaving bits armed over
    disarming them promptly.

`~GpioPin()` is unchanged: it still unconditionally disables all four bits
on teardown regardless of any outstanding refcounts, as a backstop — a
`GpioPin` never depends on every `GpioEdgeFuture` having correctly released
its bits first (though per the lifetime contract above, no `GpioEdgeFuture`
should still exist by the time `~GpioPin()` runs).

`wait_for_level()`'s already-satisfied fast path (the level already matches
at call time) constructs its `GpioEdgeFuture` in an "already ready" state:
no bit is incremented, `gpio_set_irq_enabled()` is never called, and
`poll()` resolves immediately without ever registering with the executor.
Its destructor has nothing to release.

Modeled loosely on MicroPython's `Pin.irq()`, which supports
`Pin.IRQ_LOW_LEVEL`/`Pin.IRQ_HIGH_LEVEL` triggers alongside
`Pin.IRQ_RISING`/`Pin.IRQ_FALLING` (OR-able together for "any edge"), and on
embedded-hal-async's `Wait` trait, which has the identical five-method shape
(`wait_for_high`/`wait_for_low`/`wait_for_rising_edge`/`wait_for_falling_edge`/
`wait_for_any_edge`) with the same asymmetric level-vs-edge contract —
`wait_for_edge()` / `edges()` give the same capability as a callback
registration but `co_await`-able / iterable in a `for`-loop; `wait_for_level()`
gives the same capability as MicroPython's level-triggered IRQ modes, as a
single primitive rather than a caller-assembled check-then-arm sequence.

!!! warning "WARNING: once suspended, resolve on the latched signal — never re-sample the level on resume"
    `wait_for_level()`'s *initial* check is, and should be, a single plain
    read of the current level with no ISR involved at all: if the pin is
    already at the target level, return immediately without ever arming an
    interrupt or suspending — that's the whole point of distinguishing it
    from `wait_for_edge()`. This warning is about what happens *after* that
    check fails and the returned `GpioEdgeFuture` actually needs to suspend
    (`wait_for_level()`'s "not yet satisfied" branch, or any
    `wait_for_edge()` call): from that point on, the future must resolve
    based on the ISR having recorded that the transition occurred, never by
    reading the pin's current level again when `poll()` is next called. The
    two can disagree — by the time a woken task runs, the pin may already
    have changed again (e.g. a fast
    pulse that both rose and fell before the executor got back around to
    resuming the waiter) — and the future must still resolve in that case,
    since the condition it promised to detect did occur. This is exactly
    what `IsrEvent` already gets right by never re-deriving truth from a
    live read once a wait is registered (see `isr_event.h`'s `wait()`
    comments), and `GpioPin` must preserve that property for the suspended
    path. It would be an easy, plausible-looking regression to add a
    defensive `gpio_get()` re-check before returning from a *suspended*
    wait "just to be sure" — don't; that reintroduces the exact race this
    primitive exists to remove. (This is unrelated to, and does not argue
    against, the synchronous pre-check `wait_for_level()` performs before
    ever suspending — that check is a deliberate optimization, not a race.)

!!! note "NOTE: shared internal helper for wait_for_level()'s fallback and wait_for_edge()"
    Once `wait_for_level()`'s initial plain-read check fails, arming a wait
    for the pin to *reach* `level` is the same event as arming
    `wait_for_edge()` for the corresponding polarity (transitioning into
    `level`) — there's no behavioral difference once execution reaches that
    point, so both share one internal helper that builds the armed
    `GpioEdgeFuture` (mask refcount increment + `IsrWaitFuture` construction)
    rather than duplicating it. Since the future's constructor runs
    synchronously as part of `wait_for_edge()`/`wait_for_level()` itself,
    arming has already happened unconditionally before either function
    returns.
