#pragma once

#include <coro/detail/poll_result.h>
#include <coro/detail/context.h>
#include <coro/runtime/timer_service.h>
#include <chrono>

namespace coro {

/**
 * @brief Future that completes once a wall-clock deadline has passed.
 *
 * Satisfies @ref Future<void>. Returns `PollReady` on the first `poll()` call after
 * `std::chrono::steady_clock::now() >= m_deadline`.
 *
 * @note Without libuv timer integration the executor is **not** woken at the precise deadline.
 *       The sleep fires on the next poll after the deadline, which may be later than requested.
 *       This limitation will be removed when libuv timer support is added.
 *
 * Prefer the @ref sleep_for factory function over constructing this directly.
 */
struct SleepFuture {
    using OutputType = void;

    std::chrono::steady_clock::time_point m_deadline;

    explicit SleepFuture(std::chrono::nanoseconds duration)
        : m_deadline(std::chrono::steady_clock::now() + duration) {}

    PollResult<void> poll(detail::Context& ctx) {
        if (std::chrono::steady_clock::now() >= m_deadline)
            return PollReady;
        // Register a wakeup with the runtime's TimerService. The timer thread will call
        // waker->wake() at m_deadline, moving this task from Suspended back to Ready.
        // If poll() is called again before the deadline fires (e.g. woken by another
        // branch of a select), we re-register — the extra wake is a no-op in the executor.
        // TODO: replace with a libuv timer registration when I/O integration is added.
        schedule_wakeup(m_deadline, ctx.getWaker());
        return PollPending;
    }
};

/**
 * @brief Returns a @ref SleepFuture that completes after `duration` has elapsed.
 *
 * @param duration How long to sleep. Resolution is limited to the executor's poll frequency
 *                 until libuv timer integration is available.
 */
[[nodiscard]] inline SleepFuture sleep_for(std::chrono::nanoseconds duration) {
    return SleepFuture(duration);
}

} // namespace coro
