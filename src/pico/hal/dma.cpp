#ifdef CORO_PICO

#include <coro/pico/hal/dma.h>
#include <hardware/dma.h>
#include <hardware/irq.h>
#include <stdexcept>
#include <mutex>

// ---------------------------------------------------------------------------
// Module-internal dispatch table and IRQ handler at file scope.
//
// Keeping these outside any namespace lets the test hook (global linkage) call
// the handler directly without namespace tricks.
//
// Protected only by the single-core / IRQ-disable guarantee of RP2040:
// entries are written from coroutine context and read from ISR context on the
// same core, with no concurrent access from core 1 in the intended use case.
// ---------------------------------------------------------------------------

static coro::IsrEvent* s_dispatch[NUM_DMA_CHANNELS] = {};
static std::once_flag  s_irq_registered;

static void dma_irq0_handler() {
    for (uint ch = 0; ch < NUM_DMA_CHANNELS; ++ch) {
        if (dma_irqn_get_channel_status(0, ch)) {
            dma_irqn_acknowledge_channel(0, ch);
            if (s_dispatch[ch])
                s_dispatch[ch]->signal_from_isr();
        }
    }
}

// ---------------------------------------------------------------------------
// Test hook — global linkage, matches declaration in hardware/dma.h stub.
// Compiled only when CORO_PICO_TEST is defined by the test CMakeLists.
// ---------------------------------------------------------------------------
#ifdef CORO_PICO_TEST
void coro_pico_hal_dma_fire_irq0() {
    dma_irq0_handler();
}
#endif

// ---------------------------------------------------------------------------
// AsyncDmaTransfer
// ---------------------------------------------------------------------------

namespace coro::pico::hal {

AsyncDmaTransfer::AsyncDmaTransfer() {
    int ch = dma_claim_unused_channel(true);
    if (ch < 0)
        throw std::runtime_error("AsyncDmaTransfer: no free DMA channels");
    m_channel = ch;

    std::call_once(s_irq_registered, []() {
        irq_add_shared_handler(DMA_IRQ_0, dma_irq0_handler,
                               PICO_SHARED_IRQ_HANDLER_DEFAULT_ORDER_PRIORITY);
        irq_set_enabled(DMA_IRQ_0, true);
    });

    // Allow this channel's completion to assert DMA_IRQ_0.
    // Without this the global irq_set_enabled(DMA_IRQ_0) has no effect for
    // this channel: the NVIC line is armed but the channel never drives it.
    dma_channel_set_irq0_enabled(static_cast<uint>(m_channel), true);
}

AsyncDmaTransfer::~AsyncDmaTransfer() {
    // Clear dispatch table entry before aborting so a late-firing IRQ does not
    // write into a destroyed IsrEvent.
    s_dispatch[m_channel] = nullptr;
    dma_channel_set_irq0_enabled(static_cast<uint>(m_channel), false);
    dma_channel_abort(static_cast<uint>(m_channel));
    dma_channel_unclaim(static_cast<uint>(m_channel));
}

Coro<void> AsyncDmaTransfer::transfer(const dma_channel_config& ctrl,
                                       const volatile void*       read_addr,
                                       volatile void*             write_addr,
                                       uint                       transfer_count) {
    // Register before starting — the IRQ could fire immediately after start.
    s_dispatch[m_channel] = &m_done;

    dma_channel_configure(static_cast<uint>(m_channel), &ctrl,
                          write_addr, read_addr, transfer_count, /*trigger=*/false);
    dma_channel_start(static_cast<uint>(m_channel));

    // AbortGuard: if this coroutine is cancelled while suspended in wait(),
    // the guard destructor clears the dispatch entry and aborts the channel.
    struct AbortGuard {
        int         channel;
        bool        active = true;
        ~AbortGuard() {
            if (active) {
                s_dispatch[channel] = nullptr;
                dma_channel_abort(static_cast<uint>(channel));
            }
        }
    } guard{m_channel};

    co_await m_done.wait();

    guard.active = false;
    s_dispatch[m_channel] = nullptr;
}

} // namespace coro::pico::hal

#endif // CORO_PICO
