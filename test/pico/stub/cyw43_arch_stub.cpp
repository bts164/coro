#include <pico/async_context_poll.h>
#include <pico/cyw43_arch.h>
#include <pico/lwip_nosys.h>

#include <chrono>
#include <cstdint>

namespace {
async_context_poll_t g_context{};
bool                 g_initialised = false;
} // namespace

// As the SDK's poll-mode cyw43_arch_init(): a poll context with lwIP on it. There is
// no radio to drive.
int cyw43_arch_init() {
    if (g_initialised) return 0;
    async_context_poll_init_with_defaults(&g_context);
    lwip_nosys_init(&g_context.core);
    g_initialised = true;
    return 0;
}

void cyw43_arch_deinit() {
    if (!g_initialised) return;
    lwip_nosys_deinit(&g_context.core);
    async_context_deinit(&g_context.core);
    g_initialised = false;
}

async_context_t* cyw43_arch_async_context() {
    return g_initialised ? &g_context.core : nullptr;
}

void cyw43_arch_poll() {
    if (g_initialised) async_context_poll(&g_context.core);
}

// time_us_64 stub — returns microseconds since the first call, matching the
// Pico SDK semantics (microseconds since boot) closely enough for timer tests.
uint64_t time_us_64() {
    static const auto start = std::chrono::steady_clock::now();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start).count());
}
