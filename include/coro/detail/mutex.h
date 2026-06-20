#pragma once

// Platform-portable mutex / condvar / shared_mutex types.
//
//   Multi-threaded platforms (non-CORO_PICO):
//     Wakers may be held and called from any thread — a spawn_blocking worker,
//     a libuv I/O callback, or any external thread that receives a waker.
//     std::mutex provides the necessary cross-thread serialisation.
//
//   CORO_PICO (RP2040 bare-metal):
//     CurrentThreadExecutor is cooperative and single-threaded. co_await is the
//     only yield point, so no two coroutines ever interleave. ISR concurrency is
//     handled explicitly by IsrEvent / IsrChannel via a dedicated hardware
//     spin lock per flag (see doc/design/isr_safety.md) — not by these
//     mutexes. All three types are therefore no-ops.
//
// Usage:
//   #include <coro/detail/mutex.h>
//   coro::detail::Mutex m;
//   std::lock_guard lock(m);   // lock_guard / unique_lock / shared_lock still work

#ifdef CORO_PICO

// <mutex> / <shared_mutex> provide std::lock_guard, std::unique_lock, and
// std::shared_lock as templates that work with any Lockable type.
#include <mutex>
#include <shared_mutex>

namespace coro::detail {

// CurrentThreadExecutor is cooperative and single-threaded: co_await is the
// only yield point, so no two coroutines ever interleave. The only true
// concurrency source on RP2040 is ISR preemption (same-core) and, on the
// second core, genuinely concurrent execution — both are handled explicitly
// by IsrEvent / IsrChannel via a dedicated hardware spin lock per flag (see
// doc/design/isr_safety.md, "Cross-core ISR delivery") — not by these
// mutexes.
//
// No-op mutexes mean:
//   • No hardware spin lock is consumed.
//   • IRQs are never disabled by coro internals, so USB-CDC, SPI, I2C and
//     other interrupt-driven I/O remain live during any lock-guarded section.
//   • Holding a "lock" while doing slow I/O (lcd.write, printf) is harmless.
//
// If core-1 task dispatch is added in the future, revisit this: any shared
// state accessed from both cores will need real synchronisation.
class Mutex {
public:
    void lock()     {}
    void unlock()   {}
    bool try_lock() { return true; }
};

class SharedMutex {
public:
    void lock()            {}
    void unlock()          {}
    bool try_lock()        { return true; }
    void lock_shared()     {}
    void unlock_shared()   {}
    bool try_lock_shared() { return true; }
};

struct CondVar {
    void notify_all() {}
    void notify_one() {}
    // wait() must never be called on the single-threaded CurrentThreadExecutor —
    // there is no second thread to satisfy the predicate.
    template<typename Lock, typename Pred>
    void wait(Lock&, Pred) { for (;;) {} }
};

} // namespace coro::detail

#else  // ---- multi-threaded targets, per-instance mutexes ------------------

#include <condition_variable>
#include <mutex>
#include <shared_mutex>

namespace coro::detail {
    using Mutex       = std::mutex;
    using SharedMutex = std::shared_mutex;
    using CondVar     = std::condition_variable;
} // namespace coro::detail

#endif // CORO_PICO
