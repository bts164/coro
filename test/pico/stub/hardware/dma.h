#pragma once
#include <cstdint>

// Stub for Pico SDK <hardware/dma.h> used in Linux unit-test builds.
// Only the symbols used by AsyncDmaTransfer are stubbed. Real RP2040 hardware
// transfers do not happen; the stub tracks channel claim/release state and
// exposes test helpers to simulate DMA completion IRQs.

using uint = unsigned int;

static constexpr uint NUM_DMA_CHANNELS = 12;
static constexpr uint DMA_IRQ_0 = 11;  // matches Pico SDK value
static constexpr uint8_t PICO_SHARED_IRQ_HANDLER_DEFAULT_ORDER_PRIORITY = 0x80;

// Transfer data sizes (match pico SDK enum values)
enum dma_channel_transfer_size {
    DMA_SIZE_8  = 0,
    DMA_SIZE_16 = 1,
    DMA_SIZE_32 = 2,
};

struct dma_channel_config {
    uint32_t ctrl = 0;
};

using irq_handler_t = void(*)();

// ---------------------------------------------------------------------------
// Channel config helpers — all no-ops in the stub
// ---------------------------------------------------------------------------
inline dma_channel_config dma_channel_get_default_config(uint) { return {}; }
inline void channel_config_set_transfer_data_size(dma_channel_config*, dma_channel_transfer_size) {}
inline void channel_config_set_read_increment(dma_channel_config*, bool) {}
inline void channel_config_set_write_increment(dma_channel_config*, bool) {}
inline void channel_config_set_dreq(dma_channel_config*, uint) {}

// ---------------------------------------------------------------------------
// Channel claim / release — track which channels are claimed
// ---------------------------------------------------------------------------
int  dma_claim_unused_channel(bool required);
void dma_channel_unclaim(uint ch);

// ---------------------------------------------------------------------------
// Transfer control
// ---------------------------------------------------------------------------
void dma_channel_configure(uint ch, const dma_channel_config*,
                            volatile void* write_addr, const volatile void* read_addr,
                            uint transfer_count, bool trigger);
void dma_channel_start(uint ch);
void dma_channel_abort(uint ch);

// ---------------------------------------------------------------------------
// IRQ status — controlled by test helpers below
// ---------------------------------------------------------------------------
bool dma_irqn_get_channel_status(uint irq_index, uint ch);
void dma_irqn_acknowledge_channel(uint irq_index, uint ch);

// ---------------------------------------------------------------------------
// IRQ registration — stubs record the handler; tests invoke it via helpers
// ---------------------------------------------------------------------------
void irq_add_shared_handler(uint irq_num, irq_handler_t handler, uint8_t order_priority);
void irq_set_enabled(uint irq_num, bool enabled);

// ---------------------------------------------------------------------------
// Test helpers
//
// Call dma_stub_complete_channel() to simulate a DMA completion IRQ on the
// given channel. Then call coro_pico_hal_dma_fire_irq0() to invoke the
// coro_pico_hal IRQ handler, which will signal the waiting IsrEvent.
// ---------------------------------------------------------------------------
namespace dma_stub {
    // Mark channel ch as having completed (sets its IRQ status bit).
    void complete_channel(uint ch);

    // Reset all stub state (claimed channels, IRQ status). Call in test teardown.
    void reset();
}

// Defined in src/pico/hal/dma.cpp (compiled with CORO_PICO_TEST).
// Invokes the internal DMA IRQ0 dispatch handler directly.
void coro_pico_hal_dma_fire_irq0();
