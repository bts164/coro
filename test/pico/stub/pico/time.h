#pragma once
// Stub for pico/time.h — timer functions backed by steady_clock on Linux test builds.
// time_us_64() is declared here and defined in cyw43_arch_stub.cpp.
#include <cstdint>

uint64_t time_us_64();

// absolute_time_t is microseconds since the first time_us_64() call, as the SDK's is
// microseconds since boot. A plain integer, as in a non-debug SDK build.
using absolute_time_t = uint64_t;

inline constexpr absolute_time_t at_the_end_of_time = UINT64_MAX;

inline absolute_time_t get_absolute_time() { return time_us_64(); }

inline uint64_t to_us_since_boot(absolute_time_t t) { return t; }

// Saturates at at_the_end_of_time, as the SDK's delayed_by_us() does.
inline absolute_time_t make_timeout_time_us(uint64_t us) {
    const uint64_t now = time_us_64();
    return us > at_the_end_of_time - now ? at_the_end_of_time : now + us;
}

inline absolute_time_t make_timeout_time_ms(uint32_t ms) {
    return make_timeout_time_us(static_cast<uint64_t>(ms) * 1000u);
}
