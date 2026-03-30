#pragma once

#include <coro/sync/select.h>
#include <coro/sync/sleep.h>
#include <coro/future.h>
#include <chrono>
#include <utility>

namespace coro {

// timeout(duration, future) wraps a Future with a deadline.
//
// Returns SelectFuture<F, SleepFuture> whose OutputType is
//   std::variant<SelectBranch<0, T>, SelectBranch<1, void>>
// where:
//   SelectBranch<0, T>    — the future completed in time; value() holds the result.
//   SelectBranch<1, void> — the deadline elapsed before the future completed.
//
// NOTE: Proactive wakeup at the deadline requires libuv integration (see sleep.h).
// Until then, the timeout fires on the next poll after the deadline passes.
template<Future F>
[[nodiscard]] auto timeout(std::chrono::nanoseconds duration, F future) {
    return select(std::move(future), sleep_for(duration));
}

} // namespace coro
