# ISR Safety in Coro

The policy is a whitelist: **nothing is ISR-safe unless explicitly listed in this
document**. Everything else is undefined behavior from ISR context, regardless of
whether it appears to work on a specific hardware configuration.

At present the only ISR-safe API is `IsrEvent::signal_from_isr()` and
`IsrChannel<T>::send_from_isr()`.

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
- The executor checks registered ISR flags once per event loop iteration. When it
  observes a flag set, it fires the waker from executor context — where all the
  normal scheduling machinery is safe to use.

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

### `IsrEvent` — signal with no value

```cpp
// coro/sync/isr_event.h
class IsrEvent {
public:
    IsrEvent() : m_lock(spin_lock_instance(next_striped_spin_lock_num())) {}

    // ISR-safe. Takes the dedicated hardware spin lock (disables IRQs on this
    // core, spins on the SIO register for cross-core exclusion — see
    // "Cross-core ISR delivery" above), sets the flag, releases.
    void signal_from_isr() noexcept {
        uint32_t save = spin_lock_blocking(m_lock);
        m_flag = true;
        spin_unlock(m_lock, save);
    }

    [[nodiscard]] coro::Coro<void> wait() {
        co_await IsrWaitFuture{IsrFlagRef{&m_flag, m_lock}};
        uint32_t save = spin_lock_blocking(m_lock);
        m_flag = false;  // consume the signal
        spin_unlock(m_lock, save);
    }

private:
    spin_lock_t*  m_lock;
    volatile bool m_flag = false;
};
```

On first `co_await`, `IsrWaitFuture` registers `(IsrFlagRef, waker)` with the
executor and suspends. The executor checks all registered flags once per event
loop iteration — each check takes the paired spin lock, exactly like the ISR
write does — and when it observes the flag set it fires the waker and the
coroutine resumes. The ISR never touches the scheduler — it takes its own
dedicated lock, writes one flag, and returns.

### `IsrChannel<T>` — signal with a value

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

    [[nodiscard]] coro::Coro<T> receive() {
        co_await IsrWaitFuture{IsrFlagRef{&m_flag, m_lock}};
        uint32_t save = spin_lock_blocking(m_lock);
        T value = m_value;
        m_flag  = false;
        spin_unlock(m_lock, save);
        co_return value;
    }

private:
    spin_lock_t*  m_lock;
    volatile bool m_flag = false;
    T             m_value{};
};
```

`m_flag` and `m_value` are the only memory shared between the ISR and the
coroutine, and every access to either — from the ISR, from `IsrWaitFuture::poll()`,
and from `receive()` — goes through the same dedicated spin lock instance. The
`requires trivially_copyable` constraint ensures the assignment in the ISR
involves no allocation, no constructor, and no exception (a spin-lock critical
section must stay as short as the SDK's own convention demands — see Platform
background above).

### Usage example

```cpp
// Shared between ISR and coroutine — must outlive both.
coro::IsrEvent g_dma_done;

void dma_irq_handler() {
    dma_irqn_acknowledge_channel(0, MY_DMA_CHANNEL);
    g_dma_done.signal_from_isr();   // one volatile write; done
}

coro::Coro<void> do_dma_transfer(uint8_t* buf, size_t len) {
    // configure and start DMA here...
    co_await g_dma_done.wait();     // parks until ISR signals
    // DMA complete; buf is ready
}
```

### Limitations

- **One waiter at a time.** Only one coroutine should call `wait()` / `receive()`
  at a time. If multiple coroutines need to react to a single ISR, funnel through
  a single waiting coroutine that then notifies the others via a normal `Event`.
- **No signal queuing.** If `signal_from_isr()` fires again before `wait()`
  returns, the second signal is lost. Design the protocol so at most one signal is
  in flight at a time.

!!! tip "TODO: IsrCounter and IsrRingBuffer"
    Two additional primitives would cover the signal-queuing gap:

    - **`IsrCounter`** — a `volatile uint32_t` incremented by the ISR; the
      coroutine receives the accumulated count on each `wait()`. Useful for
      tick sources or GPIO edge counters where the value per event is
      unimportant but the number of missed events is.
    - **`IsrRingBuffer<T, N>`** — a fixed-size lock-free ring buffer written
      by the ISR and drained by the coroutine. Useful for high-frequency
      producers (UART RX bytes, ADC samples) where each event carries a value
      and none should be dropped.

