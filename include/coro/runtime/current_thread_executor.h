#pragma once

// Single-threaded executor that drives coroutine tasks on the calling thread and
// waits for outside events through a Parker.
//
// The scheduling loop is platform-agnostic; the platform-specific piece is the
// injected Parker: PollingParker(cyw43_arch_poll) on Pico W (never blocks), or
// IoDriverParker on desktop (blocks in epoll when idle). Timers use coro::Clock.
// See doc/design/executor_design.md, "CurrentThreadExecutor".

#include <coro/runtime/executor.h>
#include <coro/runtime/parker.h>
#include <coro/detail/mutex.h>
#include <coro/detail/task.h>
#include <coro/detail/task_state.h>
#include <coro/detail/waker.h>
#include <coro/detail/rc.h>
#include <coro/detail/isr_flag.h>
#include <coro/detail/timer_queue.h>
#include <coro/runtime/clock.h>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <unordered_set>
#include <vector>

namespace coro {

//! Forward declaration to avoid circular dependency with Runtime.
class Runtime;

/**
 * @brief Single-threaded executor that drives all coroutine tasks on the calling
 * thread.
 *
 * Internal implementation class — the public entry point is @ref Runtime
 * (`Runtime(1)` on desktop; the only executor on Pico). `wait_for_completion()`
 * loops:
 *
 * @code
 * loop:
 *   poll_ready_tasks()      // poll all runnable coroutines once
 *   check_expired_timers()  // fire sleep_for / timeout wakers
 *   parker.park(max_wait)   // wait for outside events (see below)
 * @endcode
 *
 * `max_wait` is zero while tasks are ready, otherwise the time until the next
 * timer in this executor's own queue, otherwise unlimited. On desktop the parker
 * is an @ref IoDriverParker, so an idle executor blocks in epoll and I/O events
 * are dispatched on this thread. A desktop Runtime puts its timers in the
 * driver's queue rather than this one, and `turn()` bounds its own wait by them;
 * this executor's queue serves Pico and a standalone executor. On Pico it is a @ref PollingParker around `cyw43_arch_poll()`, which
 * never blocks, so the loop busy-polls.
 *
 * `enqueue()` may be called from other threads (blocking pool, lws service threads) or
 * from an ISR on Pico. It pushes under `m_ready_mutex`, and unparks the
 * executor if it is parked. See doc/design/executor_design.md, "Parking and its races".
 */
class CurrentThreadExecutor : public Executor {
public:
    /// @param parker How the loop waits for outside events; must not be null.
    explicit CurrentThreadExecutor(std::unique_ptr<Parker> parker);

    // Overload for Runtime(std::in_place_type<CurrentThreadExecutor>, parker).
    // The Runtime passes itself as the first argument; we ignore it.
    CurrentThreadExecutor(Runtime* /*rt*/, std::unique_ptr<Parker> parker)
        : CurrentThreadExecutor(std::move(parker)) {}

#ifndef CORO_PICO
    /// Desktop default, used by `Runtime(1)` and
    /// `Runtime(std::in_place_type<CurrentThreadExecutor>)`: an IoDriverParker on
    /// `rt->io_driver()`.
    explicit CurrentThreadExecutor(Runtime* rt);
#endif

    ~CurrentThreadExecutor() override = default;

    CurrentThreadExecutor(const CurrentThreadExecutor&)            = delete;
    CurrentThreadExecutor& operator=(const CurrentThreadExecutor&) = delete;
    CurrentThreadExecutor(CurrentThreadExecutor&&)                 = delete;
    CurrentThreadExecutor& operator=(CurrentThreadExecutor&&)      = delete;

    /// Takes ownership of `task` and enqueues it for its first poll.
    void schedule(detail::Rc<detail::TaskBase> task) override;

    /// Re-enqueues a woken task. May be called from a context other than the
    /// executor thread — an IRQ handler on Pico (e.g. DMA completion ISR) or
    /// an external thread on multi-threaded platforms. m_ready_mutex serialises
    /// access appropriately for the current platform (see detail/mutex.h).
    /// Unparks the executor if it is parked and the caller is another thread.
    void enqueue(detail::Rc<detail::TaskBase> task) override;

    /// @brief Runs the event loop until `state.terminated`.
    ///
    /// Alternates between draining the coroutine ready queue, firing expired
    /// timers, and parking. Whether parking blocks depends on the Parker: on
    /// Pico (PollingParker) it never does, so the loop busy-polls.
    void wait_for_completion(detail::TaskStateBase& state) override;

    /// True only when built by `CurrentThreadExecutor(Runtime*)`, whose parker turns
    /// the runtime's driver. A caller-supplied parker may not.
    bool turns_io_driver() const noexcept override { return m_turns_io_driver; }

    /// @brief Drains the ready queue: polls each task once in FIFO order.
    /// @return `true` if at least one task was polled.
    bool poll_ready_tasks();

    /// @brief Returns `true` if no task is waiting in the ready queue.
    bool empty() const;

    /// @brief Adds a timer to this executor's own queue: `waker` fires once
    /// `deadline` has passed. Thread-safe; unparks the executor if it is parked
    /// for a later deadline. Used by `Runtime::add_timer()` on Pico.
    /// @return The id to pass to cancel_timer().
    detail::TimerId add_timer(Instant deadline, detail::Weak<detail::Waker> waker);

    /// @brief Cancels a timer added by add_timer(). Thread-safe. Does nothing if it
    /// has already fired or been cancelled.
    void cancel_timer(detail::TimerId id) noexcept;

    /// @brief Fires wakers for any timers whose deadline has passed.
    /// Called from wait_for_completion() on every loop iteration.
    void check_expired_timers();

#ifdef CORO_PICO
    /// @brief Registers an ISR-safe waiter to be peeked once per event loop iteration.
    ///
    /// `entry` must outlive the registration (removed via remove_isr_poll() from the
    /// waiter's own destructor/move, same as today). Keyed by `entry`'s own identity,
    /// not by any state it reads — so multiple waiters sharing the same underlying
    /// flag/count each get their own, independently removable registration. See
    /// doc/design/isr_safety.md, "Multiple waiters". Safe to call from the executor
    /// thread only (not from an ISR — registration happens inside the coroutine, not
    /// the ISR).
    void add_isr_poll(IsrPollEntry* entry, detail::Rc<detail::Waker> waker);
    void remove_isr_poll(IsrPollEntry* entry);

    /// @brief Scans registered ISR waiters; fires wakers for any whose is_ready()
    /// peek returns true. Never removes entries itself -- nothing here resolves a
    /// wait, so there's nothing to react to by removing one; removal stays with the
    /// owning waiter's destructor. Called from wait_for_completion() on every loop
    /// iteration, after parking.
    void check_isr_events();
#endif // CORO_PICO

private:
    /// Picks the wait (zero / until next timer / unlimited), marks the executor
    /// parked, and parks once.
    void park_once();

    std::unique_ptr<Parker> m_parker;
    bool                    m_turns_io_driver = false;  // set once in the constructor

    // Internally synchronized. Lock order: m_ready_mutex, then the queue's mutex
    // (park_once() calls begin_wait() under m_ready_mutex).
    //
    // Declared before m_ready and m_owned_tasks so that it is destroyed after them:
    // destroying an unfinished task destroys its futures, and a SleepFuture cancels
    // its timer in this queue from its destructor.
    detail::TimerQueue m_timers;

    // m_ready_mutex serialises m_ready access against ISR preemption (Pico) or
    // concurrent thread wakers (multi-threaded platforms). See detail/mutex.h.
    mutable detail::Mutex m_ready_mutex;
    std::queue<detail::Rc<detail::TaskBase>> m_ready;
    // True from park_once()'s empty-queue check until park() returns. Read by
    // enqueue() to decide whether to unpark. GUARDED BY m_ready_mutex.
    bool m_parked = false;

    // Category 1 (doc/task_ownership.md): persistent lifetime anchor for every live task.
    // Inserted in schedule(), erased after poll() returns true (task reached terminal state).
    detail::Mutex                                       m_owned_mutex;
    std::unordered_set<detail::Rc<detail::TaskBase>>     m_owned_tasks;

#ifdef CORO_PICO
    struct IsrPollRegistration {
        IsrPollEntry*               entry;   // non-owning; entry outlives the registration
        detail::Rc<detail::Waker>   waker;
    };
    // Written from coroutine context (executor thread) only; read from
    // check_isr_events() on the same thread. No synchronisation needed.
    std::vector<IsrPollRegistration> m_isr_polls;
#endif // CORO_PICO
};

} // namespace coro
