#pragma once

// The runtime's monotonic clock: every timer deadline is a coro::Instant.
// See doc/design/io_driver.md, "Timers".

#include <chrono>
#include <cstdint>

namespace coro {

#ifdef CORO_PICO
/**
 * @brief Microseconds since boot, from the Pico SDK's `time_us_64()`.
 *
 * Meets the standard's *Clock* requirements, so `Instant` arithmetic with
 * `std::chrono` durations works as it does with `std::chrono::steady_clock`.
 */
struct PicoClock {
    using rep        = int64_t;
    using period     = std::micro;
    using duration   = std::chrono::duration<rep, period>;
    using time_point = std::chrono::time_point<PicoClock>;
    static constexpr bool is_steady = true;

    /// Defined in runtime.cpp, so this header doesn't pull in the SDK.
    static time_point now() noexcept;
};
using Clock = PicoClock;
#else
using Clock = std::chrono::steady_clock;
#endif

/// A point on @ref Clock. Timer deadlines (`sleep_until()`, `timeout_at()`) are Instants.
using Instant = Clock::time_point;

} // namespace coro
