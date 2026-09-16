#ifdef CORO_PICO

#include <coro/pico/hal/gpio.h>
#include <hardware/gpio.h>
#include <mutex>
#include <stdexcept>
#include <utility>

// ---------------------------------------------------------------------------
// Module-internal dispatch table and shared IRQ callback at file scope.
//
// Same pattern as src/pico/hal/dma.cpp's per-channel dispatch table, keyed by
// GPIO number instead of DMA channel. Unlike DMA_IRQ_0 (one shared line,
// polled per-channel), the Pico SDK's gpio_irq_callback_t already receives
// the firing pin number directly, so no per-pin status scan is needed here.
//
// Unlike dma.cpp's dispatch table (which assumes single-core registration
// with no lock), s_dispatch here is guarded by a real spin lock:
// GpioPin construction/destruction is not documented anywhere as core-0-only,
// so nothing stops a caller from constructing/destructing GpioPins on core 1
// while the GPIO IRQ (registered once, on whichever core constructs first)
// fires on core 0 and reads s_dispatch concurrently -- an unguarded plain
// pointer array read/write across cores is a genuine data race under the
// C++ memory model, not just a theoretical one. spin_lock_blocking() also
// disables IRQs on the calling core while held, so this is equally safe
// against same-core ISR reentrancy during construction/destruction. Kept
// deliberately brief (array element read/write only) per the spin lock
// contract noted in hardware/sync.h.
// ---------------------------------------------------------------------------

static constexpr uint kNumGpios = 30;

static coro::pico::hal::GpioPin* s_dispatch[kNumGpios] = {};
static std::once_flag            s_irq_registered;

// Function-local static rather than a dynamically-initialized file-scope
// global -- avoids relying on this TU's static-initialization order (the
// real spin_lock_instance()/next_striped_spin_lock_num() have no such
// dependency today, but there's no need to take that on).
static spin_lock_t* dispatch_lock() {
    static spin_lock_t* lock = spin_lock_instance(next_striped_spin_lock_num());
    return lock;
}

static void gpio_irq_handler(uint gpio, uint32_t event_mask) {
    if (gpio >= kNumGpios)
        return;
    uint32_t save = spin_lock_blocking(dispatch_lock());
    coro::pico::hal::GpioPin* target = s_dispatch[gpio];
    spin_unlock(dispatch_lock(), save);
    // NOTE(race): target is read out from under the lock, then called
    // without it -- necessary because notify_irq() (and everything it calls)
    // acquires other striped spin locks, and holding dispatch_lock() across
    // that call would nest two striped locks on one core, which the striped
    // pool's contract explicitly forbids (a collision on the same
    // underlying hardware lock number would self-deadlock). This leaves a
    // narrow window: if `target` is destructed on another core between the
    // unlock above and the call below, this is a use-after-free. Closing it
    // fully would need a dedicated (non-striped) lock held for the whole
    // call, or reference-counting GpioPin -- not done here; see
    // doc/design/gpio_pin.md.
    if (target)
        target->notify_irq(event_mask);
}

// ---------------------------------------------------------------------------
// Test hook — global linkage, matches declaration in hardware/gpio.h stub.
// Compiled only when CORO_PICO_TEST is defined by the test CMakeLists.
// ---------------------------------------------------------------------------
#ifdef CORO_PICO_TEST
void coro_pico_hal_gpio_fire_irq(uint gpio, uint32_t event_mask) {
    gpio_irq_handler(gpio, event_mask);
}
#endif

// ---------------------------------------------------------------------------
// GpioPin
// ---------------------------------------------------------------------------

namespace coro::pico::hal {

namespace {

// Maps an Edge value to the GPIO_IRQ_EDGE_* bit(s) that detect it. Shared by
// wait_for_edge() (via arm_and_wait()) and edges().
constexpr uint32_t edge_irq_mask(Edge edge) {
    switch (edge) {
        case Edge::Rising:  return GPIO_IRQ_EDGE_RISE;
        case Edge::Falling: return GPIO_IRQ_EDGE_FALL;
        case Edge::Any:     return GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL;
    }
    return 0;
}

// Maps each of the four independent GPIO IRQ trigger bits to an index into
// GpioPin::m_mask_refcount. -1 for anything else (never passed here -- every
// mask GpioEdgeFuture arms is built from these four bits only).
constexpr int mask_bit_index(uint32_t bit) {
    switch (bit) {
        case GPIO_IRQ_LEVEL_LOW:  return 0;
        case GPIO_IRQ_LEVEL_HIGH: return 1;
        case GPIO_IRQ_EDGE_FALL:  return 2;
        case GPIO_IRQ_EDGE_RISE:  return 3;
    }
    return -1;
}

} // namespace

GpioPin::GpioPin(uint pin, Direction dir, Pull pull)
    : m_pin(pin),
      m_dir_lock(spin_lock_instance(next_striped_spin_lock_num())),
      m_mask_lock(spin_lock_instance(next_striped_spin_lock_num())) {
    // Reject a double-claim before touching any hardware state -- otherwise
    // two GpioPins on the same pin silently clobber s_dispatch[pin], leaving
    // the first instance's IRQs dispatched nowhere and both destructors
    // fighting over the same slot. See doc/design/gpio_pin.md.
    //
    // NOTE(race): this check-then-later-set is not one atomic operation --
    // two threads racing to construct on the same pin could both pass this
    // check before either has registered in s_dispatch. Not closed here: an
    // airtight fix needs a reservation step under dispatch_lock() before any
    // hardware configuration runs, which is more machinery than a genuine
    // (as opposed to buggy-caller) concurrent double-construction on one pin
    // number seems to warrant.
    if (pin < kNumGpios) {
        uint32_t save = spin_lock_blocking(dispatch_lock());
        bool claimed = s_dispatch[pin] != nullptr;
        spin_unlock(dispatch_lock(), save);
        if (claimed)
            throw std::logic_error("GpioPin: pin already claimed by another GpioPin instance");
    }

    gpio_init(pin);
    gpio_set_dir(pin, dir == Direction::Out);
    switch (pull) {
        case Pull::Up:   gpio_pull_up(pin);      break;
        case Pull::Down: gpio_pull_down(pin);    break;
        case Pull::None: gpio_disable_pulls(pin); break;
    }

    // enabled=true here, even though event_mask is 0 (no pin events are
    // actually armed by this call -- individual pins arm their own masks
    // later via plain gpio_set_irq_enabled(), see arm_and_wait()). This is
    // the only call in this file that ever runs the SDK's
    // irq_set_enabled(IO_IRQ_BANK0, true) (buried inside
    // gpio_set_irq_enabled_with_callback(), gated on its own `enabled`
    // argument) -- the top-level NVIC enable for GPIO IRQs on this core.
    // Passing enabled=false here (as this used to) registers the shared
    // callback but leaves IO_IRQ_BANK0 permanently masked at the NVIC, so
    // no GPIO edge on any pin -- no matter how many pins later arm their
    // own event mask -- ever reaches the CPU. Confirmed via hardware test:
    // GDO0 physically pulsed correctly (verified with a raw busy-poll) but
    // gpio_irq_handler() never ran at all.
    std::call_once(s_irq_registered, [pin]() {
        gpio_set_irq_enabled_with_callback(pin, 0, true, gpio_irq_handler);
    });

    if (pin < kNumGpios) {
        uint32_t save = spin_lock_blocking(dispatch_lock());
        s_dispatch[pin] = this;
        spin_unlock(dispatch_lock(), save);
    }
}

GpioPin::~GpioPin() {
    // Clear the dispatch slot before disabling the IRQ, not after -- matches
    // dma.cpp's identical convention ("clear dispatch table entry before
    // aborting"). Any gpio_irq_handler() invocation that hasn't yet read
    // s_dispatch[m_pin] by this point will see nullptr and skip the call
    // entirely, rather than risking a call into an object whose destructor
    // has already started running. Still not airtight against another core
    // calling in mid-notify_irq(), per the NOTE(race) on gpio_irq_handler()
    // above.
    if (m_pin < kNumGpios) {
        uint32_t save = spin_lock_blocking(dispatch_lock());
        s_dispatch[m_pin] = nullptr;
        spin_unlock(dispatch_lock(), save);
    }
    // Unconditional, independent of m_mask_refcount -- a backstop, not the
    // normal disarm path (that's GpioEdgeFuture's destructor via
    // release_mask()). No live GpioEdgeFuture should exist by the time this
    // runs (see the non-owning-reference contract in doc/design/gpio_pin.md),
    // but this still clears every bit regardless, so a GpioPin's teardown
    // never depends on that contract having been honored.
    gpio_set_irq_enabled(m_pin, GPIO_IRQ_LEVEL_LOW | GPIO_IRQ_LEVEL_HIGH |
                                 GPIO_IRQ_EDGE_FALL | GPIO_IRQ_EDGE_RISE, false);
}

void GpioPin::notify_irq(uint32_t event_mask) {
    constexpr uint32_t kEdgeMask = GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL;

    // edges()/m_edge_count only cares about edge occurrences, not level
    // occurrences -- route those separately from the broadcast signal below,
    // and latch the direction/parity bookkeeping edges() needs to recover
    // each drained item's polarity without storing a payload per edge.
    if (event_mask & kEdgeMask) {
        bool to_high = (event_mask & GPIO_IRQ_EDGE_RISE) != 0;
        uint32_t save = spin_lock_blocking(m_dir_lock);
        m_last_edge_to_high = to_high;
        ++m_pending_edge_count;
        spin_unlock(m_dir_lock, save);
        m_edge_count.release_from_isr();
    }

    // wait_for_edge()/wait_for_level() waiters both resolve off this single
    // broadcast signal regardless of which specific event_mask bit fired.
    // NOTE(race): if two concurrent waiters on this same instance have
    // armed different masks (e.g. one wait_for_edge(Rising), another
    // wait_for_edge(Falling)), a Falling firing wakes both -- IsrEvent has
    // no per-mask routing, only a single epoch. This is consistent with the
    // "concurrent waiters legitimately want the same event" assumption in
    // doc/design/gpio_pin.md's Open Questions; it is not safe for waiters
    // with genuinely different interests on one instance.
    m_edge_event.signal_from_isr();
}

void GpioPin::acquire_mask(uint32_t mask) {
    uint32_t to_enable = 0;
    uint32_t save = spin_lock_blocking(m_mask_lock);
    for (uint32_t bit = mask; bit != 0; ) {
        uint32_t one = bit & (~bit + 1); // isolate lowest set bit
        bit &= ~one;
        int idx = mask_bit_index(one);
        if (idx < 0) continue;
        if (m_mask_refcount[idx]++ == 0)
            to_enable |= one;
    }
    spin_unlock(m_mask_lock, save);
    // gpio_set_irq_enabled() is two plain register stores (acknowledge +
    // atomic-alias set/clear on INTE) with no critical section -- safe and
    // cheap to call outside the lock above. See doc/design/gpio_pin.md's
    // "Refcounted disarm" note for the cost data this relies on.
    if (to_enable)
        gpio_set_irq_enabled(m_pin, to_enable, true);
}

void GpioPin::release_mask(uint32_t mask) {
    uint32_t to_disable = 0;
    uint32_t save = spin_lock_blocking(m_mask_lock);
    for (uint32_t bit = mask; bit != 0; ) {
        uint32_t one = bit & (~bit + 1);
        bit &= ~one;
        int idx = mask_bit_index(one);
        if (idx < 0) continue;
        if (--m_mask_refcount[idx] == 0)
            to_disable |= one;
    }
    spin_unlock(m_mask_lock, save);
    if (to_disable)
        gpio_set_irq_enabled(m_pin, to_disable, false);
}

GpioEdgeFuture GpioPin::arm_and_wait(uint32_t event_mask) {
    // GpioEdgeFuture's constructor snapshots the epoch *before* arming
    // (acquire_mask() below), then resolves against that exact baseline --
    // not against a baseline captured on first poll. Capturing it first
    // closes the arming race this primitive exists to avoid: any edge
    // landing between epoch-snapshot and gpio_set_irq_enabled() can only
    // ever bump the epoch to something strictly past the baseline, which
    // still reads as ready. See doc/design/gpio_pin.md's "Eager arming"
    // section.
    return GpioEdgeFuture(*this, event_mask);
}

GpioEdgeFuture GpioPin::wait_for_edge(Edge edge) {
    return arm_and_wait(edge_irq_mask(edge));
}

GpioEdgeFuture GpioPin::wait_for_level(bool level) {
    if (read() == level)
        return GpioEdgeFuture{};
    // See WARNING in doc/design/gpio_pin.md: from here on we resolve on
    // m_edge_event's latched signal only -- never re-sample read() again.
    uint32_t mask = level ? GPIO_IRQ_LEVEL_HIGH : GPIO_IRQ_LEVEL_LOW;
    return arm_and_wait(mask);
}

// ---------------------------------------------------------------------------
// GpioEdgeFuture
// ---------------------------------------------------------------------------

GpioEdgeFuture::GpioEdgeFuture() : m_ready(true) {}

GpioEdgeFuture::GpioEdgeFuture(GpioPin& pin, uint32_t mask)
    : m_pin(&pin), m_mask(mask), m_ready(false) {
    uint64_t baseline = m_pin->m_edge_event.epoch();
    m_pin->acquire_mask(m_mask);
    m_wait.emplace(m_pin->m_edge_event, baseline);
}

GpioEdgeFuture::~GpioEdgeFuture() {
    if (m_mask != 0 && m_pin != nullptr)
        m_pin->release_mask(m_mask);
}

GpioEdgeFuture::GpioEdgeFuture(GpioEdgeFuture&& other) noexcept
    : m_pin(other.m_pin), m_mask(std::exchange(other.m_mask, 0)),
      m_ready(other.m_ready), m_wait(std::move(other.m_wait)) {}

GpioEdgeFuture& GpioEdgeFuture::operator=(GpioEdgeFuture&& other) noexcept {
    if (this != &other) {
        if (m_mask != 0 && m_pin != nullptr)
            m_pin->release_mask(m_mask);
        m_pin   = other.m_pin;
        m_mask  = std::exchange(other.m_mask, 0);
        m_ready = other.m_ready;
        m_wait  = std::move(other.m_wait);
    }
    return *this;
}

PollResult<void> GpioEdgeFuture::poll(detail::Context& ctx) {
    if (m_ready)
        return PollReady;
    return m_wait->poll(ctx);
}

CoroStream<Edge> GpioPin::edges(Edge edge) {
    // Goes through the same refcounted acquire/release as GpioEdgeFuture
    // (see doc/design/gpio_pin.md's "Refcounted disarm") rather than a bare
    // gpio_set_irq_enabled(), so a concurrent wait_for_edge()/wait_for_any()
    // on the same bit can't have its GpioEdgeFuture's destructor disarm a
    // bit this stream still needs, or vice versa. MaskGuard's destructor
    // runs as part of this coroutine frame's teardown (stream cancellation
    // or destruction) -- same mechanism as any other local in a suspended
    // coroutine frame.
    uint32_t mask = edge_irq_mask(edge);
    acquire_mask(mask);
    struct MaskGuard {
        GpioPin* pin;
        uint32_t mask;
        ~MaskGuard() { pin->release_mask(mask); }
    } guard{this, mask};

    for (;;) {
        co_await m_edge_count.acquire();

        bool to_high;
        uint32_t remaining;
        {
            uint32_t save = spin_lock_blocking(m_dir_lock);
            to_high   = m_last_edge_to_high;
            remaining = --m_pending_edge_count;
            spin_unlock(m_dir_lock, save);
        }
        // remaining == edges still queued behind the one just claimed.
        // Directions alternate strictly, so parity of `remaining` relative
        // to the most recently recorded edge (`to_high`) recovers this
        // specific drained edge's direction -- see doc/design/gpio_pin.md.
        bool this_to_high = (remaining % 2 == 0) ? to_high : !to_high;
        co_yield this_to_high ? Edge::Rising : Edge::Falling;
    }
}

bool GpioPin::read() const {
    return gpio_get(m_pin);
}

void GpioPin::write(bool level) {
    gpio_put(m_pin, level);
}

} // namespace coro::pico::hal

#endif // CORO_PICO
