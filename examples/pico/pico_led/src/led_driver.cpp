#include <coro/sync/sleep.h>
#include <hardware/clocks.h>
#include <chrono>
#include <algorithm>
#include <stdexcept>

#include "pico_led/led_driver.h"
#include "pico_led/ws2812.pio.h"

namespace led {

LedDriver::LedDriver() = default;  // m_dma default-constructs and claims a channel

coro::Coro<std::shared_ptr<LedDriver>> LedDriver::create(
        uint data_pin, std::size_t led_count, PIO pio, uint sm) {

    auto d = std::make_shared<LedDriver>();  // DMA channel claimed here
    d->m_pio       = pio;
    d->m_sm        = sm;
    d->m_led_count = led_count;

    if (!pio_can_add_program(pio, &ws2812_program))
        panic("led::LedDriver: no PIO program space");

    uint offset = pio_add_program(pio, &ws2812_program);
    pio_sm_claim(pio, sm);
    ws2812_program_init(pio, sm, offset, data_pin, 800000.0f);

    // Pre-configure the DMA channel for PIO TX FIFO transfers.  read_addr and
    // transfer_count are set per-transfer in show().
    dma_channel_config cfg = dma_channel_get_default_config(
        static_cast<uint>(d->m_dma.channel()));
    channel_config_set_transfer_data_size(&cfg, DMA_SIZE_32);
    channel_config_set_dreq(&cfg, pio_get_dreq(pio, sm, true));
    channel_config_set_read_increment(&cfg, true);
    channel_config_set_write_increment(&cfg, false);
    d->m_cfg = cfg;

    co_return d;
}

coro::Coro<void> LedDriver::show(const PixelBuffer& pixels) {
    std::size_t n = std::min(pixels.size(), m_led_count);

    // Save pre-brightness copy so buffer() reflects what was requested.
    m_last_frame.assign(pixels.begin(), pixels.begin() + static_cast<std::ptrdiff_t>(n));
    m_last_frame.resize(m_led_count, 0u);

    const uint32_t* src;
    if (m_brightness == 255) {
        // DMA directly from the caller's buffer — safe because the caller is
        // suspended at co_await show() for the duration of the transfer.
        src = m_last_frame.data();
    } else {
        // Scale each GRB channel by brightness.
        m_scaled_buf.resize(m_led_count);
        uint16_t b16 = m_brightness;
        for (std::size_t i = 0; i < m_led_count; ++i) {
            uint32_t w = m_last_frame[i];
            uint8_t g = static_cast<uint8_t>(((w >> 24) & 0xFF) * b16 >> 8);
            uint8_t r = static_cast<uint8_t>(((w >> 16) & 0xFF) * b16 >> 8);
            uint8_t b = static_cast<uint8_t>(((w >>  8) & 0xFF) * b16 >> 8);
            m_scaled_buf[i] = (uint32_t(g) << 24) | (uint32_t(r) << 16) | (uint32_t(b) << 8);
        }
        src = m_scaled_buf.data();
    }

    co_await m_dma.transfer(m_cfg, src, &m_pio->txf[m_sm],
                            static_cast<uint>(m_led_count));

    // WS2812 requires >50 µs of idle before the next frame latches.
    co_await coro::sleep_for(std::chrono::microseconds(60));
}

} // namespace led
