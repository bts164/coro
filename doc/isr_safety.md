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
    // ISR-safe: single volatile write, nothing else.
    // A volatile bool is a single STRB instruction on ARM — naturally atomic.
    // No memory barrier needed: there is no payload to order.
    void signal_from_isr() noexcept { m_flag = true; }

    [[nodiscard]] coro::Coro<void> wait() {
        m_flag = false;  // discard any stale signal on entry
        co_await IsrWaitFuture{&m_flag};
        m_flag = false;  // consume the signal
    }

private:
    volatile bool m_flag = false;
};
```

On first `co_await`, `IsrWaitFuture` registers `(flag, waker)` with the executor
and suspends. The executor checks all registered flags once per event loop
iteration; when it observes the flag set it fires the waker and the coroutine
resumes. The ISR never touches the scheduler — it writes one flag and returns.

### `IsrChannel<T>` — signal with a value

```cpp
// coro/sync/isr_event.h
template<typename T>
    requires std::is_trivially_copyable_v<T>
class IsrChannel {
public:
    // ISR-safe.
    // __DMB() provides release semantics: the "memory" clobber is a compiler
    // barrier that prevents the compiler from reordering the value store past
    // the flag store, and the dmb instruction drains the write buffer so
    // m_value is globally visible before m_flag is written.
    void send_from_isr(T value) noexcept {
        m_value = value;
        __DMB();
        m_flag = true;
    }

    // __DMB() provides acquire semantics: the "memory" clobber prevents the
    // compiler from hoisting the m_value load above the point where m_flag
    // was observed true, pairing with the release __DMB() in send_from_isr().
    [[nodiscard]] coro::Coro<T> receive() {
        co_await IsrWaitFuture{&m_flag};
        __DMB();
        T value = m_value;
        m_flag  = false;
        co_return value;
    }

private:
    volatile bool m_flag = false;
    T             m_value{};
};
```

`m_flag` and `m_value` are the only memory shared between the ISR and the
coroutine. `m_flag` is a single volatile byte — a naturally atomic STRB/LDRB on
ARM. `m_value` is plain `T`; the `__DMB()` pair provides the release/acquire
ordering that guarantees the receiver never observes a partially-written value.
The `requires trivially_copyable` constraint ensures the assignment in the ISR
involves no allocation, no constructor, and no exception.

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

