# cmake/platforms/pico_hal.cmake
#
# Defines coro_pico_hal — optional async wrappers for RP2040 hardware peripherals
# (DMA, SPI, I2C, ADC). Must be included AFTER pico.cmake so that coro_pico exists.
#
# Usage:
#   include(${CORO_ROOT}/cmake/platforms/pico.cmake)
#   include(${CORO_ROOT}/cmake/platforms/pico_hal.cmake)
#   target_link_libraries(my_firmware PRIVATE coro_pico coro_pico_hal hardware_dma)

include_guard(GLOBAL)

if(NOT DEFINED CORO_ROOT)
    set(CORO_ROOT ${CMAKE_CURRENT_LIST_DIR}/../..)
endif()

add_library(coro_pico_hal STATIC
    ${CORO_ROOT}/src/pico/hal/dma.cpp
)
target_link_libraries(coro_pico_hal PUBLIC coro_pico hardware_dma hardware_irq)
