# examples/pico is Conan-only — this is the package's sole CMakeLists.txt,
# driven entirely by ../conanfile.py (`conan create`/`conan build`).
#
# Must call pico_sdk_init() itself, before project(). The profile's
# enabled_blocks conf keeps Conan's CMakeToolchain from setting
# CMAKE_SYSTEM_NAME/CMAKE_<LANG>_COMPILER (see ../pico_led/conanfile.py and
# doc/design/pico_led_conan_packaging.md), so there is no conflict over who
# owns the compiler/toolchain — pico_sdk_import.cmake does, same as any
# non-Conan Pico build.
cmake_minimum_required(VERSION 3.20)

if(NOT DEFINED ENV{PICO_SDK_PATH} OR "$ENV{PICO_SDK_PATH}" STREQUAL "")
    message(FATAL_ERROR "PICO_SDK_PATH environment variable is not set. "
        "Export it in the shell running `conan`, or add it under "
        "[buildenv] in the host profile.")
endif()

include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(pico_ws2812_tcp LANGUAGES C CXX ASM)
set(PICO_CXX_ENABLE_EXCEPTIONS 1)
pico_sdk_init()

# coro::pico / coro::pico_hal and pico_led::pico_led come from their
# respective Conan packages, found via CMakeDeps-generated *-config.cmake
# files. CMAKE_PREFIX_PATH/CMAKE_MODULE_PATH pointing at the generators
# folder is set by CMakeToolchain's "find_paths" block (see
# ../conanfile.py's generate()).
find_package(coro CONFIG REQUIRED)
find_package(pico_led CONFIG REQUIRED)

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

add_executable(pico_ws2812_tcp pico_ws2812_tcp.cpp)
target_include_directories(pico_ws2812_tcp PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(pico_ws2812_tcp PRIVATE
    coro::pico coro::pico_hal pico_led::pico_led
    pico_stdlib pico_cyw43_arch_lwip_poll pico_lwip_mdns hardware_i2c
)
pico_enable_stdio_usb(pico_ws2812_tcp 1)
pico_enable_stdio_uart(pico_ws2812_tcp 0)
pico_add_extra_outputs(pico_ws2812_tcp)
