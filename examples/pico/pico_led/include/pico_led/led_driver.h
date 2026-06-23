#pragma once
#include <memory>
#include <cstddef>
#include <vector>

#include <coro/coro.h>
#include <coro/pico/hal/dma.h>
#include <hardware/pio.h>

#include "pico_led/pixel_buffer.h"

namespace led {

// Drives a WS2812 LED strip via PIO + AsyncDmaTransfer.
//
// show() builds the GRB DMA buffer (applying brightness), then delegates to
// AsyncDmaTransfer::transfer() which suspends until DMA_IRQ_0 fires — no
// manual ISR handler needed.
//
// Non-movable (AsyncDmaTransfer is non-movable).  Always access via the
// shared_ptr returned by create().
class LedDriver {
public:
    // Use create() — direct construction reserves a DMA channel immediately.
    LedDriver();

    ~LedDriver() = default;
    LedDriver(const LedDriver&)             = delete;
    LedDriver& operator=(const LedDriver&)  = delete;
    LedDriver(LedDriver&&)                  = delete;
    LedDriver& operator=(LedDriver&&)       = delete;

    // Claim a DMA channel and initialise the WS2812 PIO program.
    [[nodiscard]]
    static coro::Coro<std::shared_ptr<LedDriver>> create(
        uint data_pin,
        std::size_t led_count,
        PIO  pio = pio0,
        uint sm  = 0);

    // Push pixels to the strip, applying the global brightness setting.
    // When brightness == 255 the buffer is DMA'd directly; otherwise a
    // scaled copy is produced first.  Suspends until DMA completes, then
    // waits the WS2812 reset interval.
    [[nodiscard]] coro::Coro<void> show(const PixelBuffer& pixels);

    void        set_brightness(uint8_t v) noexcept { m_brightness = v; }
    uint8_t     brightness()        const noexcept { return m_brightness; }

    std::size_t led_count()         const noexcept { return m_led_count; }
    void        set_led_count(std::size_t n)        { m_led_count = n; }

    // GRB words from the most recent show() call (pre-brightness scaling).
    // Used by the set command to read-modify-write the current frame.
    const PixelBuffer& buffer() const noexcept { return m_last_frame; }

private:
    PIO         m_pio{};
    uint        m_sm{};
    std::size_t m_led_count{};
    uint8_t     m_brightness{255};

    coro::pico::hal::AsyncDmaTransfer m_dma;
    dma_channel_config                m_cfg{};

    // Scratch buffer for brightness-scaled GRB words.  Only allocated when
    // brightness < 255.
    std::vector<uint32_t> m_scaled_buf;

    // Pre-brightness GRB words from the last show() call.
    PixelBuffer m_last_frame;
};

} // namespace led
