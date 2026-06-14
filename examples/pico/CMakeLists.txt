cmake_minimum_required(VERSION 3.13)

# Must come before project() — sets up the ARM toolchain and SDK variables.
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(coro_pico_examples VERSION 0.1.0 LANGUAGES CXX C ASM)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(PICO_CXX_ENABLE_EXCEPTIONS 1)
pico_sdk_init()

set(CORO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
include(${CORO_ROOT}/cmake/platforms/pico.cmake)
include(${CORO_ROOT}/cmake/platforms/pico_hal.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/PicoLED/PicoLed.cmake)

# lwipopts.h lives alongside this CMakeLists.txt — add it to coro_pico's
# compile path so lwIP finds it when building the library sources.
target_include_directories(coro_pico PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# ---------------------------------------------------------------------------
# WiFi credentials — copy wifi_credentials.h.example to wifi_credentials.h
# and fill in your SSID and password. That file is gitignored and never
# appears on the build command line or in compile_commands.json.
# ---------------------------------------------------------------------------
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/wifi_credentials.h")
    message(FATAL_ERROR
        "Missing wifi_credentials.h — copy wifi_credentials.h.example to "
        "wifi_credentials.h and fill in your network details.")
endif()

# ---------------------------------------------------------------------------
# pico_tcp_echo_server
# ---------------------------------------------------------------------------
add_executable(pico_tcp_echo_server pico_tcp_echo_server.cpp)
target_include_directories(pico_tcp_echo_server PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(pico_tcp_echo_server PRIVATE coro_pico pico_stdlib pico_cyw43_arch_lwip_poll)
pico_enable_stdio_usb(pico_tcp_echo_server 1)
pico_enable_stdio_uart(pico_tcp_echo_server 0)
pico_add_extra_outputs(pico_tcp_echo_server)   # generates .uf2 / .bin / .hex

# ---------------------------------------------------------------------------
# pico_tcp_echo_client
# ---------------------------------------------------------------------------
add_executable(pico_tcp_echo_client pico_tcp_echo_client.cpp)
target_include_directories(pico_tcp_echo_client PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(pico_tcp_echo_client PRIVATE coro_pico pico_stdlib pico_cyw43_arch_lwip_poll)
pico_enable_stdio_usb(pico_tcp_echo_client 1)
pico_enable_stdio_uart(pico_tcp_echo_client 0)
pico_add_extra_outputs(pico_tcp_echo_client)

# ---------------------------------------------------------------------------
# pico_ws2812_tcp
# ---------------------------------------------------------------------------
#add_executable(pico_ws2812_tcp pico_ws2812_tcp.cpp)
#target_include_directories(pico_ws2812_tcp PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
#target_link_libraries(pico_ws2812_tcp PRIVATE
#    coro_pico coro_pico_hal PicoLed
#    pico_stdlib pico_cyw43_arch_lwip_poll pico_lwip_mdns hardware_i2c
#)
#pico_enable_stdio_usb(pico_ws2812_tcp 1)
#pico_enable_stdio_uart(pico_ws2812_tcp 0)
#pico_add_extra_outputs(pico_ws2812_tcp)
