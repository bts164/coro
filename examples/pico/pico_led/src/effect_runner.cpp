#include <coro/stream.h>
#include <coro/runtime/runtime.h>
#include <coro/sync/interval.h>

#include "pico_led/effect_runner.h"

namespace led {

coro::Coro<void> EffectRunner::stop() {
    if (m_handle.valid())
        co_await std::move(m_handle).cancel_and_join();
}

coro::Coro<void> EffectRunner::set_effect(coro::CoroStream<PixelBuffer> stream) {
    if (m_handle.valid())
        co_await std::move(m_handle).cancel_and_join();
    m_handle = coro::spawn(run_effect(std::move(stream)));
}

coro::Coro<void> EffectRunner::run_effect(coro::CoroStream<PixelBuffer> stream) {
    coro::IntervalTimer timer(m_frame_period);
    while (auto frame = co_await coro::next(stream)) {
        co_await m_driver->show(*frame);
        co_await timer.tick();
    }
}

} // namespace led
