#pragma once
#include <cstdint>

// Stub for Pico SDK <hardware/gpio.h> used in Linux unit-test builds.
// Only the symbols used by GpioPin are stubbed. Real RP2040 pins are not
// involved; the stub tracks direction/pull/level state and exposes test
// helpers to simulate GPIO IRQs.

using uint = unsigned int;

static constexpr uint32_t GPIO_IRQ_LEVEL_LOW  = 0x1u;
static constexpr uint32_t GPIO_IRQ_LEVEL_HIGH = 0x2u;
static constexpr uint32_t GPIO_IRQ_EDGE_FALL  = 0x4u;
static constexpr uint32_t GPIO_IRQ_EDGE_RISE  = 0x8u;

using gpio_irq_callback_t = void (*)(uint gpio, uint32_t event_mask);

// ---------------------------------------------------------------------------
// Configuration -- state changes tracked by the stub, no real hardware
// ---------------------------------------------------------------------------
void gpio_init(uint gpio);
void gpio_set_dir(uint gpio, bool out);
void gpio_pull_up(uint gpio);
void gpio_pull_down(uint gpio);
void gpio_disable_pulls(uint gpio);

// ---------------------------------------------------------------------------
// Level read/write -- driven by the stub's tracked level, settable by tests
// via gpio_stub::set_level()
// ---------------------------------------------------------------------------
bool gpio_get(uint gpio);
void gpio_put(uint gpio, bool value);

// ---------------------------------------------------------------------------
// IRQ configuration -- no real IRQ; the test suite drives the handler
// directly via coro_pico_hal_gpio_fire_irq()
// ---------------------------------------------------------------------------
void gpio_set_irq_enabled(uint gpio, uint32_t event_mask, bool enabled);
void gpio_set_irq_enabled_with_callback(uint gpio, uint32_t event_mask, bool enabled,
                                        gpio_irq_callback_t callback);

// ---------------------------------------------------------------------------
// Test helpers
// ---------------------------------------------------------------------------
namespace gpio_stub {

void set_level(uint gpio, bool level);
void reset();

// Current set of event bits gpio_set_irq_enabled() has left enabled for
// `gpio` (tracked by OR-ing/AND-NOTing each call's mask into stub state,
// matching the real INTE register's semantics). Lets tests assert on
// GpioPin's refcounted arm/disarm without touching real hardware.
uint32_t enabled_mask(uint gpio);

// Number of times gpio_set_irq_enabled() has been called for `gpio` since
// the last reset(), regardless of mask or enabled/disabled. Lets tests
// assert a bit was armed/disarmed exactly once even when enabled_mask()
// alone can't distinguish "never called" from "called and left unchanged".
int irq_enable_call_count(uint gpio);

} // namespace gpio_stub

// Invokes the module-internal gpio_irq_handler(gpio, event_mask) directly --
// simulates the GPIO IRQ firing for a given pin/event mask. Set the stub's
// tracked level first via gpio_stub::set_level() if the test cares what
// read()/wait_for_level() observe. Defined in src/pico/hal/gpio.cpp, only
// when CORO_PICO_TEST is defined by the test CMakeLists.
void coro_pico_hal_gpio_fire_irq(uint gpio, uint32_t event_mask);
