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

# nanopb — proto code generation + runtime, same recipe as
# examples/pico/CMakeLists.txt's ws2812_proto target. Generator must be on
# PATH (`pip install -r examples/pico/requirements.txt`); the runtime C
# sources are fetched from GitHub at the matching version tag.
find_program(NANOPB_GENERATOR nanopb_generator REQUIRED)

include(FetchContent)
FetchContent_Declare(nanopb
    GIT_REPOSITORY https://github.com/nanopb/nanopb.git
    GIT_TAG        0.4.9.1
    GIT_SHALLOW    TRUE
)
FetchContent_GetProperties(nanopb)
if(NOT nanopb_POPULATED)
    FetchContent_Populate(nanopb)
endif()
set(NANOPB_DIR ${nanopb_SOURCE_DIR})

# PROTO_SRC_DIR is normally led/proto — exported by conanfile.py's
# export_sources() from examples/pico/proto/ (ws2812.proto is shared with
# the non-Conan examples/pico/CMakeLists.txt build). But export_sources()
# only runs for `conan export`/`conan create`; in editable mode (`conan
# editable add .`) this CMakeLists.txt runs directly against the live
# checkout, where that copy never happened — fall back to the real sibling
# location in that case.
if(EXISTS ${LED_SRC_DIR}/proto/ws2812.proto)
    set(PROTO_SRC_DIR ${LED_SRC_DIR}/proto)
else()
    set(PROTO_SRC_DIR ${LED_SRC_DIR}/../proto)
endif()
set(PROTO_OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/proto)
file(MAKE_DIRECTORY ${PROTO_OUT_DIR})

add_custom_command(
    OUTPUT
        ${PROTO_OUT_DIR}/ws2812.pb.c
        ${PROTO_OUT_DIR}/ws2812.pb.h
    COMMAND ${NANOPB_GENERATOR}
        -I ${PROTO_SRC_DIR}
        -D ${PROTO_OUT_DIR}
        ${PROTO_SRC_DIR}/ws2812.proto
    DEPENDS
        ${PROTO_SRC_DIR}/ws2812.proto
        ${PROTO_SRC_DIR}/ws2812.options
    COMMENT "Generating nanopb sources from ws2812.proto"
    VERBATIM
)

add_library(ws2812_proto STATIC
    ${PROTO_OUT_DIR}/ws2812.pb.c
    ${NANOPB_DIR}/pb_encode.c
    ${NANOPB_DIR}/pb_decode.c
    ${NANOPB_DIR}/pb_common.c
)
target_include_directories(ws2812_proto PUBLIC
    ${PROTO_OUT_DIR}
    ${NANOPB_DIR}
)

add_library(pico_led STATIC
    ${LED_SRC_DIR}/color.cpp
    ${LED_SRC_DIR}/led_driver.cpp
    ${LED_SRC_DIR}/effect_runner.cpp
    ${LED_SRC_DIR}/effects.cpp
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
    ws2812_proto
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
    ${LED_SRC_DIR}/effects.h
    ${LED_SRC_DIR}/effect_runner.h
    DESTINATION include/led
)
# ws2812.pb.h is generated into the build dir, not LED_SRC_DIR — co-located
# with the rest of include/led/ so effects.h's bare #include "ws2812.pb.h"
# resolves via quoted-include same-directory lookup with no extra include
# dir needed at consume time.
install(FILES
    ${PROTO_OUT_DIR}/ws2812.pb.h
    DESTINATION include/led
)
# ws2812.pb.h itself only needs <pb.h> (nanopb's single-header runtime type
# defs) — pb_decode.h/pb_encode.h aren't needed since nothing in effects.cpp/
# effect_runner.cpp calls pb_decode()/pb_encode(). Installed at include/ root
# (angle-bracket include, not same-directory) — package_info()'s
# includedirs=["include"] already covers it, no extra entry needed.
install(FILES
    ${NANOPB_DIR}/pb.h
    DESTINATION include
)
