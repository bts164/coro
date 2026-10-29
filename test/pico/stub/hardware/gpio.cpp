#include "gpio.h"

#include <mutex>

// ---------------------------------------------------------------------------
// Stub state
// ---------------------------------------------------------------------------

static constexpr uint kNumGpios = 30;

// Each access is one register read or write on the board. Here the IRQ handler runs
// on a test's thread (set_level(), then the handler re-arming or disarming the IRQ)
// while the task reads the level and arms the IRQ, so every access takes g_mutex.
static std::mutex g_mutex;
static bool     g_level[kNumGpios]         = {};
static uint32_t g_enabled_mask[kNumGpios]  = {};
static int      g_irq_call_count[kNumGpios] = {};

// ---------------------------------------------------------------------------
// Configuration -- no-ops beyond what tests need to observe
// ---------------------------------------------------------------------------

void gpio_init(uint) {}
void gpio_set_dir(uint, bool) {}
void gpio_pull_up(uint) {}
void gpio_pull_down(uint) {}
void gpio_disable_pulls(uint) {}

// ---------------------------------------------------------------------------
// Level read/write
// ---------------------------------------------------------------------------

bool gpio_get(uint gpio) {
    std::lock_guard lock(g_mutex);
    return gpio < kNumGpios && g_level[gpio];
}

void gpio_put(uint gpio, bool value) {
    std::lock_guard lock(g_mutex);
    if (gpio < kNumGpios)
        g_level[gpio] = value;
}

// ---------------------------------------------------------------------------
// IRQ configuration -- no real IRQ line; tests fire the handler directly
// ---------------------------------------------------------------------------

void gpio_set_irq_enabled(uint gpio, uint32_t event_mask, bool enabled) {
    if (gpio >= kNumGpios)
        return;
    std::lock_guard lock(g_mutex);
    ++g_irq_call_count[gpio];
    if (enabled)
        g_enabled_mask[gpio] |= event_mask;
    else
        g_enabled_mask[gpio] &= ~event_mask;
}

void gpio_set_irq_enabled_with_callback(uint, uint32_t, bool, gpio_irq_callback_t) {}

// ---------------------------------------------------------------------------
// Test helpers
// ---------------------------------------------------------------------------

namespace gpio_stub {

void set_level(uint gpio, bool level) {
    std::lock_guard lock(g_mutex);
    if (gpio < kNumGpios)
        g_level[gpio] = level;
}

void reset() {
    std::lock_guard lock(g_mutex);
    for (auto& l : g_level) l = false;
    for (auto& m : g_enabled_mask) m = 0;
    for (auto& c : g_irq_call_count) c = 0;
}

uint32_t enabled_mask(uint gpio) {
    std::lock_guard lock(g_mutex);
    return gpio < kNumGpios ? g_enabled_mask[gpio] : 0;
}

int irq_enable_call_count(uint gpio) {
    std::lock_guard lock(g_mutex);
    return gpio < kNumGpios ? g_irq_call_count[gpio] : 0;
}

} // namespace gpio_stub
