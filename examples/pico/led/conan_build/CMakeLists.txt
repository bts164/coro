# Standalone bootstrap used only when Conan builds the pico_led package on
# its own (see ../conanfile.py). Not used by examples/pico/CMakeLists.txt's
# add_subdirectory(led), which already has pico_sdk_init() done by its
# parent and uses the wider ../CMakeLists.txt (coro_pico_led, with effects).
#
# Must call pico_sdk_init() itself, before project() — same as
# examples/pico/CMakeLists.txt. The profile's enabled_blocks conf keeps
# Conan's CMakeToolchain from setting CMAKE_SYSTEM_NAME/CMAKE_<LANG>_COMPILER
# (see ../conanfile.py and doc/design/pico_led_conan_packaging.md), so there
# is no conflict over who owns the compiler/toolchain — pico_sdk_import.cmake
# does, same as any non-Conan Pico build.
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

project(pico_led LANGUAGES C CXX ASM)
set(PICO_CXX_ENABLE_EXCEPTIONS 1)
pico_sdk_init()

# coro::pico / coro::pico_hal come from the coro Conan package (built for
# settings.os == "baremetal" — see the root conanfile.py), found via the
# CMakeDeps-generated coro-config.cmake. CMAKE_PREFIX_PATH/CMAKE_MODULE_PATH
# pointing at the generators folder is set by CMakeToolchain's "find_paths"
# block (see ../conanfile.py's generate()) — no manual path wiring needed
# here, unlike the old CORO_ROOT-relative source build this replaced.
find_package(coro CONFIG REQUIRED)

set(LED_SRC_DIR ${CMAKE_CURRENT_LIST_DIR}/..)

add_library(pico_led STATIC
    ${LED_SRC_DIR}/color.cpp
    ${LED_SRC_DIR}/led_driver.cpp
)
pico_generate_pio_header(pico_led ${LED_SRC_DIR}/ws2812.pio)

# Consumers include via "led/led_driver.h" (matching the convention used by
# the non-Conan examples/pico build, e.g. pico_ws2812_tcp.cpp) — so headers
# are installed under include/led/, and the BUILD_INTERFACE points one level
# above this package's own files (LED_SRC_DIR's parent), since LED_SRC_DIR
# itself is a directory literally named "led".
target_include_directories(pico_led PUBLIC
    $<BUILD_INTERFACE:${LED_SRC_DIR}/..>
    $<INSTALL_INTERFACE:include>
)
# coro::pico_hal pulls in hardware_dma/hardware_irq/hardware_pio itself via a
# CMakeDeps build module (cmake/pico_hal_build_module.cmake, wired up in the
# root conanfile.py) — no need to relink hardware_pio here, even for this
# package's own compilation: the build module re-links it onto coro::pico_hal
# as soon as find_package(coro) runs above, and PUBLIC propagates from there.
# hardware_clocks stays explicit since it's only used internally by
# led_driver.cpp/ws2812.pio (clock_get_hz), not exposed via led_driver.h, so
# it's outside the scope of what coro_pico_hal's blanket-link is meant to cover.
target_link_libraries(pico_led PUBLIC
    coro::pico
    coro::pico_hal
    hardware_clocks
    pico_stdlib
)

# No install(EXPORT ...) here: the consumer side (KitchenLED) finds this
# package via Conan's CMakeDeps, which generates pico_led-config.cmake
# straight from conanfile.py's package_info() — it never reads a CMake
# export set installed by this file. Exporting one anyway would additionally
# require coro::pico/coro::pico_hal/hardware_clocks/pico_stdlib (pico_led's
# PUBLIC link deps) to each be in an export set of their own, and
# pico_generate_pio_header()'s generated-header include dir is an unguarded
# build-directory path that install(EXPORT ...) validation rejects outright.
include(GNUInstallDirs)
install(TARGETS pico_led
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(FILES
    ${LED_SRC_DIR}/color.h
    ${LED_SRC_DIR}/led_driver.h
    ${LED_SRC_DIR}/pixel_buffer.h
    DESTINATION include/led
)
