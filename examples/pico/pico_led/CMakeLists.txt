# examples/pico is Conan-only — this is the package's sole CMakeLists.txt,
# driven entirely by ../conanfile.py (`conan create`/`conan build`). There is
# no in-tree add_subdirectory() build of pico_led.
#
# Must call pico_sdk_init() itself, before project(). The profile's
# enabled_blocks conf keeps Conan's CMakeToolchain from setting
# CMAKE_SYSTEM_NAME/CMAKE_<LANG>_COMPILER
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

# nanopb — see experimental/nanopb/conanfile.py. find_package() pulls in
# nanopb_generate.cmake (a cmake_build_modules entry from the host package's
# package_info()), which defines nanopb_generate_cpp() below — it internally
# find_program()s nanopb_generator, the PyInstaller-frozen executable that
# this package's tool_requires("nanopb/...") in build_requirements() puts on
# PATH via Conan's buildenv.
find_package(nanopb CONFIG REQUIRED)

# proto/ws2812.proto lives directly under this package now (a sibling of
# CMakeLists.txt, like include/ and src/) — exported by conanfile.py's
# export_sources() like every other file, so this path resolves the same
# way whether running from a real `conan create` or in editable mode
# (`conan editable add .`) against the live checkout.
# Generate into pico_led/ so #include "pico_led/ws2812.pb.h" resolves when
# ${CMAKE_CURRENT_BINARY_DIR} is on the include path (the build-tree equivalent
# of the installed include/ root).
set(PROTO_OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/pico_led)

nanopb_generate_cpp(PROTO_SRCS PROTO_HDRS
    PROTOS ${CMAKE_CURRENT_SOURCE_DIR}/proto/ws2812.proto
    PROTO_PATH ${CMAKE_CURRENT_SOURCE_DIR}/proto
    OUT_DIR ${PROTO_OUT_DIR}
)

# PROTO_SRCS is compiled directly into pico_led (not a separate library) —
# only the pico_led target is install(TARGETS ...)'d / declared in
# conanfile.py's cpp_info.libs, so a separate ws2812_proto static lib would
# never ship in the Conan package, leaving Command_msg/Response_msg (defined
# in ws2812.pb.c) undefined at link time for any consumer — e.g. ws2812_tcp,
# which needs those symbols for pb_encode()/pb_decode() and previously hit
# exactly that: undefined reference to Command_msg/Response_msg.
add_library(pico_led STATIC
    src/color.cpp
    src/led_driver.cpp
    src/effect_runner.cpp
    src/effects.cpp
    ${PROTO_SRCS}
)
# OUTPUT_DIR puts ws2812.pio.h under pico_led/ (matching the installed layout).
# pico_generate_pio_header adds OUTPUT_DIR itself; we add the parent so that
# #include "pico_led/ws2812.pio.h" resolves against ${CMAKE_CURRENT_BINARY_DIR}.
pico_generate_pio_header(pico_led ${CMAKE_SOURCE_DIR}/src/ws2812.pio
    OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/pico_led)
target_include_directories(pico_led PUBLIC ${CMAKE_CURRENT_BINARY_DIR})

# Headers already live under include/pico_led/ in this package's own tree,
# matching the installed layout below — so a plain "include" (no
# BUILD_INTERFACE/INSTALL_INTERFACE generator-expression split needed here,
# since there's no install(EXPORT ...) — see below) is enough for consumers
# to #include "pico_led/led_driver.h" once this target is linked.
target_include_directories(pico_led PUBLIC include)

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
    nanopb::nanopb
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
    include/pico_led/color.h
    include/pico_led/led_driver.h
    include/pico_led/pixel_buffer.h
    include/pico_led/effects.h
    include/pico_led/effect_runner.h
    DESTINATION include/pico_led
)
# ws2812.pb.h is generated into the build dir, not include/pico_led/ in the
# source tree — installed alongside the rest of include/pico_led/ so
# effects.h's bare #include "ws2812.pb.h" resolves via quoted-include
# same-directory lookup with no extra include dir needed at consume time.
install(FILES
    ${PROTO_OUT_DIR}/ws2812.pb.h
    DESTINATION include/pico_led
)
# ws2812.pb.h itself only needs <pb.h> (nanopb's single-header runtime type
# defs) — pb_decode.h/pb_encode.h aren't needed since nothing in effects.cpp/
# effect_runner.cpp calls pb_decode()/pb_encode(). pb.h now comes from the
# nanopb::nanopb dependency itself (cpp_info.requires in ../conanfile.py),
# not installed by this package.
