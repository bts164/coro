#pragma once

// Single-threaded polling executor that drives coroutine tasks on the calling
// thread alongside a platform I/O poll function.
//
// The scheduling loop is platform-agnostic; the two platform-specific pieces
// are injected by the build:
//   - Timer source:    time_us_64() on Pico; could be steady_clock on Linux
//   - I/O poll:        cyw43_arch_poll() on Pico W; could be a libuv tick elsewhere
//
// Currently only the Pico SDK implementations are provided (CORO_PICO), which
// was the original motivation for this executor. Future ports supply their own
// timer and poll implementations without changing the scheduling logic.
//
// Dependencies: pico_cyw43_arch (provides cyw43_arch_poll()) must be linked.

#include <coro/runtime/executor.h>
#include <coro/detail/mutex.h>
#include <coro/detail/task.h>
#include <coro/detail/task_state.h>
#include <coro/detail/waker.h>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <unordered_set>
#include <vector>

namespace coro {

//! Forward declaration to avoid circular dependency with Runtime.
class Runtime;

/**
 * @brief Single-threaded polling executor that drives all coroutine tasks on
 * the calling thread.
 *
 * Internal implementation class — the public entry point is @ref Runtime.
 * `wait_for_completion()` runs a tight loop: drain the ready queue, fire
 * expired timers, then call the platform I/O poll function:
 *
 * @code
 * loop:
 *   poll_ready_tasks()      // poll all runnable coroutines once
 *   check_expired_timers()  // fire sleep_for / timeout wakers
 *   platform_poll()         // drive I/O events (cyw43_arch_poll() on Pico W)
 * @endcode
 *
 * This model is well suited to bare-metal and embedded targets where a
 * dedicated I/O thread is unavailable or undesirable. The Raspberry Pi Pico W
 * was the first supported platform; future ports replace only the timer source
 * and poll function, leaving the scheduling state machine unchanged.
 *
 * Because everything runs on a single thread, `enqueue()` only needs to protect
 * `m_ready` against ISR preemption (not thread contention). See @ref detail::Mutex
 * for the IRQ-disabling critical section used there.
 */
class CurrentThreadExecutor : public Executor {
public:
    /// Microsecond clock — returns time since an arbitrary epoch (e.g. boot).
    /// On Pico W: time_us_64(). In tests: steady_clock offset.
    using ClockFn = std::function<uint64_t()>;

    /// Platform I/O poll function called once per event loop iteration.
    /// On Pico W: cyw43_arch_poll(). In tests: no-op.
    using PollFn  = std::function<void()>;

    explicit CurrentThreadExecutor(ClockFn clock, PollFn poll)
        : m_clock(std::move(clock)), m_poll(std::move(poll)) {}

    // Overload for Runtime(std::in_place_type<CurrentThreadExecutor>, clock, poll).
    // The Runtime passes itself as the first argument; we ignore it.
    explicit CurrentThreadExecutor(Runtime* /*rt*/, ClockFn clock, PollFn poll)
        : CurrentThreadExecutor(std::move(clock), std::move(poll)) {}

    ~CurrentThreadExecutor() override = default;

    CurrentThreadExecutor(const CurrentThreadExecutor&)            = delete;
    CurrentThreadExecutor& operator=(const CurrentThreadExecutor&) = delete;
    CurrentThreadExecutor(CurrentThreadExecutor&&)                 = delete;
    CurrentThreadExecutor& operator=(CurrentThreadExecutor&&)      = delete;

    /// Returns the current time in microseconds from the injected clock.
    uint64_t now_us() const { return m_clock(); }

    /// Takes ownership of `task` and enqueues it for its first poll.
    void schedule(std::shared_ptr<detail::TaskBase> task) override;

    /// Re-enqueues a woken task. May be called from a context other than the
    /// executor thread — an IRQ handler on Pico (e.g. DMA completion ISR) or
    /// an external thread on multi-threaded platforms. m_ready_mutex serialises
    /// access appropriately for the current platform (see detail/mutex.h).
    void enqueue(std::shared_ptr<detail::TaskBase> task) override;

    /// @brief Runs the event loop until `state.terminated`.
    ///
    /// Alternates between draining the coroutine ready queue, firing expired
    /// timers, and calling `cyw43_arch_poll()` to process WiFi and lwIP events.
    /// Never blocks — idles by polling rather than sleeping. For battery-sensitive
    /// applications, add a short `sleep_us(100)` when both queues are empty.
    void wait_for_completion(detail::TaskStateBase& state) override;

    /// @brief Drains the ready queue: polls each task once in FIFO order.
    /// @return `true` if at least one task was polled.
    bool poll_ready_tasks();

    /// @brief Registers a one-shot timer that wakes `waker` at `deadline_us`
    /// (microseconds since boot, as returned by time_us_64()).
    void schedule_timer(uint64_t deadline_us, std::shared_ptr<detail::Waker> waker);

    /// @brief Fires wakers for any timers whose deadline has passed.
    /// Called from wait_for_completion() on every loop iteration.
    void check_expired_timers();

private:
    ClockFn m_clock;
    PollFn  m_poll;

    // m_ready_mutex serialises m_ready access against ISR preemption (Pico) or
    // concurrent thread wakers (multi-threaded platforms). See detail/mutex.h.
    detail::Mutex m_ready_mutex;
    std::queue<std::shared_ptr<detail::TaskBase>> m_ready;

    struct TimerEntry {
        uint64_t deadline_us;
        std::shared_ptr<detail::Waker> waker;
    };
    struct TimerCmp {
        // min-heap: smallest deadline (next to expire) at the top
        bool operator()(const TimerEntry& a, const TimerEntry& b) const {
            return a.deadline_us > b.deadline_us;
        }
    };
    std::priority_queue<TimerEntry, std::vector<TimerEntry>, TimerCmp> m_timers;

    // Category 1 (doc/task_ownership.md): persistent lifetime anchor for every live task.
    // Inserted in schedule(), erased after poll() returns true (task reached terminal state).
    detail::Mutex                                                               m_owned_mutex;
    std::unordered_set<std::shared_ptr<detail::TaskBase>> m_owned_tasks;
};

} // namespace coro
