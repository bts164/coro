# ISR Safety in Coro

The policy is a whitelist: **nothing is ISR-safe unless explicitly listed in this
document**. Everything else is undefined behavior from ISR context, regardless of
whether it appears to work on a specific hardware configuration.

At present the only ISR-safe API is `IsrEvent::signal_from_isr()`,
`IsrChannel<T>::send_from_isr()`, and `IsrSemaphore::release_from_isr()`.

---

## Background

While designing a DMA-driven WS2812B LED driver on RP2040, the question arose of
whether `waker->wake()` could be called from a DMA completion ISR. That question
led to a deeper examination of what C++ atomics and `shared_ptr` compile to on
Cortex-M0+, and ultimately to the whitelist policy above.

---

## Platform background: Cortex-M0+ (RP2040 / RP2350)

### No hardware exclusive-access instructions

Cortex-M0+ has no `LDREX`/`STREX` instructions. All `std::atomic<T>` operations
are therefore implemented in software by the Pico SDK's `pico_atomic` library.

### How `pico_atomic` implements atomics

Every atomic operation — regardless of variable address — routes through a single
shared spin-lock (`PICO_SPINLOCK_ID_ATOMIC`):

```c
// pico-sdk/src/rp2_common/pico_atomic/atomic.c
static inline uint32_t atomic_lock(__unused const volatile void *ptr) {
    return spin_lock_blocking(spin_lock_instance(PICO_SPINLOCK_ID_ATOMIC));
}
```

`spin_lock_blocking` **disables IRQs before acquiring the spin-lock** and
`spin_unlock` restores them after:

```c
uint32_t save = save_and_disable_interrupts();
while (!*lock) {
    restore_interrupts(save);   // briefly re-enable while spinning for other core
    tight_loop_contents();
    save = save_and_disable_interrupts();
}
```

If an ISR fires on core 0 while core 1 holds the lock, the ISR enters the
spin-loop and **briefly re-enables IRQs on core 0** while waiting — ISR latency
becomes unbounded relative to core 1's atomic activity. This is why nearly
every coro operation (which touches `shared_ptr` ref-counts, atomic scheduling
state, or `detail::Mutex`) is unsafe from an ISR.

!!! danger "WARNING: portability"
    This analysis is specific to RP2040's `pico_atomic`. A port to any other
    platform with a different atomic implementation could silently change these
    properties.

### Heap allocation from ISR

`malloc` / `operator new` are not ISR-safe on bare-metal targets. Any operation
that allocates — constructing a `shared_ptr`, resizing a container — must not be
called from an ISR.

---

## Cross-core ISR delivery

The reasoning above assumes the only concurrency between an ISR and the executor is
*preemption on the same core* — the ISR interrupts the executor thread, runs to
completion, and returns. A plain `volatile bool` flag is sufficient for that case: the
ISR's write and the executor's later read can never physically overlap, so there's no
tear, and ordering between the flag and any payload is enforced with a `dmb` (see
`IsrChannel<T>` below, prior revisions).

That assumption breaks on RP2040's second core. Nothing prevents an ISR from firing on
core 1 while the `CurrentThreadExecutor` is running on core 0 — that is genuine
concurrent execution, not preemption, and a `volatile bool` gives no atomicity or
ordering guarantee for genuinely concurrent access under the C++ memory model. Today
this is low-risk in practice: the executor busy-polls, so the worst case is the
executor's check lands one poll-loop iteration before the flag write completes — never
observed, caught next iteration. **That stops being true once `wfi`-based idle parking
lands** (planned, see `pico_port.md`): if the executor parks with `wfi` between poll
iterations, a flag write that lands in the parking window has no guaranteed mechanism
to wake the core, and the race becomes a real, user-visible missed signal rather than a
one-iteration delay.

!!! danger "WARNING: do not fix this with std::atomic"
    The obvious-looking fix — making the flag `std::atomic<bool>` — was considered and
    rejected. Two reasons:

    1. Cortex-M0+ has no `LDREX`/`STREX` (see Platform background above), so even a
       plain `std::atomic<bool>::load()`/`store()` is not guaranteed to compile to a
       bare `ldrb`/`strb`. Whether the toolchain inlines it that way or routes it
       through `pico_atomic`'s software spin-lock fallback depends on the ABI and
       toolchain version — it is not something `isr_event.h`'s source can audit the
       way it can audit a literal `__asm__` block.
    2. If it *did* fall through to `pico_atomic`'s shared spin-lock (`PICO_SPINLOCK_ID_ATOMIC`),
       that lock is taken by *all* atomic operations system-wide. An ISR spinning on a
       lock that ordinary code elsewhere holds is a genuine deadlock — the lock owner
       can never make progress if the ISR fires on its core and spins forever waiting
       for the IRQ-disabled owner to release it. (`pico_atomic`'s own implementation
       briefly re-enables IRQs while spinning specifically to avoid this on the *local*
       core, but that doesn't help if the lock owner is the same core's ISR busy-spinning.)

    The fix below uses the same underlying hardware primitive `pico_atomic` uses
    (an SIO spin-lock register) but applies it explicitly and only to the one flag (and
    payload) that needs it — fully auditable as documented hardware-register
    read/write semantics, never routed through a generic atomics codegen path.

### The fix: a dedicated hardware spin lock per flag

`pico-sdk`'s own `queue_t` (`pico/util/queue.h`) solves exactly this problem — a value
shared between an ISR and arbitrary other code, safe across both cores — using two
primitives from `hardware/sync.h`, not `std::atomic`:

```c
// pico-sdk's pattern (queue_add_internal, abbreviated):
uint32_t save = spin_lock_blocking(lock);   // disables IRQs on this core, then
                                             // spins on a real SIO hardware register
                                             // (genuine mutual exclusion vs. the other core)
... touch shared state ...
spin_unlock(lock, save);                    // releases the register, restores IRQs
```

`spin_lock_blocking()` does two things, both fully documented hardware/architecture
behavior rather than toolchain-dependent codegen:

- `save_and_disable_interrupts()` — `cpsid i`. Masks IRQs on *this* core only, so the
  local ISR cannot preempt the critical section (same guarantee `volatile` relied on
  before, now explicit).
- Spins on one of RP2040's 32 SIO spin-lock registers. A *read* of the register
  atomically claims the lock and returns nonzero if it was free; a *write of zero*
  releases it. This is real silicon, not LDREX/STREX and not a software CAS loop — the
  other core spinning on the same register physically cannot proceed until this core
  writes zero. This is the cross-core exclusion `volatile` never had.

`IsrEvent` and `IsrChannel<T>` each own one dedicated spin-lock instance (drawn from the
SDK's "striped" pool via `next_striped_spin_lock_num()`, the same allocation strategy
`queue_init()` uses) and take it around every access to their flag/payload — on both the
ISR side and the executor side. See the updated primitives below.

On the host test build there is no real SIO register or IRQ to disable;
`test/pico/stub/hardware/sync.h` backs `spin_lock_blocking()`/`spin_unlock()` with a
`std::mutex` instead, so that `test_isr_event.cpp`'s `std::thread`-simulated ISR — which
*is* genuinely concurrent on x86, unlike a real single-core interrupt — gets the same
mutual-exclusion guarantee under TSan that the SIO register gives real hardware. A
`std::mutex` would never be safe to take from a *real* ISR (it can block); the host stub
is only valid because the host's simulated "ISR" is an ordinary thread that's allowed to
block, not a real interrupt handler.

---

## Design philosophy

The root cause of ISR unsafety in coro is that nearly every operation eventually
calls `wake()`, which touches `shared_ptr` ref-counts, atomic scheduling state,
and `detail::Mutex` — all of which involve the `pico_atomic` spin-lock described
above.

The solution is to keep the ISR path minimal and push all scheduling work onto the
executor loop:

- The ISR writes a flag (and for `IsrChannel<T>`, a value) and returns immediately.
  It never touches the scheduler, wakers, or any ref-counted object.
- The executor checks each registered waiter's readiness once per event loop
  iteration — a non-mutating peek, see `IsrPollEntry::is_ready()` under "Multiple
  waiters" below. When a peek comes back true, it fires that waiter's waker from
  executor context — where all the normal scheduling machinery is safe to use.
  The peek never consumes anything itself; see "Multiple waiters" for why that
  has to be true and where the actual consuming logic lives instead.

This is the same pattern as MicroPython's `ThreadSafeFlag`, which solves the
identical problem on the same hardware:

```python
class ThreadSafeFlag(io.IOBase):
    def set(self):          # ISR path: one integer write, nothing else
        self.state = 1

    async def wait(self):   # coroutine path: polls via event loop
        if not self.state:
            yield _io_queue.queue_read(self)
        self.state = 0

    def ioctl(self, req, flags):
        if req == MP_STREAM_POLL:
            return self.state * flags   # event loop discovers state here
```

The ISR writes one integer and returns. The event loop discovers the change
through its normal polling pass. It completely sidesteps the spin-lock concern
by never entering that code from ISR context.

---

## ISR-safe primitives

### `IsrEvent` — reusable signal with no value, broadcast to every waiter

Despite the name suggesting a one-shot signal, `IsrEvent` is meant to be
`wait()`-ed on repeatedly across its lifetime — a real call site reuses the
same instance for every touch IRQ from a touch-controller driver, not once.
It's a *broadcast* primitive: every current waiter legitimately wants to know
the same thing happened, and every one of them resuming on the same signal is
correct.

The exact ordering guarantee: `signal_from_isr()` wakes every `wait()` that
was already registered at the moment it's called — i.e. every `wait()` whose
`co_await` was reached before this `signal_from_isr()`, regardless of which
of them the executor happens to resume first. A `wait()` registered *after*
`signal_from_isr()` has already run does not observe that signal; it parks
until the next one. A second `signal_from_isr()` before the first is observed
does not queue (see Limitations below).

Represented as a 64-bit generation counter (`m_epoch`), not a bool flag:

```cpp
// coro/sync/isr_event.h
class IsrEvent {
public:
    IsrEvent() : m_lock(spin_lock_instance(next_striped_spin_lock_num())) {}

    // ISR-safe. Takes the dedicated hardware spin lock (disables IRQs on this
    // core, spins on the SIO register for cross-core exclusion — see
    // "Cross-core ISR delivery" above), bumps the epoch, releases.
    void signal_from_isr() noexcept {
        uint32_t save = spin_lock_blocking(m_lock);
        ++m_epoch;
        spin_unlock(m_lock, save);
    }

    [[nodiscard]] coro::Coro<void> wait() {
        co_await IsrWaitFuture{*this};
    }

private:
    friend class IsrWaitFuture;

    spin_lock_t*  m_lock;
    uint64_t      m_epoch = 0;
};
```

`m_epoch` is deliberately not `volatile` — it's only ever read or written inside
`spin_lock_blocking()`/`spin_unlock()`'s critical section (an opaque function
call the compiler can't reorder across or cache through), so the lock already
provides the ordering/visibility `volatile` would otherwise be papering over.
Same reasoning applies to `IsrChannel<T>`'s `m_flag`/`m_value` and
`IsrSemaphore`'s `m_count` below.

!!! note "NOTE: why 64 bits, not 32"
    Comparisons against `m_epoch` only ever test "has it changed", never
    magnitude or ordering, so wraparound itself is harmless at any width —
    but a 32-bit counter can genuinely *wrap* within a single long-lived
    `IsrEvent`'s real uptime, which the reusable/broadcast design explicitly
    invites (see the class comment above). A 1us-period signal wraps
    `UINT32_MAX` in ~71 minutes; even a 100us-period signal wraps in a few
    days — both well inside a plausible instance lifetime. At the same
    1us rate, `UINT64_MAX` takes ~584,000 years, removing the concern
    entirely. The extra width costs nothing here: `m_epoch` is only ever
    touched inside the same spin-lock critical section as everything else in
    the class, never read or written outside it, so there's no atomicity
    property being traded away by widening it — just one extra register's
    worth of storage and a slightly wider load/store, both already inside
    the locked section.

`IsrWaitFuture` is `IsrEvent`'s waiter — the two sit at the same level (one
owns the ISR-shared state, the other is its executor-facing waiter), so
`IsrWaitFuture` reaches `m_epoch`/`m_lock` directly via friendship rather than
through an indirection struct. No separate "ref" type is needed to keep
`IsrEvent`'s details out of the executor either — the executor only ever
sees the `IsrPollEntry` interface (below), never `IsrEvent` itself.

`IsrWaitFuture`'s constructor captures the current epoch as `m_baseline`, and
both `is_ready()` and `poll()` are purely `m_epoch != m_baseline` — a
non-mutating comparison, never a claim. This is what makes broadcast to any
number of concurrent waiters correct with no coordination between them: each
one independently compares the same shared epoch against its own baseline,
so every waiter started before a given `signal_from_isr()` resolves on it,
and none of them can accidentally consume it on another's behalf.

!!! danger "WARNING: a bool flag cannot implement this correctly — a generation counter can"
    An earlier revision used a bool flag, cleared either automatically by
    `wait()` or explicitly by a since-removed `clear()`. Both designs are
    broken for a reusable, repeatedly-`wait()`-ed, potentially-concurrent
    primitive:

    - **Auto-clearing on `wait()`'s own resumption** breaks broadcast: if
      waiter A resumes first and clears the flag before waiter B has had its
      own turn to be polled in the same wake batch, B's poll observes the
      flag already false and incorrectly re-suspends on a signal that
      already happened and may never recur.
    - **Never auto-clearing (requiring an explicit `clear()`)** breaks reuse
      even for a single waiter: once `signal_from_isr()` sets the flag, every
      subsequent `wait()` returns immediately regardless of whether a new
      signal ever occurs, unless the caller remembers to call `clear()`
      between every pair of waits — silently defeats the entire "wait for
      the next occurrence" contract if it's ever forgotten, and a stale flag
      looks identical to a fresh one to whatever calls `wait()` next.

    A generation counter has neither failure mode: nothing needs to be
    reset, because "new" is defined relative to each waiter's own baseline,
    captured once at construction, not relative to a single shared bit that
    every waiter has to agree on the state of.

On first `co_await`, `IsrWaitFuture` registers itself (as an `IsrPollEntry`,
see "Multiple waiters" below) with the executor and suspends. The executor
peeks each registered entry's readiness once per event loop iteration — a
non-mutating check under the paired spin lock — and when a peek comes back
true it fires that waiter's real waker; the resumed coroutine's own poll then
re-reads the epoch and resolves. The ISR never touches the scheduler — it
takes its own dedicated lock, bumps one counter, and returns.

### `IsrChannel<T>` — reusable single-slot channel, one value claimed per send

Like `IsrEvent`, `IsrChannel<T>` is meant to be `receive()`-ed repeatedly, not
used once — "single-slot, non-queuing" is the accurate description, not
"one-shot" (see Limitations below for what "non-queuing" costs). Unlike
`IsrEvent`, a sent value belongs to exactly *one* receiver, not every waiter —
this is a **claim**, not a broadcast.

```cpp
// coro/sync/isr_event.h
template<typename T>
    requires std::is_trivially_copyable_v<T>
class IsrChannel {
public:
    IsrChannel() : m_lock(spin_lock_instance(next_striped_spin_lock_num())) {}

    // ISR-safe. m_value and m_flag are written inside the same critical
    // section, so the spin lock's own acquire/release ordering (the same
    // ordering pico-sdk's queue_t relies on) is what guarantees the receiver
    // never observes a partially-written value — no separate barrier needed.
    void send_from_isr(T value) noexcept {
        uint32_t save = spin_lock_blocking(m_lock);
        m_value = value;
        m_flag  = true;
        spin_unlock(m_lock, save);
    }

    // The claim (check flag, copy value, clear flag) all happens inside
    // IsrChannelWaitFuture<T>::poll() -- one locked critical section, called
    // at most once per receive() (see "Multiple waiters" below for why no
    // explicit retry loop is needed here even under concurrent receivers).
    [[nodiscard]] coro::Coro<T> receive() {
        co_return co_await IsrChannelWaitFuture<T>{*this};
    }

private:
    friend class IsrChannelWaitFuture<T>;

    spin_lock_t*  m_lock;
    bool          m_flag = false;
    T             m_value{};
};
```

`m_flag` and `m_value` are the only memory shared between the ISR and the
coroutine, and every access to either — from the ISR and from
`IsrChannelWaitFuture<T>::poll()` — goes through the same dedicated spin lock
instance. The `requires trivially_copyable` constraint ensures the assignment
in the ISR involves no allocation, no constructor, and no exception (a
spin-lock critical section must stay as short as the SDK's own convention
demands — see Platform background above).

`IsrChannelWaitFuture<T>::poll()` follows the same shape as `IsrWaitFuture::poll()`
(register on first `Pending`, no separate consuming step elsewhere), except
its `OutputType` is `T`: under the lock, if `m_flag` is set it copies
`*m_value_ptr` out, clears the flag, and returns `PollReady(value)` — claim
and value-copy happen atomically in the same critical section, so there is no
window for a second `send_from_isr()` to land between "claimed" and "read."

### `IsrSemaphore` — saturating count, no value

```cpp
// coro/sync/isr_event.h
class IsrSemaphore {
public:
    IsrSemaphore() : m_lock(spin_lock_instance(next_striped_spin_lock_num())) {}

    // ISR-safe. Increments the count, saturating at UINT32_MAX instead of
    // wrapping. Wrapping to 0 would make a consumer mid-drain see an empty
    // counter while edges are still outstanding — silently losing track of
    // the backlog rather than merely folding together some very old events.
    void release_from_isr() noexcept {
        uint32_t save = spin_lock_blocking(m_lock);
        if (m_count != UINT32_MAX) ++m_count;
        spin_unlock(m_lock, save);
    }

    // Suspends until the count is nonzero, then claims one unit (decrements
    // by one) and returns. Unlike IsrEvent::wait() / IsrChannel<T>::receive(),
    // a second release_from_isr() before acquire() returns is NOT lost — it
    // simply means a later acquire() call resolves immediately instead of
    // suspending. The claim-and-decrement happens inside
    // IsrSemaphoreWaitFuture::poll() -- see "Multiple waiters" below for why no
    // explicit retry loop is needed here even under concurrent acquire()s.
    [[nodiscard]] coro::Coro<void> acquire() {
        co_await IsrSemaphoreWaitFuture{*this};
    }

private:
    friend class IsrSemaphoreWaitFuture;

    spin_lock_t*  m_lock;
    uint32_t      m_count = 0;
};
```

Same dedicated-spin-lock discipline as `IsrEvent`/`IsrChannel<T>` — the ISR
takes the lock, does one bounded-time integer update, and releases.
`IsrSemaphoreWaitFuture::poll()` takes the same lock and, if `m_count != 0`,
decrements it and returns `PollReady`; otherwise it returns `PollPending`.
This is a **claim**, not a broadcast — the counter is not `IsrEvent`-shaped:
one `release_from_isr()` makes exactly one unit available, and it belongs to
whichever `acquire()` claims it first, not to every waiter at once.

This directly covers the "no signal queuing" gap noted below for the case
where only the *number* of missed events matters, not any per-event payload
— GPIO edge counting being the motivating case (see
`doc/design/gpio_pin.md`'s `GpioPin::edges()`), but the same shape applies
to any tick/pulse source (quadrature encoders, flow meters, tachometers)
where an ISR-side value would just be an ever-alternating enum recoverable
from parity plus one remembered piece of state, not something that needs to
be queued as data in its own right — see the ring buffer note below for the
case where a queued value genuinely can't be discarded.

### Usage example

```cpp
// Shared between ISR and coroutine — must outlive both.
coro::IsrEvent g_dma_done;

void dma_irq_handler() {
    dma_irqn_acknowledge_channel(0, MY_DMA_CHANNEL);
    g_dma_done.signal_from_isr();   // one locked increment; done
}

coro::Coro<void> do_dma_transfer(uint8_t* buf, size_t len) {
    // configure and start DMA here...
    co_await g_dma_done.wait();     // parks until ISR signals
    // DMA complete; buf is ready
}
```

### Multiple waiters

**The executor's poll table is a peek, not a claim.** Every ISR-safe
primitive's waiter (`IsrWaitFuture`, `IsrChannelWaitFuture<T>`,
`IsrSemaphoreWaitFuture`) implements a small non-mutating interface:

```cpp
// coro/detail/isr_flag.h
class IsrPollEntry {
public:
    virtual ~IsrPollEntry() = default;

    // Non-mutating. Executor-thread only, called once per event loop
    // iteration for every currently-registered entry. Must never consume,
    // decrement, or clear anything -- it only decides whether this entry's
    // real waker is worth firing this tick. Deliberately not named `poll`:
    // unlike Future::poll(Context&), which advances state and must not be
    // called again after Ready, is_ready() is designed to be called
    // repeatedly and changes nothing.
    virtual bool is_ready() const = 0;
};
```

Each waiter registers `this` (it already implements `is_ready()`, no
separate wrapper object needed) together with its real task waker via
`add_isr_poll(IsrPollEntry*, Rc<Waker>)`, and deregisters via
`remove_isr_poll(IsrPollEntry*)` from its own destructor — same as today,
except keyed by the waiter's own identity (a unique pointer per
registration) instead of the shared flag address. That alone fixes the
`remove_isr_poll` bug above: with multiple waiters sharing one flag, each
still has its own distinct `IsrPollEntry*`, so removing one can never
deregister another — no multimap keyed by flag needed, a plain
`std::vector<Entry>` with removal by entry-pointer identity is correct
as-is.

`check_isr_events()` becomes:

```cpp
for (auto& reg : m_isr_polls)
    if (reg.entry->is_ready())
        reg.waker->wake();
```

— no removal here either. `wake()` only reschedules the owning task for a
fresh top-down `poll()` pass (see `doc/poll_vs_continuation.md`); it does not
resolve anything itself.

**The actual claim happens exactly once, and it already has a home:**
`Coro<T>::poll()`'s existing spurious-wake guard
(`include/coro/coro.h`, `if (promise.m_poll_current) { ... promise.m_poll_current->poll() ... }`)
re-polls whatever future the coroutine is currently suspended at on *every*
poll of the root task, regardless of why it was scheduled — spurious wakes
included, by contract (`doc/future.h`: "`poll()` must not be called again
after it returns `PollReady`..." governs calls *after* Ready, not repeated
calls while `Pending`, which every leaf future must already tolerate). This
is not new machinery to add; it is the reason `IsrPollEntry::is_ready()` is
allowed to be a shallow peek in the first place. When `wake()` fires and the
root traversal reaches the suspended `co_await`, this guard calls the real
`IsrWaitFuture`/`IsrChannelWaitFuture<T>`/`IsrSemaphoreWaitFuture::poll()` —
which does the real, mutating, locked check-and-consume — exactly once, and
only through this single path. Two independent pollers can never race for
the same claim, because only one caller (the coroutine's own traversal) is
ever allowed to call the mutating `poll()`; the executor's `is_ready()` peek
never touches the shared state beyond reading it.

This is also why removal never needs to happen from inside
`check_isr_events()`: nothing there ever resolves a wait, so there's nothing
to react to by removing an entry mid-loop. Removal stays exactly where it
already is — the owning `IsrWaitFuture`/etc.'s destructor, called once its
`co_await` actually completes (or is cancelled).

What differs between primitives is only what `is_ready()` checks and what
the real `poll()` claims:

- **`IsrEvent`** — `is_ready()` and `poll()` both just compare `m_epoch !=
  m_baseline`; neither one has a side effect. Broadcast is the correct
  semantics here: it carries no payload, every waiter legitimately wants to
  know the same thing happened, and nothing is consumed, so every waiter
  resolving on the same signal is correct, not a race.
- **`IsrChannel<T>`** and **`IsrSemaphore`** carry exactly one unit (a value,
  or one count) that belongs to exactly *one* waiter. `is_ready()` still
  just peeks (`m_flag` / `m_count != 0`) so every waiter gets woken and gets
  a chance, but each one's real `poll()` re-checks and claims under the lock
  — decrementing `m_count`, or clearing `m_flag` and copying `m_value` —
  before returning `PollReady`. A waiter that loses the race (another
  `poll()` claimed first) simply observes the now-consumed state and returns
  `PollPending` again, naturally retried on the next real signal — no
  explicit retry loop needed in `acquire()`/`receive()` themselves, since the
  spurious-wake guard above already re-polls automatically.

### Limitations

- **`IsrEvent`/`IsrChannel<T>` do not queue signals.** If `signal_from_isr()`/
  `send_from_isr()` fires again before `wait()`/`receive()` returns, the
  second signal (and any value it carried) is lost. Design the protocol so
  at most one signal is in flight at a time, or use `IsrSemaphore` (above) if
  only the number of missed events — not a per-event value — needs to
  survive.

!!! tip "TODO: IsrRingBuffer"
    `IsrSemaphore` (above) covers queuing a bare count. Still missing:
    **`IsrRingBuffer<T, N>`** — a fixed-size lock-free ring buffer written by
    the ISR and drained by the coroutine. Useful for high-frequency producers
    (UART RX bytes, ADC samples) where each event carries a value that can't
    be reconstructed the way `GpioPin::edges()` reconstructs edge direction
    from parity, and none should be dropped.

