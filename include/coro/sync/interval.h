#pragma once

#include <coro/coro.h>
#include <coro/runtime/clock.h>
#include <coro/sync/sleep.h>
#include <chrono>

namespace coro {

/**
 * @brief Drift-compensating periodic timer.
 *
 * Each `co_await timer.tick()` suspends until the next scheduled tick, subtracting
 * the time already spent on work since the previous tick. If the work consistently
 * takes longer than the period, the timer resets (no spiral of zero-length sleeps).
 *
 * The first tick() call waits one full period from construction.
 *
 * Typical usage in a frame loop:
 *
 *   IntervalTimer timer(std::chrono::milliseconds(20));  // 50 fps
 *   while (true) {
 *       do_work();
 *       co_await timer.tick();  // waits the remainder of the 20 ms window
 *   }
 */
class IntervalTimer {
public:
    explicit IntervalTimer(std::chrono::nanoseconds period) noexcept
        : m_period(std::chrono::ceil<Clock::duration>(period)),
          m_next(Clock::now() + m_period) {}

    [[nodiscard]] Coro<void> tick() {
        co_await sleep_until(m_next);
        m_next += m_period;
        // Drift guard: if we've fallen more than one period behind, reset rather
        // than trying to catch up (which would result in a burst of immediate ticks).
        const Instant now = Clock::now();
        if (m_next < now)
            m_next = now + m_period;
    }

private:
    Clock::duration m_period;
    Instant         m_next;
};

} // namespace coro
