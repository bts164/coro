#ifdef CORO_PICO

#include <coro/pico/hal/dma.h>
#include <hardware/dma.h>
#include <hardware/irq.h>
#include <hardware/sync.h>
#include <stdexcept>
#include <mutex>
#include <utility>

// ---------------------------------------------------------------------------
// Module-internal dispatch table and IRQ handler at file scope.
//
// Keeping these outside any namespace lets the test hook (global linkage) call
// the handler directly without namespace tricks.
//
// s_dispatch is guarded by a dedicated striped spin lock -- see the matching
// note in gpio.cpp's dispatch table, which has the identical shape (a plain
// pointer array written from non-ISR context and read from ISR context, with
// nothing restricting that non-ISR context to a single core). An unguarded
// plain pointer read/write across cores is a genuine data race under the
// C++ memory model, not a theoretical one.
// ---------------------------------------------------------------------------

static coro::IsrEvent* s_dispatch[NUM_DMA_CHANNELS] = {};
static std::once_flag  s_irq_registered;

// Function-local static rather than a dynamically-initialized file-scope
// global -- see gpio.cpp's identical dispatch_lock() for why.
static spin_lock_t* dispatch_lock() {
    static spin_lock_t* lock = spin_lock_instance(next_striped_spin_lock_num());
    return lock;
}

static void dma_irq0_handler() {
    for (uint ch = 0; ch < NUM_DMA_CHANNELS; ++ch) {
        if (dma_irqn_get_channel_status(0, ch)) {
            dma_irqn_acknowledge_channel(0, ch);
            uint32_t save = spin_lock_blocking(dispatch_lock());
            coro::IsrEvent* target = s_dispatch[ch];
            spin_unlock(dispatch_lock(), save);
            // NOTE(race): target is read out from under the lock, then
            // called without it -- signal_from_isr() acquires IsrEvent's own
            // striped lock, and holding dispatch_lock() across that call
            // risks a same-core self-deadlock if the two alias the same
            // underlying hardware lock (the striped pool forbids nesting).
            // This leaves a narrow use-after-free window if the owning
            // AsyncDmaTransfer is destructed on another core between the
            // unlock and the call. See gpio.cpp's identical NOTE(race) on
            // gpio_irq_handler() and doc/design/gpio_pin.md.
            if (target)
                target->signal_from_isr();
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

AsyncDmaTransfer::AsyncDmaTransfer(bool track_completion)
    : m_track_completion(track_completion) {
    int ch = dma_claim_unused_channel(true);
    if (ch < 0)
        throw std::runtime_error("AsyncDmaTransfer: no free DMA channels");
    m_channel = ch;

    if (!m_track_completion)
        return;

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
    if (m_track_completion) {
        // Clear dispatch table entry before aborting so a late-firing IRQ does
        // not write into a destroyed IsrEvent.
        uint32_t save = spin_lock_blocking(dispatch_lock());
        s_dispatch[m_channel] = nullptr;
        spin_unlock(dispatch_lock(), save);
        dma_channel_set_irq0_enabled(static_cast<uint>(m_channel), false);
    }
    dma_channel_abort(static_cast<uint>(m_channel));
    dma_channel_unclaim(static_cast<uint>(m_channel));
}

void AsyncDmaTransfer::start(const dma_channel_config& ctrl,
                              const volatile void*       read_addr,
                              volatile void*             write_addr,
                              uint                       transfer_count) {
    // Register before starting — the IRQ could fire immediately after start.
    if (m_track_completion) {
        // Snapshot the epoch here too, before the dispatch entry goes live
        // and the transfer is triggered -- not inside wait(), which may be
        // called an arbitrary amount of time later (that's the whole point
        // of the start()/wait() split). If wait() captured its own baseline
        // instead, a transfer that completes between start() and wait()
        // would bump the epoch before wait() ever reads it, and wait() would
        // hang forever waiting for a second completion that never comes.
        // Same bug class as arm_and_wait()'s fix in gpio.cpp -- see
        // IsrEvent::wait_from()'s doc comment.
        m_wait_baseline = m_done.epoch();
        uint32_t save = spin_lock_blocking(dispatch_lock());
        s_dispatch[m_channel] = &m_done;
        spin_unlock(dispatch_lock(), save);
    }

    dma_channel_configure(static_cast<uint>(m_channel), &ctrl,
                          write_addr, read_addr, transfer_count, /*trigger=*/false);
    dma_channel_start(static_cast<uint>(m_channel));
}

DmaWaitFuture AsyncDmaTransfer::wait() {
    return DmaWaitFuture(*this);
}

DmaWaitFuture AsyncDmaTransfer::transfer(const dma_channel_config& ctrl,
                                         const volatile void*       read_addr,
                                         volatile void*             write_addr,
                                         uint                       transfer_count) {
    start(ctrl, read_addr, write_addr, transfer_count);
    return wait();
}

// ---------------------------------------------------------------------------
// DmaWaitFuture
// ---------------------------------------------------------------------------

// Takes the channel out of the dispatch table, so a late-firing IRQ signals nothing.
static void clear_dispatch(int channel) {
    uint32_t save = spin_lock_blocking(dispatch_lock());
    s_dispatch[channel] = nullptr;
    spin_unlock(dispatch_lock(), save);
}

// Resolves against the baseline start() captured, not one taken here: the transfer
// may already have completed by the time wait() is called.
DmaWaitFuture::DmaWaitFuture(AsyncDmaTransfer& dma)
    : m_dma(&dma), m_wait(dma.m_done.wait_from(dma.m_wait_baseline)) {}

DmaWaitFuture::DmaWaitFuture(DmaWaitFuture&& other) noexcept
    : m_dma(std::exchange(other.m_dma, nullptr)), m_wait(std::move(other.m_wait)) {}

DmaWaitFuture::~DmaWaitFuture() {
    if (m_dma == nullptr)
        return;
    // Destroyed before completion (the awaiting coroutine was cancelled, or the
    // future was never awaited): stop the transfer.
    // RACE CONDITION NOTE: the transfer may complete, and its IRQ fire, at any point
    // here. Before clear_dispatch() that signals m_done, which nothing waits for any
    // more; after it the handler finds no entry. Either way dma_channel_abort() on a
    // finished channel is harmless.
    clear_dispatch(m_dma->m_channel);
    dma_channel_abort(static_cast<uint>(m_dma->m_channel));
}

PollResult<void> DmaWaitFuture::poll(detail::Context& ctx) {
    auto result = m_wait.poll(ctx);
    if (result.isReady()) {
        clear_dispatch(m_dma->m_channel);
        m_dma = nullptr;
    }
    return result;
}

} // namespace coro::pico::hal

#endif // CORO_PICO
