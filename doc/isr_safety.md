# ISR Safety in Coro

This document records what is and is not safe to call from an interrupt service
routine (ISR), the platform analysis that surfaced these concerns, and the
proposed path to a principled, portable ISR-safe API.

!!! warning "WARNING: Audit in progress"
    The library was designed and implemented without an explicit ISR safety
    audit. The analysis below is the beginning of that audit, not the end. Do
    not assume any coro API is ISR-safe unless this document explicitly says so
    and explains why.

---

## Background: what triggered this audit

While designing a DMA-driven WS2812B LED driver on the Raspberry Pi Pico W
(RP2040), the question arose of whether `waker->wake()` could be called from a
DMA completion ISR. That question led to a deeper examination of what C++
atomics and `shared_ptr` actually compile to on Cortex-M0+, which revealed that
the entire library had been written with implicit threading assumptions that do
not hold on all platforms or in all calling contexts.

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

The `ptr` argument is ignored. This is a global lock over all atomic operations
on the device.

`spin_lock_blocking` **disables IRQs before acquiring the spin-lock** and
`spin_unlock` restores them after. The relevant loop:

```c
uint32_t save = save_and_disable_interrupts();
while (!*lock) {
    restore_interrupts(save);   // briefly re-enable while spinning for other core
    tight_loop_contents();
    save = save_and_disable_interrupts();
}
```

### Consequences for ISR safety

**Single-core (core 1 idle):** The spin-loop body never executes. An atomic op
on core 0 always finds the lock free because IRQs are disabled for the duration
— no ISR can fire mid-operation. `std::atomic<T>` and `shared_ptr` ref-counting
are ISR-safe in this configuration.

**Dual-core (core 1 active):** Core 1 can hold the atomic spin-lock
independently of core 0's IRQ state. If an ISR fires on core 0 while core 1
holds the lock, the ISR enters the spin-loop body and **briefly re-enables IRQs
on core 0** while waiting. This means:

- No deadlock — core 1 will release without needing anything from core 0.
- ISR latency on core 0 becomes **unbounded** relative to core 1's atomic
  activity. A high-frequency atomic user on core 1 can starve core 0 ISRs.
- During the spin, higher-priority ISRs on core 0 can fire, potentially nesting
  additional atomic waits.

!!! danger "WARNING: portability"
    The analysis above is specific to RP2040's `pico_atomic`. A port to any
    other Cortex-M0+ board, a different SDK, or any platform with a different
    atomic implementation could silently change these properties. Code that
    calls `shared_ptr` operations or `std::atomic` from an ISR and works on
    RP2040 may deadlock or corrupt memory on another target.

### Heap allocation from ISR

`malloc` / `operator new` maintain global heap state. Standard C library
allocators are **not ISR-safe** on bare-metal targets. Any coro operation that
allocates — constructing a new `shared_ptr` control block, resizing a
`std::deque`, emplacing into an `std::vector` — must not be called from an ISR.

This rules out constructing most coro objects from an ISR context entirely.

---

## Current state: what the library does and does not guarantee

At the time of writing, the library makes **no explicit ISR-safety guarantees**
for any API. The following table reflects the best current understanding after
the audit so far. It is incomplete and should be extended as the audit
progresses.

| Operation | ISR-safe? | Notes |
|---|---|---|
| `waker->wake()` | **No — undefined behavior** | Calls `shared_from_this()` (atomic ref-count) and `Executor::enqueue()`. May appear to work on single-core RP2040 but is not a supported use — portability to dual-core or any other platform is not guaranteed. |
| `shared_ptr` copy / destroy | **Platform-dependent** | Safe on single-core RP2040; unbounded latency risk on dual-core; unknown elsewhere. Do not rely on it in ISR paths. |
| `new` / `malloc` | **No** | Heap allocator is not ISR-safe on bare-metal. |
| `detail::Mutex` lock/unlock | **Unknown** | Uses `spin_lock_blocking` + IRQ-disable on Pico — likely safe, but not audited on all targets. |
| `mpsc::Sender::send()` | **No — undefined behavior** | Calls `wake()`. Not ISR-safe. Use `IsrChannel<T>` instead. |
| `oneshot::Sender::send()` | **No — undefined behavior** | Calls `wake()`. Not ISR-safe. Use `IsrEvent` instead. |
| `watch::Sender::send()` | **No — undefined behavior** | Calls `wake()`. Not ISR-safe. Use `IsrChannel<T>` instead. |
| `event::set()` | **No — undefined behavior** | Calls `wake()`. Not ISR-safe. Use `IsrEvent` instead. |
| `CancellationToken::cancel()` | **No — undefined behavior** | Calls `wake()` on registered wakers. Not ISR-safe. |
| Any `co_await` | **No** | Coroutines cannot suspend from ISR context on any platform. |
| Any I/O (TcpStream, File, …) | **No** | Calls into lwIP or libuv, which are not ISR-safe. |
| `volatile uint32_t` read/write | **Yes** | Single aligned word access is atomically safe on all Cortex-M. |
| `IsrEvent::signal_from_isr()` | **Yes** | Single `volatile bool` write. Designed specifically for ISR use. |
| `IsrChannel<T>::send_from_isr()` | **Yes** (T trivially copyable) | Volatile value copy + flag write. See multi-core note in design section. |

---

## The core problem with `waker->wake()`

`TaskBase::wake()` does three things:

1. CAS on `scheduling_state` — an atomic operation.
2. `shared_from_this()` — increments a `shared_ptr` ref-count atomically.
3. `owning_executor->enqueue(shared_ptr<TaskBase>)` — pushes to the ready queue
   under `detail::Mutex`.

Steps 1 and 2 depend on the platform's `std::atomic` implementation being
ISR-safe. Step 3 depends on `detail::Mutex` being ISR-safe. As shown above,
these properties hold on single-core RP2040 but are not universally guaranteed.

More importantly: even if every step is safe today, a user calling `wake()` from
an ISR cannot see any of this from the call site. The function signature gives no
indication that it involves ref-counting or platform-dependent atomic behaviour.
It is an easy mistake to port to a platform where it silently fails.

---

## Proposed solution: dedicated ISR-safe primitives

### Design philosophy

Rather than making existing sync primitives ISR-safe (which would require
auditing and potentially duplicating every API surface), the library provides a
small set of types designed from the ground up for ISR use. Everything else is
explicitly **not** ISR-safe. Users who need ISR-to-coroutine communication must
use these types — attempting to use any other coro API from an ISR is
unsupported.

This is the same philosophy as POSIX async-signal-safe functions and C++'s
`std::atomic`: a small, audited set of designated primitives rather than a
guarantee spread across the entire API.

### Inspiration: MicroPython's `ThreadSafeFlag`

MicroPython's asyncio faces the same problem on the same hardware and solves it
with a single primitive, `ThreadSafeFlag`. Its design reveals the key insight:

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

The ISR **never touches the scheduler**. It writes one flag and returns. The
event loop discovers the state change through its normal polling pass on the next
iteration. The coroutine wakes with at most one iteration of latency.

This completely sidesteps every concern in this document: no `shared_ptr`, no
atomics, no wakers, no risk of interaction with the scheduler's internal state.

### `IsrEvent` — signal with no value

For the common case of signalling that something happened (DMA complete, timer
fired, GPIO edge):

```cpp
// coro/sync/isr_event.h
class IsrEvent {
public:
    // ISR-safe: single volatile write, nothing else.
    // Safe to call from hard or soft IRQ on any platform.
    void signal_from_isr() noexcept { m_flag = true; }

    // Coroutine side: yields to the executor until signal_from_isr() is called.
    // Clears the flag before returning so the event can be reused.
    [[nodiscard]] coro::Coro<void> wait() {
        while (!m_flag)
            co_await coro::sleep_for(std::chrono::microseconds(0));
        m_flag = false;
    }

private:
    volatile bool m_flag = false;
};
```

`sleep_for(0)` yields once to the executor without any real delay — the
deadline is already past on the very next poll. This keeps the executor free
to handle TCP events, timers, and other tasks while the coroutine is waiting,
while adding at most one event loop iteration of latency between the ISR firing
and the coroutine resuming.

### `IsrChannel<T>` — signal with a value

For cases where the ISR produces a value (ADC reading, encoder count, received
byte):

```cpp
// coro/sync/isr_event.h
template<typename T>
    requires std::is_trivially_copyable_v<T>
class IsrChannel {
public:
    // ISR-safe: copies value, then sets flag.
    // The volatile qualifier on both members prevents the compiler from
    // reordering the value write after the flag write.
    void send_from_isr(T value) noexcept {
        m_value = value;
        m_flag  = true;
    }

    // Coroutine side: waits for a value and returns it.
    [[nodiscard]] coro::Coro<T> receive() {
        while (!m_flag)
            co_await coro::sleep_for(std::chrono::microseconds(0));
        T value = m_value;
        m_flag  = false;
        return value;
    }

private:
    volatile bool m_flag  = false;
    volatile T    m_value = {};
};
```

The `requires trivially_copyable` constraint is load-bearing: it guarantees
the value copy in `send_from_isr` is a plain `memcpy`-equivalent with no
allocation, no constructor call, and no exception.

!!! note "NOTE: multi-core value ordering"
    On dual-core RP2040, a data memory barrier (`__dmb()`) should be inserted
    between the value write and the flag write in `send_from_isr` to prevent
    the store buffer on core 1 from making the flag visible to core 0 before
    the value. On single-core (core 1 idle), the `volatile` ordering suffices.
    This will be addressed when the multi-core usage pattern is formally
    specified.

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
    co_await g_dma_done.wait();     // yields until ISR signals
    // DMA complete; buf is ready
}
```

### What this approach does NOT support

- **Multiple waiters on one `IsrEvent`.** Only one coroutine should call
  `wait()` at a time. Multiple waiters is a design smell for ISR communication
  anyway — if multiple coroutines need to react to a single ISR, funnel through
  a single waiting coroutine that then notifies the others via a normal `Event`.
- **Queuing multiple signals.** If `signal_from_isr()` is called again before
  `wait()` returns, the second signal is lost. For a stream of ISR-produced
  values, use `IsrChannel<T>` with a ring-buffer value type, or process the ISR
  value inside the waiting coroutine before the next transfer begins.
- **Any existing coro sync primitive from ISR.** `event::set()`, `oneshot::send()`,
  `mpsc::send()`, `watch::send()`, `wake()`, and `CancellationToken::cancel()` are all
  **undefined behavior** from ISR context. They may appear to work on single-core RP2040
  but are explicitly not supported. No exception.

---

## Audit checklist

- [ ] Implement `IsrEvent` and `IsrChannel<T>` and add them to `include/coro/sync/`.
- [ ] Add multi-core DMB note to `IsrChannel<T>::send_from_isr()` once the
  multi-core usage pattern is defined.
- [x] Add `// NOT ISR-SAFE` comments to `wake()` (`detail/task.h`), `event::set()`,
  `oneshot::send()` (both overloads), `mpsc::send()`, `mpsc::try_send()`,
  `mpsc::blocking_send()`, and `watch::send()`.
- [x] Document ISR safety policy in `pico_port.md` — replaced `oneshot::send()` ISR
  pattern with `IsrEvent`; added danger admonition explicitly calling all other
  coro APIs undefined behavior from ISR.
- [ ] Audit `detail::Mutex` on every supported target — confirm IRQ-disable is
  always part of the critical section (not just Pico).
- [ ] Document the multi-core RP2040 `pico_atomic` latency concern in `pico_port.md`.
- [ ] Add a `coro/sync/isr_event.h` entry to `doc/module_structure.md`.

---

## What is safe from an ISR right now

Until `IsrEvent` and `IsrChannel<T>` are implemented, the only safe interim
pattern is a raw `volatile` flag that the coroutine polls:

```cpp
volatile bool g_dma_done = false;

void dma_irq_handler() {
    dma_irqn_acknowledge_channel(0, MY_CHANNEL);
    g_dma_done = true;
}

coro::Coro<void> wait_for_dma() {
    using namespace std::chrono_literals;
    while (!g_dma_done)
        co_await coro::sleep_for(100us);
    g_dma_done = false;
}
```

`IsrEvent::wait()` is exactly this pattern wrapped in a reusable type — the
implementation above IS the final implementation, not a temporary workaround.
