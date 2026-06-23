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

# PICO_SDK_PATH must reach this process's actual environment (e.g. exported
# in the shell conan runs in, or set via [buildenv] in the host profile).
# If it's missing, include() below fails non-fatally and CMake silently
# falls through to configuring against the host's gcc instead of
# arm-none-eabi-gcc, only crashing later and confusingly at pico_sdk_init().
if(NOT DEFINED ENV{PICO_SDK_PATH} OR "$ENV{PICO_SDK_PATH}" STREQUAL "")
    message(FATAL_ERROR "PICO_SDK_PATH environment variable is not set. "
        "Export it in the shell running `conan`, or add it under "
        "[buildenv] in the host profile.")
endif()

include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(pico_tcp_echo LANGUAGES C CXX ASM)
set(PICO_CXX_ENABLE_EXCEPTIONS 1)
pico_sdk_init()

# coro::pico comes from the coro Conan package (built for settings.os ==
# "baremetal" — see the root conanfile.py), found via the
# CMakeDeps-generated coro-config.cmake. CMAKE_PREFIX_PATH/CMAKE_MODULE_PATH
# pointing at the generators folder is set by CMakeToolchain's "find_paths"
# block (see ../conanfile.py's generate()).
find_package(coro CONFIG REQUIRED)

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

add_executable(pico_tcp_echo_server pico_tcp_echo_server.cpp)
target_include_directories(pico_tcp_echo_server PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(pico_tcp_echo_server PRIVATE coro::pico pico_stdlib pico_cyw43_arch_lwip_poll)
pico_enable_stdio_usb(pico_tcp_echo_server 1)
pico_enable_stdio_uart(pico_tcp_echo_server 0)
pico_add_extra_outputs(pico_tcp_echo_server)   # generates .uf2 / .bin / .hex

add_executable(pico_tcp_echo_client pico_tcp_echo_client.cpp)
target_include_directories(pico_tcp_echo_client PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(pico_tcp_echo_client PRIVATE coro::pico pico_stdlib pico_cyw43_arch_lwip_poll)
pico_enable_stdio_usb(pico_tcp_echo_client 1)
pico_enable_stdio_uart(pico_tcp_echo_client 0)
pico_add_extra_outputs(pico_tcp_echo_client)
