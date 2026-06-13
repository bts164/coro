#pragma once

// Platform-portable mutex / condvar / shared_mutex types.
//
// The mutex protection in this library serves two distinct purposes depending
// on the platform:
//
//   Multi-threaded platforms (non-CORO_PICO):
//     Wakers may be held and called from any thread — a spawn_blocking worker,
//     a libuv I/O callback, or any external thread that receives a waker.
//     std::mutex provides the necessary cross-thread serialisation.
//
//   CORO_PICO (RP2040 bare-metal):
//     Two sources of concurrency exist: ISR preemption on the same core, and
//     code running on core 1 (e.g. a sensor driver calling a waker or sending
//     on a channel). std::mutex is unavailable in bare-metal newlib, so Mutex
//     and SharedMutex use spin_lock_blocking() / spin_unlock() from the Pico
//     SDK, which both disable IRQs and acquire a hardware spin lock. A single
//     shared spin lock (number 16) is used for all instances — see the
//     k_coro_spin_lock_num comment for the reasoning.
//
// CORO_PICO design tradeoff: using a full critical section (IRQ disable + spin
// lock) for every lock/unlock is stronger than strictly necessary for
// intra-coroutine state that never leaves the executor thread. This is accepted
// because:
//
//   1. The overhead is small — CPSID I + spin lock acquire is a handful of
//      cycles at 125 MHz, and all critical sections protect only a few
//      instructions (a queue push, a flag write, a waker swap).
//
//   2. Correctness is uniform — users are expected to call channel senders
//      from ISR handlers and from core 1. Uniform full-critical-section
//      protection means the right mutex is always used without per-site
//      reasoning about which protection level is needed.
//
//   3. Only one hardware spin lock is consumed for the entire coro runtime,
//      regardless of how many channels, wakers, or tasks exist.
//
// Usage:
//   #include <coro/detail/mutex.h>
//   coro::detail::Mutex m;
//   std::lock_guard lock(m);   // lock_guard / unique_lock / shared_lock still work
//
// Usage:
//   #include <coro/detail/mutex.h>
//   coro::detail::Mutex m;
//   std::lock_guard lock(m);   // lock_guard / unique_lock / shared_lock still work

#ifdef CORO_PICO

#include <hardware/sync.h>

// <mutex> still provides std::lock_guard and std::unique_lock (templates that
// work with any Lockable) even when std::mutex itself is not available.
// <shared_mutex> likewise provides std::shared_lock.
#include <mutex>
#include <shared_mutex>

namespace coro::detail {

// Spin lock number reserved for coro. Uses spin lock 16 — the first in the
// user-available range (SDK reserves 0-15). Spin locks are in the released
// state after reset, so no explicit init call is required.
static constexpr uint k_coro_spin_lock_num = 16;

// A single spin lock shared by all Mutex and SharedMutex instances.
// This is correct because without preemptive scheduling each core can hold
// at most one coro mutex at a time: IRQs are disabled while the lock is held
// (preventing ISR preemption on the same core), and there are no OS threads.
// One spin lock therefore suffices to arbitrate between core 0, core 1, and
// any ISR on either core — and consumes only 1 of the 32 hardware spin locks
// regardless of how many channel or waker objects exist.
class Mutex {
public:
    void lock()     { m_save = spin_lock_blocking(spin_lock_instance(k_coro_spin_lock_num)); }
    void unlock()   { spin_unlock(spin_lock_instance(k_coro_spin_lock_num), m_save); }
    bool try_lock() { lock(); return true; }
private:
    uint32_t m_save = 0;
};

// Shared reads and exclusive writes both take the full critical section —
// on RP2040 there are no concurrent readers within a single executor thread,
// so the shared/exclusive distinction is moot.
class SharedMutex {
public:
    void lock()            { m_save = spin_lock_blocking(spin_lock_instance(k_coro_spin_lock_num)); }
    void unlock()          { spin_unlock(spin_lock_instance(k_coro_spin_lock_num), m_save); }
    bool try_lock()        { lock(); return true; }
    void lock_shared()     { lock(); }
    void unlock_shared()   { unlock(); }
    bool try_lock_shared() { lock(); return true; }
private:
    uint32_t m_save = 0;
};

struct CondVar {
    void notify_all() {}
    void notify_one() {}
    // wait() must never be called on the single-threaded CurrentThreadExecutor — there
    // is no second thread to satisfy the predicate. Spin-halt to make misuse
    // obvious rather than silently returning with the predicate still false.
    template<typename Lock, typename Pred>
    void wait(Lock&, Pred) { for (;;) {} }
};

} // namespace coro::detail

#else  // ---- multi-threaded targets ----------------------------------------

#include <condition_variable>
#include <mutex>
#include <shared_mutex>

namespace coro::detail {
    using Mutex       = std::mutex;
    using SharedMutex = std::shared_mutex;
    using CondVar     = std::condition_variable;
} // namespace coro::detail

#endif // CORO_PICO
