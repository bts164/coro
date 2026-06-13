#pragma once
// RP2040-specific async DMA primitive. Part of the optional coro_pico_hal
// cmake target — only available when that target is linked.

#ifdef CORO_PICO

#include <hardware/dma.h>
#include <coro/coro.h>
#include <coro/sync/isr_event.h>

namespace coro::pico::hal {

/**
 * @brief RAII async DMA channel wrapper.
 *
 * Claims one DMA channel on construction, releases it on destruction.
 * transfer() configures and starts a DMA transfer, then suspends the calling
 * coroutine until the DMA_IRQ_0 completion interrupt fires.
 *
 * Only one transfer may be in progress at a time per AsyncDmaTransfer instance.
 *
 * Cancellation: if the coroutine awaiting transfer() is cancelled, the RAII
 * destructor of the internal future calls dma_channel_abort() immediately,
 * stopping the DMA engine. The PIO TX FIFO (or other peripheral) may contain
 * stale data for one bus cycle after abort; this is acceptable for WS2812B.
 *
 * Usage:
 * @code
 * coro::pico::hal::AsyncDmaTransfer dma;  // claims a channel
 *
 * dma_channel_config cfg = dma_channel_get_default_config(dma.channel());
 * channel_config_set_transfer_data_size(&cfg, DMA_SIZE_32);
 * channel_config_set_dreq(&cfg, pio_get_dreq(pio, sm, true));
 * // ... other config ...
 *
 * co_await dma.transfer(cfg, src, dst, count);  // suspends until IRQ fires
 * @endcode
 */
class AsyncDmaTransfer {
public:
    // Claims an unused DMA channel. Registers the shared DMA_IRQ_0 handler on
    // the first construction. Throws std::runtime_error if no channels are free.
    AsyncDmaTransfer();

    // Aborts any in-progress transfer and releases the channel.
    ~AsyncDmaTransfer();

    AsyncDmaTransfer(const AsyncDmaTransfer&)             = delete;
    AsyncDmaTransfer& operator=(const AsyncDmaTransfer&)  = delete;
    AsyncDmaTransfer(AsyncDmaTransfer&&)                  = delete;
    AsyncDmaTransfer& operator=(AsyncDmaTransfer&&)       = delete;

    // Returns the claimed channel number. Useful for computing DREQ values
    // (e.g. pio_get_dreq(pio, sm, true) for PIO TX FIFO DMA).
    [[nodiscard]] int channel() const { return m_channel; }

    // Configures and starts the DMA transfer described by ctrl/read_addr/
    // write_addr/transfer_count, then suspends until the DMA_IRQ_0 handler
    // fires for this channel.
    //
    // Cancellable: dma_channel_abort() is called immediately if the awaiting
    // coroutine is cancelled. The abort is synchronous — the channel is free
    // to reuse as soon as this coroutine resumes after cancellation.
    [[nodiscard]] Coro<void> transfer(const dma_channel_config& ctrl,
                                      const volatile void*       read_addr,
                                      volatile void*             write_addr,
                                      uint                       transfer_count);

private:
    int      m_channel;
    IsrEvent m_done;
    // m_done is registered in a module-internal IsrEvent* dispatch table indexed
    // by channel number. The shared DMA_IRQ_0 handler calls
    // dispatch_table[ch]->signal_from_isr() when the channel completes.
};

} // namespace coro::pico::hal

#endif // CORO_PICO
