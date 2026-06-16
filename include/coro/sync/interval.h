#pragma once

#include <coro/coro.h>
#include <coro/sync/sleep.h>
#include <chrono>

#ifdef CORO_PICO
#include <coro/runtime/runtime.h>
#endif

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
        : m_period(period) {}

    [[nodiscard]] Coro<void> tick() {
#ifdef CORO_PICO
        auto period_us = static_cast<uint64_t>(
            std::max<int64_t>(0,
                std::chrono::duration_cast<std::chrono::microseconds>(m_period).count()));
        if (!m_initialized) {
            m_next_us    = current_runtime().now_us() + period_us;
            m_initialized = true;
        }
        uint64_t now = current_runtime().now_us();
        if (now < m_next_us)
            co_await sleep_for(std::chrono::microseconds(m_next_us - now));
        m_next_us += period_us;
        // Drift guard: if we've fallen more than one period behind, reset rather
        // than trying to catch up (which would result in a burst of immediate ticks).
        now = current_runtime().now_us();
        if (m_next_us < now)
            m_next_us = now + period_us;
#else
        if (!m_initialized) {
            m_next        = std::chrono::steady_clock::now() + m_period;
            m_initialized = true;
        }
        auto now = std::chrono::steady_clock::now();
        if (now < m_next)
            co_await sleep_for(m_next - now);
        m_next += m_period;
        // Drift guard: same logic as the Pico branch.
        now = std::chrono::steady_clock::now();
        if (m_next < now)
            m_next = now + m_period;
#endif
    }

private:
    std::chrono::nanoseconds m_period;
    bool                     m_initialized = false;

#ifdef CORO_PICO
    uint64_t m_next_us = 0;
#else
    std::chrono::steady_clock::time_point m_next{};
#endif
};

} // namespace coro
