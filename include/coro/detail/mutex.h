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
//   CORO_PICO (single-threaded, bare-metal):
//     There are no OS threads, so cross-thread races cannot occur. The only
//     source of concurrency is ISR preemption — e.g. OneshotSender::send()
//     called from a DMA completion IRQ handler. std::mutex is unavailable in
//     bare-metal newlib, so Mutex and SharedMutex are implemented as
//     IRQ-disabling critical sections via save_and_disable_interrupts() /
//     restore_interrupts().
//
// CORO_PICO design tradeoff: using IRQ-disabling critical sections for every
// mutex instance is stronger than strictly necessary for intra-coroutine state
// that never leaves the executor thread. This is accepted because:
//
//   1. The overhead is negligible — CPSID I / CPSIE I is 2 CPU cycles (~16 ns
//      at 125 MHz), and all critical sections protect only a handful of
//      instructions (a queue push, a flag write, a CAS).
//
//   2. Correctness is uniform — users are encouraged to call channel senders
//      from IRQ handlers to bridge hardware events into coroutines. Uniform
//      ISR safety means the right mutex is always used without per-site thought.
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

// IRQ-disabling critical section. See file-level comment for tradeoff rationale.
class Mutex {
public:
    void lock()     { m_save = save_and_disable_interrupts(); }
    void unlock()   { restore_interrupts(m_save); }
    bool try_lock() { lock(); return true; }
private:
    uint32_t m_save = 0;
};

// Shared reads and exclusive writes both disable interrupts — on a single-core
// executor there are no concurrent readers, so the distinction is moot.
class SharedMutex {
public:
    void lock()            { m_save = save_and_disable_interrupts(); }
    void unlock()          { restore_interrupts(m_save); }
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
