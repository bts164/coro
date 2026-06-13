#include <chrono>
#include <cstdint>

// No-op cyw43_arch_poll for executor-only tests.
// The executor calls this in its poll loop in place of blocking; without any
// TCP sources compiled there are no lwIP callbacks to fire, so a no-op is correct.
void cyw43_arch_poll() {}

// time_us_64 stub — returns microseconds since the first call, matching the
// Pico SDK semantics (microseconds since boot) closely enough for timer tests.
uint64_t time_us_64() {
    static const auto start = std::chrono::steady_clock::now();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start).count());
}
