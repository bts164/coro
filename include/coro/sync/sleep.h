#pragma once

#include <coro/detail/poll_result.h>
#include <coro/detail/context.h>
#include <coro/runtime/io_service.h>
#include <chrono>
#include <memory>

namespace coro {

/**
 * @brief Future that completes once a wall-clock deadline has passed.
 *
 * Satisfies @ref Future<void>. On the first `poll()` call after the deadline
 * has not yet passed, registers a one-shot libuv timer via @ref IoService.
 * The I/O thread fires the timer at the deadline and calls `waker->wake()`,
 * which re-enqueues the task. The next `poll()` then sees
 * `steady_clock::now() >= m_deadline` and returns `PollReady`.
 *
 * @note Timer resolution is **milliseconds** (libuv limitation). Durations
 *       shorter than 1ms are rounded up to 1ms. The actual wake latency is
 *       also subject to I/O thread scheduling jitter, so sub-millisecond
 *       precision is not achievable regardless.
 *
 * @note `SleepFuture` must not be shared across threads. It is intended to
 *       live inside a coroutine frame and be polled by a single executor thread.
 *
 * Prefer the @ref sleep_for factory function over constructing this directly.
 */
struct SleepFuture {
    using OutputType = void;

    std::chrono::steady_clock::time_point m_deadline;
    /// Null until first poll(); once set, owned jointly with the I/O thread.
    std::shared_ptr<TimerState> m_state;

    explicit SleepFuture(std::chrono::nanoseconds duration)
        : m_deadline(std::chrono::steady_clock::now() + duration) {}

    ~SleepFuture() {
        if (m_state) {
            // Always submit CancelTimer — CancelTimer::execute() uses fired.exchange(true)
            // to avoid a double-close if the timer already fired before we get there.
            // Pass m_state as a shared_ptr so TimerState stays alive until the I/O
            // thread processes the request, even though we release m_state below.
            current_io_service().submit(std::make_unique<CancelTimer>(m_state));
        }
    }

    // Not copyable; moving after first poll() is safe (m_state transfers cleanly).
    SleepFuture(const SleepFuture&)            = delete;
    SleepFuture& operator=(const SleepFuture&) = delete;
    SleepFuture(SleepFuture&&)                 = default;
    SleepFuture& operator=(SleepFuture&&)      = default;

    PollResult<void> poll(detail::Context& ctx) {
        if (std::chrono::steady_clock::now() >= m_deadline)
            return PollReady;

        if (!m_state) {
            // First poll: allocate shared state and register the timer.
            m_state = std::make_shared<TimerState>();
            m_state->waker.store(ctx.getWaker());
            current_io_service().submit(
                std::make_unique<StartTimer>(m_state.get(), m_deadline));
        } else {
            // Re-polled before timer fired (e.g. woken by a select branch).
            // Atomically update the waker — timer_cb may read it concurrently.
            m_state->waker.store(ctx.getWaker());
        }
        return PollPending;
    }
};

/**
 * @brief Returns a @ref SleepFuture that completes after `duration` has elapsed.
 *
 * Timer resolution is milliseconds; see @ref SleepFuture for details.
 */
[[nodiscard]] inline SleepFuture sleep_for(std::chrono::nanoseconds duration) {
    return SleepFuture(duration);
}

} // namespace coro
