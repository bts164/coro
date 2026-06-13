#include "dma.h"
#include <stdexcept>
#include <cstdint>

// ---------------------------------------------------------------------------
// Stub state
// ---------------------------------------------------------------------------

static bool     g_claimed[NUM_DMA_CHANNELS] = {};
static uint32_t g_irq_status[2]             = {};  // [irq_index] bitmask of pending channels
static int      g_next_channel              = 0;

// ---------------------------------------------------------------------------
// Channel claim / release
// ---------------------------------------------------------------------------

int dma_claim_unused_channel(bool required) {
    for (uint ch = 0; ch < NUM_DMA_CHANNELS; ++ch) {
        if (!g_claimed[ch]) {
            g_claimed[ch] = true;
            return static_cast<int>(ch);
        }
    }
    if (required)
        throw std::runtime_error("dma_stub: no free DMA channels");
    return -1;
}

void dma_channel_unclaim(uint ch) {
    if (ch < NUM_DMA_CHANNELS)
        g_claimed[ch] = false;
}

// ---------------------------------------------------------------------------
// Transfer control — no actual DMA; state is driven by test helpers
// ---------------------------------------------------------------------------

void dma_channel_configure(uint, const dma_channel_config*,
                            volatile void*, const volatile void*, uint, bool) {}
void dma_channel_start(uint) {}
void dma_channel_abort(uint) {}

// ---------------------------------------------------------------------------
// IRQ status
// ---------------------------------------------------------------------------

bool dma_irqn_get_channel_status(uint irq_index, uint ch) {
    if (irq_index >= 2 || ch >= NUM_DMA_CHANNELS) return false;
    return (g_irq_status[irq_index] >> ch) & 1u;
}

void dma_irqn_acknowledge_channel(uint irq_index, uint ch) {
    if (irq_index >= 2 || ch >= NUM_DMA_CHANNELS) return;
    g_irq_status[irq_index] &= ~(1u << ch);
}

// ---------------------------------------------------------------------------
// IRQ registration — stubs just record the handler (not invoked automatically)
// ---------------------------------------------------------------------------

void irq_add_shared_handler(uint, irq_handler_t, uint8_t) {}
void irq_set_enabled(uint, bool) {}

// ---------------------------------------------------------------------------
// Test helpers
// ---------------------------------------------------------------------------

namespace dma_stub {

void complete_channel(uint ch) {
    if (ch < NUM_DMA_CHANNELS)
        g_irq_status[0] |= (1u << ch);
}

void reset() {
    for (auto& c : g_claimed)    c = false;
    for (auto& s : g_irq_status) s = 0;
}

} // namespace dma_stub
