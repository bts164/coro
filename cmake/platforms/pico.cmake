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
#   include(${CORO_ROOT}/cmake/platforms/pico.cmake)
#   target_link_libraries(my_firmware PRIVATE coro_pico)

include_guard(GLOBAL)

if(NOT DEFINED CORO_ROOT)
    # Default: two directories up from cmake/platforms/
    set(CORO_ROOT ${CMAKE_CURRENT_LIST_DIR}/../..)
endif()

set(_CORO_SRC     ${CORO_ROOT}/src)
set(_CORO_INCLUDE ${CORO_ROOT}/include)

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
target_include_directories(coro_pico PRIVATE ${_CORO_SRC}/io/lwip)
target_compile_features(coro_pico PUBLIC cxx_std_23)
target_compile_definitions(coro_pico PUBLIC CORO_PICO CORO_TCP_BACKEND_LWIP)
# pico_stdlib and pico_cyw43_arch_lwip_poll are PRIVATE — they supply include
# paths for compiling our sources but must NOT be bundled into the archive.
# Applications must link them directly (see examples/pico/CMakeLists.txt).
target_link_libraries(coro_pico PRIVATE pico_stdlib pico_cyw43_arch_lwip_poll)
