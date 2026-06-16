cmake_minimum_required(VERSION 3.20)

# Generate the ws2812.pio.h header from ws2812.pio.
# pico_generate_pio_header must be called on the target that uses it.
add_library(coro_pico_led STATIC
    color.cpp
    led_driver.cpp
    effect_runner.cpp
    effects.cpp
)

pico_generate_pio_header(coro_pico_led ${CMAKE_CURRENT_LIST_DIR}/ws2812.pio)

target_include_directories(coro_pico_led PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}/..   # so #include "led/..." works from the pico/ root
)

target_link_libraries(coro_pico_led PUBLIC
    coro_pico
    coro_pico_hal         # provides AsyncDmaTransfer (uses IsrEvent, ISR-safe)
    ws2812_proto          # provides ws2812.pb.h for effects.h / effects.cpp
    hardware_pio
    hardware_clocks
    pico_stdlib
)
