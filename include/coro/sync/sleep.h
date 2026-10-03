#pragma once

#include <coro/detail/poll_result.h>
#include <coro/detail/context.h>
#include <coro/detail/rc.h>
#include <coro/detail/timer_queue.h>
#include <coro/runtime/clock.h>
#include <chrono>

namespace coro {

/**
 * @brief Future that completes once a @ref Clock deadline has passed.
 *
 * Satisfies @ref Future<void>. The first pending `poll()` adds a timer to the
 * current runtime's queue: the IoDriver's on desktop, where the deadline bounds the
 * driver's `epoll_pwait2` at nanosecond resolution, or the CurrentThreadExecutor's
 * on Pico. Every later pending poll replaces the stored waker, so the latest
 * context is the one woken (e.g. under `select`).
 *
 * A leaf future with no `cancel()`: dropping it mid-wait is always safe. It empties
 * its timer slot, and the queue entry is later popped without a wake.
 *
 * `poll()` checks the clock itself, so it is never ready early, and an early or
 * spurious wake just leaves it pending.
 *
 * @throws std::logic_error from the first pending `poll()` if the runtime's executor
 *         never turns the IoDriver (see `Runtime::add_timer()`).
 *
 * Prefer the @ref sleep_for / @ref sleep_until factories over constructing this.
 */
class SleepFuture {
public:
    using OutputType = void;

    explicit SleepFuture(Instant deadline) noexcept : m_deadline(deadline) {}
    ~SleepFuture() { release(); }

    SleepFuture(SleepFuture&& other) noexcept = default;
    SleepFuture& operator=(SleepFuture&& other) noexcept {
        if (this != &other) {
            release();
            m_deadline = other.m_deadline;
            m_slot     = std::move(other.m_slot);
        }
        return *this;
    }
    SleepFuture(const SleepFuture&)            = delete;
    SleepFuture& operator=(const SleepFuture&) = delete;

    PollResult<void> poll(detail::Context& cx);

    /// The instant this future becomes ready.
    Instant deadline() const noexcept { return m_deadline; }

private:
    /// Empties the slot's waker, so the queue entry fires nothing. Idempotent.
    void release() noexcept;

    Instant                       m_deadline;
    // Null until the first pending poll; shared with the timer queue's entry.
    detail::Rc<detail::TimerSlot> m_slot;
};

/// @brief Completes once `deadline` has passed.
[[nodiscard]] inline SleepFuture sleep_until(Instant deadline) {
    return SleepFuture(deadline);
}

/// @brief Completes once `duration` has elapsed, measured from this call.
[[nodiscard]] inline SleepFuture sleep_for(std::chrono::nanoseconds duration) {
    return SleepFuture(Clock::now() + std::chrono::ceil<Clock::duration>(duration));
}

} // namespace coro
