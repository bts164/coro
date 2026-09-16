#pragma once
// RP2040-specific async GPIO edge/level primitive. Part of the optional
// coro_pico_hal cmake target — only available when that target is linked.
// See doc/design/gpio_pin.md for the full design rationale.

#ifdef CORO_PICO

#include <hardware/gpio.h>
#include <optional>
#include <stdexcept>
#include <coro/coro.h>
#include <coro/coro_stream.h>
#include <coro/sync/isr_event.h>

namespace coro::pico::hal {

enum class Direction { In, Out };
enum class Pull { None, Up, Down };
enum class Edge { Rising, Falling, Any };

class GpioPin;

/**
 * @brief Future returned by `GpioPin::wait_for_edge()`/`wait_for_level()`/
 * `wait_for_high()`/`wait_for_low()`.
 *
 * Arms the IRQ (or, for `wait_for_level()` when already satisfied, resolves
 * to an already-ready state) synchronously in its constructor -- i.e. before
 * the `wait_for_*()` call that created it returns, not on first `co_await`.
 * This lets a caller store the future and perform other synchronous work
 * (e.g. an SPI strobe) before awaiting it, with the IRQ guaranteed armed the
 * whole time. See doc/design/gpio_pin.md's "Eager arming" section.
 *
 * **Non-owning**: holds a pointer back into the `GpioPin` it was created
 * from and does not extend its lifetime. Must not outlive that `GpioPin` --
 * see the WARNING in doc/design/gpio_pin.md. The intended usage is always a
 * local stack variable in the same scope as the `GpioPin`.
 *
 * Move-only. Destroying a `GpioEdgeFuture` before it resolves releases its
 * share of whichever IRQ bit(s) it armed (refcounted -- see
 * doc/design/gpio_pin.md's "Refcounted disarm" section); it does not affect
 * any other concurrently in-flight `GpioEdgeFuture` on the same pin.
 */
class GpioEdgeFuture {
public:
    using OutputType = void;

    ~GpioEdgeFuture();

    GpioEdgeFuture(GpioEdgeFuture&& other) noexcept;
    GpioEdgeFuture& operator=(GpioEdgeFuture&& other) noexcept;
    GpioEdgeFuture(const GpioEdgeFuture&)            = delete;
    GpioEdgeFuture& operator=(const GpioEdgeFuture&) = delete;

    PollResult<void> poll(detail::Context& ctx);

private:
    friend class GpioPin;

    // Already-satisfied fast path (wait_for_level() when the level already
    // matches): no IRQ bit is armed, nothing to release on destruction.
    GpioEdgeFuture();

    // Armed path: increments GpioPin's per-bit refcount for every bit set in
    // `mask` (enabling any bit whose count transitions 0 -> 1), snapshots
    // the IsrEvent epoch baseline, and builds the underlying IsrWaitFuture.
    GpioEdgeFuture(GpioPin& pin, uint32_t mask);

    GpioPin*  m_pin  = nullptr; // non-owning; null in the already-ready fast path
    uint32_t  m_mask = 0;       // bit(s) this instance's refcount holds; 0 == nothing to release
    bool      m_ready;
    // Absent (never constructed) in the already-ready fast path -- there's
    // nothing to poll, so no IsrWaitFuture (and no executor registration) is
    // ever created for that case.
    std::optional<IsrWaitFuture> m_wait;
};

static_assert(Future<GpioEdgeFuture>);

/**
 * @brief RAII async GPIO pin — edge/level waits and a lossless edge stream.
 *
 * Claims one pin on construction (configures direction/pull), releases it
 * (disables its IRQ) on destruction. Any number of `GpioPin` instances can
 * coexist, each independently owning its own pin — same shape as
 * `AsyncDmaTransfer`'s per-channel ownership, just keyed by GPIO number
 * instead of DMA channel.
 *
 * `wait_for_edge()`/`wait_for_level()` are broadcast to every concurrent
 * waiter on the same instance (backed by `IsrEvent`) — see
 * doc/design/isr_safety.md's "Multiple waiters". `edges()` is instead backed
 * by `IsrSemaphore`, so each concurrent `co_await` on the stream claims one
 * drained edge rather than all of them seeing the same one.
 *
 * Usage:
 * @code
 * coro::pico::hal::GpioPin busy_pin(EPD_BUSY_PIN, Direction::In, Pull::Up);
 * co_await busy_pin.wait_for_low();   // parks until the display is idle
 * @endcode
 */
class GpioPin {
public:
    // Claims `pin`, configures its direction/pull, and installs the shared
    // gpio_irq_handler once (on first construction of any GpioPin).
    //
    // Throws std::logic_error if another live GpioPin already claims `pin`.
    // Note this only guards against two GpioPins on the same pin -- it
    // cannot detect (and does not attempt to detect) the pin being driven
    // by an unrelated peripheral (SPI, PWM, ADC, ...) configured directly
    // via the SDK; the Pico SDK has no cross-peripheral pin-ownership
    // registry to check against. See doc/design/gpio_pin.md.
    GpioPin(uint pin, Direction dir, Pull pull = Pull::None);

    // Disables this pin's IRQ and clears its dispatch table slot.
    ~GpioPin();

    GpioPin(const GpioPin&)            = delete;
    GpioPin& operator=(const GpioPin&) = delete;
    GpioPin(GpioPin&&)                 = delete;
    GpioPin& operator=(GpioPin&&)      = delete;

    // Arms the IRQ synchronously (before this call returns, not on first
    // co_await) and returns a future that resolves once the pin transitions
    // to `edge`'s polarity -- a transition that must still occur after this
    // call, regardless of the pin's level right now. Edge::Any resolves on
    // either transition. See doc/design/gpio_pin.md's "Eager arming".
    [[nodiscard]] GpioEdgeFuture wait_for_edge(Edge edge);

    // Returns an already-ready future immediately (no ISR involved) if the
    // pin is already at `level`. Otherwise arms the IRQ synchronously and
    // behaves like wait_for_edge() for the corresponding polarity. See
    // doc/design/gpio_pin.md's WARNING on why the suspended path must
    // resolve on the latched signal, never a re-sampled level.
    [[nodiscard]] GpioEdgeFuture wait_for_level(bool level);
    [[nodiscard]] GpioEdgeFuture wait_for_high() { return wait_for_level(true); }
    [[nodiscard]] GpioEdgeFuture wait_for_low()  { return wait_for_level(false); }

    // Async generator of every matching edge, in order, including ones that
    // occur while nobody is co_await-ing the stream -- unlike looping on
    // wait_for_edge(), which only ever sees the next edge and drops any that
    // occur in between. Edge::Any is included.
    [[nodiscard]] CoroStream<Edge> edges(Edge edge);

    // Direct level read, no async involved.
    [[nodiscard]] bool read() const;

    // Direct level write. No-op (and likely meaningless) unless this pin was
    // constructed with Direction::Out.
    void write(bool level);

    [[nodiscard]] uint pin() const { return m_pin; }

    // Called by the module-internal shared gpio_irq_handler (src/pico/hal/gpio.cpp)
    // when this pin's IRQ fires. Not intended to be called directly.
    void notify_irq(uint32_t event_mask);

private:
    friend class GpioEdgeFuture;

    // Shared by wait_for_level()'s fallback path and wait_for_edge(): builds
    // an armed GpioEdgeFuture for `event_mask` on this pin. See
    // doc/design/gpio_pin.md's "shared internal helper" note.
    [[nodiscard]] GpioEdgeFuture arm_and_wait(uint32_t event_mask);

    // Refcounted arm/disarm for GpioEdgeFuture -- see doc/design/gpio_pin.md's
    // "Refcounted disarm" section. `mask` may set more than one bit (e.g.
    // Edge::Any's RISE|FALL); each set bit's count is adjusted independently.
    // acquire_mask() calls gpio_set_irq_enabled(..., true) for any bit whose
    // count transitions 0 -> 1; release_mask() calls it with false for any
    // bit whose count drops back to 0. Both guarded by m_mask_lock.
    void acquire_mask(uint32_t mask);
    void release_mask(uint32_t mask);

    uint         m_pin;
    IsrEvent     m_edge_event;
    IsrSemaphore m_edge_count;

    // edges()'s direction-reconstruction bookkeeping -- see
    // doc/design/gpio_pin.md: direction of each drained edge is recovered
    // from the pending count's parity plus the level bit remembered as of
    // the most recent edge, rather than storing a payload per edge in
    // m_edge_count (which stays a bare count, per IsrSemaphore's contract).
    // Guarded by m_dir_lock rather than m_edge_count's internal lock, which
    // GpioPin has no access to -- see isr_event.h's IsrSemaphore.
    spin_lock_t* m_dir_lock;
    bool         m_last_edge_to_high    = false;
    uint32_t     m_pending_edge_count   = 0;

    // Per-bit refcount for GpioEdgeFuture's eager arm / refcounted disarm --
    // indices 0..3 correspond to GPIO_IRQ_LEVEL_LOW, GPIO_IRQ_LEVEL_HIGH,
    // GPIO_IRQ_EDGE_FALL, GPIO_IRQ_EDGE_RISE respectively (see
    // mask_bit_index() in gpio.cpp). Guarded by m_mask_lock, a separate
    // striped spin lock from m_dir_lock since the two protect independent
    // state and are adjusted from unrelated call paths.
    spin_lock_t* m_mask_lock;
    uint8_t      m_mask_refcount[4] = {};
};

} // namespace coro::pico::hal

#endif // CORO_PICO
