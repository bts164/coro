# cmake/platforms/pico_hal.cmake
#
# Defines coro_pico_hal — optional async wrappers for RP2040 hardware peripherals
# (DMA, SPI, I2C, ADC, PIO). Must be included AFTER pico.cmake so that coro_pico exists.
#
# Usage:
#   include(${CORO_ROOT}/cmake/platforms/pico.cmake)
#   include(${CORO_ROOT}/cmake/platforms/pico_hal.cmake)
#   target_link_libraries(my_firmware PRIVATE coro_pico coro_pico_hal)
#
# Blanket-links the Pico SDK peripheral modules any current or future
# coro_pico_hal wrapper might need (hardware_dma/hardware_irq for the
# existing DMA wrapper, hardware_pio for PIO-based peripherals like WS2812
# drivers) — not gated per-module yet. Revisit with finer-grained Conan
# options if/when the unconditional link set becomes a real size/dependency
# concern for a consumer that doesn't need all of it.

include_guard(GLOBAL)

if(NOT DEFINED CORO_ROOT)
    set(CORO_ROOT ${CMAKE_CURRENT_LIST_DIR}/../..)
endif()

add_library(coro_pico_hal STATIC
    ${CORO_ROOT}/src/pico/hal/dma.cpp
)
target_link_libraries(coro_pico_hal PUBLIC coro_pico hardware_dma hardware_irq hardware_pio)
