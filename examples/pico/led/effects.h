#pragma once
#include "pixel_buffer.h"
#include "led_driver.h"
#include <coro/coro_stream.h>
#include <memory>
#include "ws2812.pb.h"

namespace led {

// All effects loop forever, yielding one PixelBuffer (GRB words) per frame
// at ~50 fps.  The shared_ptr<LedDriver> is captured by value in the
// coroutine frame so the driver stays alive for the duration of the effect.
//
// Effects read driver->led_count() at the top of each frame, so
// set_led_count() takes effect on the very next frame without restarting.
//
// To change parameters, call EffectRunner::set_effect() with a new stream.

coro::CoroStream<PixelBuffer> fade_effect(std::shared_ptr<LedDriver> driver, FadeEffect params);
coro::CoroStream<PixelBuffer> marquee_effect(std::shared_ptr<LedDriver> driver, MarqueeEffect params);
coro::CoroStream<PixelBuffer> comet_effect(std::shared_ptr<LedDriver> driver, CometEffect params);
coro::CoroStream<PixelBuffer> stars_effect(std::shared_ptr<LedDriver> driver, StarsEffect params);
coro::CoroStream<PixelBuffer> bounce_effect(std::shared_ptr<LedDriver> driver, BounceEffect params);
coro::CoroStream<PixelBuffer> particles_effect(std::shared_ptr<LedDriver> driver, ParticlesEffect params);
coro::CoroStream<PixelBuffer> waves_effect(std::shared_ptr<LedDriver> driver, WavesEffect params);
coro::CoroStream<PixelBuffer> spring_effect(std::shared_ptr<LedDriver> driver, SpringEffect params);
coro::CoroStream<PixelBuffer> plasma_effect(std::shared_ptr<LedDriver> driver, PlasmaEffect params);

} // namespace led
