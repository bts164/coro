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
 * coroutine until the DMA_IRQ_0 completion interrupt fires. start()/wait()
 * split those two steps apart for callers that must start a transfer from a
 * plain, non-coroutine context and await its completion elsewhere.
 *
 * Only one transfer may be in progress at a time per AsyncDmaTransfer instance.
 *
 * Cancellation: if the coroutine awaiting transfer()/wait() is cancelled, the
 * RAII destructor of the internal future calls dma_channel_abort()
 * immediately, stopping the DMA engine. The PIO TX FIFO (or other peripheral)
 * may contain stale data for one bus cycle after abort.
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
 *
 * Decoupled start/wait, e.g. from a synchronous callback that starts the
 * transfer and a coroutine elsewhere that awaits it:
 * @code
 * dma.start(cfg, src, dst, count);   // non-blocking, returns immediately
 * // ... later, possibly in different code ...
 * co_await dma.wait();
 * @endcode
 */
class AsyncDmaTransfer {
public:
    // Claims an unused DMA channel. Registers the shared DMA_IRQ_0 handler on
    // the first construction. Throws std::runtime_error if no channels are free.
    //
    // track_completion=false skips enabling this channel's DMA_IRQ_0 line and
    // registering it in the completion dispatch table -- for a channel whose
    // completion is never awaited (e.g. a full-duplex SPI transfer's RX-side
    // sink channel is the true completion signal; the paired TX channel's own
    // completion is meaningless and must not be waited on). wait()/transfer()
    // must not be called on such an instance; only start() is valid.
    explicit AsyncDmaTransfer(bool track_completion = true);

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
    // fires for this channel. Equivalent to start() followed by co_await wait().
    //
    // Cancellable: dma_channel_abort() is called immediately if the awaiting
    // coroutine is cancelled. The abort is synchronous — the channel is free
    // to reuse as soon as this coroutine resumes after cancellation.
    [[nodiscard]] Coro<void> transfer(const dma_channel_config& ctrl,
                                      const volatile void*       read_addr,
                                      volatile void*             write_addr,
                                      uint                       transfer_count);

    // Configures and starts the DMA transfer, then returns immediately
    // without waiting for it -- safe to call from a plain synchronous
    // callback that must not suspend. Must be constructed with
    // track_completion=true if wait() will be called afterward; a
    // track_completion=false instance may call start() but never wait().
    void start(const dma_channel_config& ctrl,
               const volatile void*       read_addr,
               volatile void*             write_addr,
               uint                       transfer_count);

    // Suspends until the transfer started by start() completes. Must be
    // called exactly once per start() call, only after it, and only on an
    // instance constructed with track_completion=true.
    //
    // Cancellable: same as transfer() -- dma_channel_abort() is called
    // immediately if the awaiting coroutine is cancelled.
    [[nodiscard]] Coro<void> wait();

private:
    int      m_channel;
    bool     m_track_completion;
    IsrEvent m_done;
    // m_done is registered in a module-internal IsrEvent* dispatch table indexed
    // by channel number. The shared DMA_IRQ_0 handler calls
    // dispatch_table[ch]->signal_from_isr() when the channel completes.
    // Left unregistered (and this channel's DMA_IRQ_0 line left disabled) when
    // m_track_completion is false.
};

} // namespace coro::pico::hal

#endif // CORO_PICO
