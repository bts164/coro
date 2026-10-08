#pragma once

#include <coro/detail/poll_result.h>
#include <coro/detail/context.h>
#include <coro/detail/rc.h>
#include <coro/detail/timer_queue.h>
#include <coro/runtime/clock.h>
#include <chrono>
#include <utility>

namespace coro {

class Runtime;

/**
 * @brief Future that completes once a @ref Clock deadline has passed.
 *
 * Satisfies @ref Future<void>. The first pending `poll()` adds a timer to the
 * current runtime's queue: the IoDriver's on desktop, where the deadline bounds the
 * driver's `epoll_pwait2` at nanosecond resolution, or the CurrentThreadExecutor's
 * on Pico. The timer is an entry in the queue's own storage; the future holds only
 * its id, and nothing is allocated.
 *
 * A later pending poll does nothing if it brings the waker already registered, which
 * is the usual case. If it brings a different one (the future lives in a coroutine
 * frame that another task now polls), the timer is cancelled and added again for the
 * new waker.
 *
 * A leaf future with no `cancel()`: dropping it mid-wait is always safe. It cancels
 * its timer, and the queue entry is later removed without a wake.
 *
 * `poll()` checks the clock itself, so it is never ready early, and an early or
 * spurious wake just leaves it pending.
 *
 * The runtime it was first polled on must outlive it.
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
    ~SleepFuture() { cancel_timer(); }

    // The timer is named by id, not by this object's address, so a move is safe
    // even after a poll.
    SleepFuture(SleepFuture&& other) noexcept
        : m_deadline(other.m_deadline),
          m_runtime(std::exchange(other.m_runtime, nullptr)),
          m_timer(other.m_timer),
          m_waker(std::move(other.m_waker)) {}
    SleepFuture& operator=(SleepFuture&& other) noexcept {
        if (this != &other) {
            cancel_timer();
            m_deadline = other.m_deadline;
            m_runtime  = std::exchange(other.m_runtime, nullptr);
            m_timer    = other.m_timer;
            m_waker    = std::move(other.m_waker);
        }
        return *this;
    }
    SleepFuture(const SleepFuture&)            = delete;
    SleepFuture& operator=(const SleepFuture&) = delete;

    PollResult<void> poll(detail::Context& cx);

    /// The instant this future becomes ready.
    Instant deadline() const noexcept { return m_deadline; }

private:
    /// Cancels the timer, if one is registered. Harmless if it has already fired.
    void cancel_timer() noexcept;

    Instant                     m_deadline;
    // The runtime the timer is registered with; null while none is.
    Runtime*                    m_runtime = nullptr;
    // Valid while m_runtime is set.
    detail::TimerId             m_timer   = 0;
    // The waker the timer was registered with, to recognise a re-poll that brings
    // the same one. Holding it also keeps that comparison sound: see same_rc().
    detail::Weak<detail::Waker> m_waker;
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
