#pragma once
#include <cstdint>

// Stub for Pico SDK <hardware/irq.h> used in Linux unit-test builds.
// IRQ registration is a no-op — the test suite drives IRQ handlers directly
// via coro_pico_hal_dma_fire_irq0().

using uint = unsigned int;   // normally provided by <pico/types.h>

static constexpr uint8_t PICO_SHARED_IRQ_HANDLER_DEFAULT_ORDER_PRIORITY = 0x80;

using irq_handler_t = void(*)();

inline void irq_add_shared_handler(uint, irq_handler_t, uint8_t) {}
inline void irq_set_enabled(uint, bool) {}
