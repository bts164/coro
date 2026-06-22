# cmake/platforms/pico.cmake
#
# Defines coro_core and coro_pico CMake targets for the Raspberry Pi Pico (RP2040/RP2350).
# Backend: CORO_PICO executor + CORO_TCP_BACKEND_LWIP.
#
# Prerequisites (caller must do these before including this file):
#   include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)
#   project(...)
#   pico_sdk_init()
#
# Usage:
#   set(CORO_ROOT /path/to/coro)   # optional — defaults to two levels above this file
#   set(CORO_PICO_WITH_MQTT ON)    # optional — bakes LWIP_MQTT 1 into the default lwipopts.h
#   include(${CORO_ROOT}/cmake/platforms/pico.cmake)
#   target_link_libraries(my_firmware PRIVATE coro_pico)
#
# lwipopts.h: coro_pico owns it. By default it's generated from
# src/io/lwip/lwipopts.h.in (see doc/design/lwip_config.md for why this is
# centralized rather than left to each application). To supply a completely
# custom file instead — for tuning beyond what CORO_PICO_WITH_MQTT covers —
# set CORO_PICO_LWIPOPTS_DIR to a directory containing your own lwipopts.h
# before including this file. Whichever is used, it applies to every target
# that links coro_pico/coro_pico_mqtt in this configure run — see
# pico_mqtt.cmake, which must see the same directory.

include_guard(GLOBAL)

if(NOT DEFINED CORO_ROOT)
    # Default: two directories up from cmake/platforms/
    set(CORO_ROOT ${CMAKE_CURRENT_LIST_DIR}/../..)
endif()

set(_CORO_SRC     ${CORO_ROOT}/src)
set(_CORO_INCLUDE ${CORO_ROOT}/include)

if(DEFINED CORO_PICO_LWIPOPTS_DIR)
    set(CORO_PICO_LWIPOPTS_INCLUDE_DIR ${CORO_PICO_LWIPOPTS_DIR})
else()
    option(CORO_PICO_WITH_MQTT "Bake LWIP_MQTT 1 into the bundled default lwipopts.h" OFF)
    set(LWIP_MQTT ${CORO_PICO_WITH_MQTT})
    set(_CORO_LWIPOPTS_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/coro_pico_lwipopts)
    configure_file(${_CORO_SRC}/io/lwip/lwipopts.h.in
        ${_CORO_LWIPOPTS_GEN_DIR}/lwipopts.h)
    set(CORO_PICO_LWIPOPTS_INCLUDE_DIR ${_CORO_LWIPOPTS_GEN_DIR})
endif()

# ---------------------------------------------------------------------------
# coro_pico — full coro stack for Pico W: portable core + executor + lwIP TCP.
#
# All sources are compiled into one archive to avoid circular-dependency
# link-order issues between the core (which references CurrentThreadExecutor)
# and the executor (which references core symbols).
#
#   target_link_libraries(my_firmware PRIVATE coro_pico pico_stdlib pico_cyw43_arch_lwip_poll)
# ---------------------------------------------------------------------------
add_library(coro_pico STATIC
    ${_CORO_SRC}/task/waker.cpp
    ${_CORO_SRC}/task/task.cpp
    ${_CORO_SRC}/task/context.cpp
    ${_CORO_SRC}/sync/cancellation_token.cpp
    ${_CORO_SRC}/runtime/executor.cpp
    ${_CORO_SRC}/runtime/runtime.cpp
    ${_CORO_SRC}/runtime/current_thread_executor.cpp
    ${_CORO_SRC}/io/lwip/tcp_stream_lwip.cpp
    ${_CORO_SRC}/io/lwip/tcp_listener_lwip.cpp
)
target_include_directories(coro_pico PUBLIC  ${_CORO_INCLUDE})
# CORO_PICO_LWIPOPTS_INCLUDE_DIR is PUBLIC, not PRIVATE: lwipopts.h must be
# visible to any target that #includes Pico SDK/lwIP headers (e.g.
# <pico/cyw43_arch.h>), not just to coro_pico's own .cpp files — see
# doc/design/lwip_config.md ("coro owns lwipopts.h exclusively... transitively
# through coro"). ${_CORO_SRC}/io/lwip stays PRIVATE — it's coro's own
# internal lwIP-backend headers, not meant for consumers.
target_include_directories(coro_pico PRIVATE ${_CORO_SRC}/io/lwip)
target_include_directories(coro_pico PUBLIC  ${CORO_PICO_LWIPOPTS_INCLUDE_DIR})
target_compile_features(coro_pico PUBLIC cxx_std_23)
target_compile_definitions(coro_pico PUBLIC CORO_PICO CORO_TCP_BACKEND_LWIP)
# Coroutine frame pooling (CoroPromiseBase's pooled operator new/delete in
# coro.h) is experimental and off by default. To opt in:
#   target_compile_definitions(coro_pico PUBLIC CORO_PICO_FRAME_POOL)
# See doc/design/pico_port.md's "Coroutine frame pooling" section.
# pico_stdlib and pico_cyw43_arch_lwip_poll are PRIVATE — they supply include
# paths for compiling our sources but must NOT be bundled into the archive.
# Applications must link them directly (see examples/pico/CMakeLists.txt).
target_link_libraries(coro_pico PRIVATE pico_stdlib pico_cyw43_arch_lwip_poll)
