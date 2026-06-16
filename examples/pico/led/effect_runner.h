#pragma once
#include "pixel_buffer.h"
#include "led_driver.h"
#include <coro/coro.h>
#include <coro/coro_stream.h>
#include <coro/task/join_handle.h>
#include <chrono>
#include <memory>

namespace led {

// Owns the currently-running effect and routes its frames to LedDriver.
//
// Switching effects is a single JoinHandle replacement: the old handle's
// destructor cancels the previous effect coroutine, and the new one is
// immediately spawned.  The shared_ptr<LedDriver> keeps the driver alive
// for as long as any effect coroutine is running.
//
// The frame_period controls how long run_effect() waits between frames using
// a drift-compensating IntervalTimer (default 20 ms = 50 fps).  Effects
// themselves are pure generators: they compute a frame and co_yield it with
// no sleep, letting the runner own the timing policy.
class EffectRunner {
public:
    explicit EffectRunner(
            std::shared_ptr<LedDriver> driver,
            std::chrono::nanoseconds frame_period = std::chrono::milliseconds(20)) noexcept
        : m_driver(std::move(driver))
        , m_frame_period(frame_period) {}

    // Cancel the running effect and wait for it to fully drain.
    // Must be co_await-ed; ensures any in-flight DMA transfer completes
    // before the caller proceeds to use the driver directly.
    [[nodiscard]] coro::Coro<void> stop();

    // Replace the running effect.  Awaits drain of the previous effect so that
    // its DMA transfer has completed before the new effect calls show().
    [[nodiscard]] coro::Coro<void> set_effect(coro::CoroStream<PixelBuffer> stream);

    bool running() const noexcept { return m_handle.valid(); }

private:
    [[nodiscard]] coro::Coro<void> run_effect(coro::CoroStream<PixelBuffer> stream);

    std::shared_ptr<LedDriver>  m_driver;
    std::chrono::nanoseconds    m_frame_period;
    coro::JoinHandle<void>      m_handle;
};

} // namespace led
