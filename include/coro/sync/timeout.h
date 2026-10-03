#pragma once

#include <coro/sync/select.h>
#include <coro/sync/sleep.h>
#include <coro/future.h>
#include <chrono>
#include <utility>

namespace coro {

/**
 * @brief Wraps a @ref Future with a deadline, returning whichever completes first.
 *
 * Returns a `SelectFuture<F, SleepFuture>` whose `OutputType` is:
 * - `SelectBranch<0, T>` — `future` completed before the deadline; `value` holds the result.
 * - `SelectBranch<1, void>` — the deadline elapsed before `future` completed.
 *
 * Example:
 * @code
 * auto result = co_await timeout(std::chrono::seconds(5), fetch_data());
 * if (result.index() == 0)
 *     use(std::get<0>(result).value);
 * else
 *     handle_timeout();
 * @endcode
 *
 * @param duration  Maximum time to wait for `future`.
 * @param future    The future to race against the deadline.
 * @return A `SelectFuture` that resolves to either the future's result or a timeout signal.
 */
template<Future F>
[[nodiscard]] auto timeout(std::chrono::nanoseconds duration, F future) {
    return select(std::move(future), sleep_for(duration));
}

/**
 * @brief Like @ref timeout, but with an absolute deadline.
 *
 * @param deadline  The instant after which `future` is abandoned.
 * @param future    The future to race against the deadline.
 */
template<Future F>
[[nodiscard]] auto timeout_at(Instant deadline, F future) {
    return select(std::move(future), sleep_until(deadline));
}

} // namespace coro
