cmake_minimum_required(VERSION 3.13)

# Must come before project() — sets up the ARM toolchain and SDK variables.
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(coro_pico VERSION 0.1.0 LANGUAGES CXX C ASM)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

pico_sdk_init()

# ---------------------------------------------------------------------------
# coro_core — portable coroutine machinery, no libuv or platform I/O.
# ---------------------------------------------------------------------------
add_library(coro_core OBJECT
    ../src/waker.cpp
    ../src/task.cpp
    ../src/cancellation_token.cpp
    ../src/context.cpp
    ../src/executor.cpp
)
target_include_directories(coro_core PUBLIC ../include)
target_compile_features(coro_core PUBLIC cxx_std_20)
target_compile_definitions(coro_core PUBLIC CORO_PICO)

# ---------------------------------------------------------------------------
# coro_pico — Pico executor + lwIP TCP. Links coro_core transitively.
#
# Users: add_subdirectory(path/to/coro/pico) then:
#   target_link_libraries(my_firmware coro_pico)
# ---------------------------------------------------------------------------
add_library(coro_pico STATIC
    ../src/pico_executor.cpp
    ../src/pico_tcp_stream.cpp
    ../src/pico_tcp_listener.cpp
)
target_link_libraries(coro_pico PUBLIC
    coro_core
    pico_stdlib
    pico_cyw43_arch_lwip_poll
)

