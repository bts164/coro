#pragma once

#include <coro/detail/poll_result.h>
#include <coro/detail/context.h>
#include <chrono>

namespace coro {

// SleepFuture — completes once the wall-clock deadline has passed.
//
// NOTE: Without a libuv event loop, this future does not proactively register a
// waker for the deadline. It will complete on the next poll() after the deadline,
// but the executor will not wake the task at the precise deadline time. This
// limitation will be removed when libuv timer integration is implemented.
struct SleepFuture {
    using OutputType = void;

    std::chrono::steady_clock::time_point m_deadline;

    explicit SleepFuture(std::chrono::nanoseconds duration)
        : m_deadline(std::chrono::steady_clock::now() + duration) {}

    PollResult<void> poll(detail::Context&) {
        if (std::chrono::steady_clock::now() >= m_deadline)
            return PollReady;
        // TODO: register a libuv timer waker so the executor is woken at m_deadline.
        // Until then, this returns Pending without registering a waker — the task
        // will only wake if another future in the same task wakes it.
        return PollPending;
    }
};

// Returns a SleepFuture that completes after the given duration.
[[nodiscard]] inline SleepFuture sleep_for(std::chrono::nanoseconds duration) {
    return SleepFuture(duration);
}

} // namespace coro
