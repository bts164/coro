#pragma once

#include <coro/future.h>
#include <coro/detail/poll_result.h>
#include <coro/detail/context.h>
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
 *
 * Not thread-safe, and meant for one loop: await one tick() at a time.
 */
class IntervalTimer {
public:
    /**
     * @brief Future returned by @ref tick(). Completes at the timer's next tick.
     *
     * Satisfies @ref Future<void>. It is a @ref SleepFuture for the tick's deadline
     * plus a pointer back to the timer, so awaiting a tick allocates nothing: there
     * is no coroutine frame.
     *
     * The deadline is fixed when tick() is called. The poll() that returns ready
     * also moves the timer on to the following tick, so it must not be polled again
     * after that (as for any future).
     *
     * Dropping it mid-wait is safe: the sleep's timer is cancelled and the
     * IntervalTimer is left unchanged, so the next tick() waits for the same deadline.
     *
     * Holds a raw pointer to its IntervalTimer, which must outlive it and must not
     * move while it exists.
     */
    class TickFuture {
    public:
        using OutputType = void;

        // Movable even after a poll, as SleepFuture is.
        TickFuture(TickFuture&&) noexcept            = default;
        TickFuture& operator=(TickFuture&&) noexcept = default;

        PollResult<void> poll(detail::Context& ctx) {
            auto result = m_sleep.poll(ctx);
            if (result.isReady()) {
                m_timer->m_next += m_timer->m_period;
                // Drift guard: if we've fallen more than one period behind, reset rather
                // than trying to catch up (which would result in a burst of immediate ticks).
                if (const auto now = Clock::now(); m_timer->m_next < now) {
                    m_timer->m_next = now + m_timer->m_period;
                }
            }
            return result;
        }

    private:
        friend class IntervalTimer;

        explicit TickFuture(IntervalTimer* timer) noexcept
            : m_timer(timer), m_sleep(timer->m_next) {}

        IntervalTimer* m_timer;
        SleepFuture    m_sleep;
    };

    /// @brief Starts the timer: the first tick is one `period` from now.
    /// `period` is rounded up to the clock's resolution.
    explicit IntervalTimer(std::chrono::nanoseconds period) noexcept
        : m_period(std::chrono::ceil<Clock::duration>(period)),
          m_next(Clock::now() + m_period) {}

    /**
     * @brief Waits for the next tick.
     *
     * Ticks are one period apart whatever the time spent between calls. If the
     * caller has fallen more than a period behind, the tick is ready at once and
     * the schedule restarts one period from then, so missed ticks are skipped
     * instead of delivered in a burst.
     *
     * @return A future that is ready once the tick's deadline has passed. This
     *         timer must outlive it and must not move while it exists.
     */
    [[nodiscard]] TickFuture tick() noexcept { return TickFuture(this); }

private:
    Clock::duration m_period;
    // Deadline of the next tick. Advanced by the TickFuture that completes.
    Instant         m_next;
};

static_assert(Future<IntervalTimer::TickFuture>);

} // namespace coro
