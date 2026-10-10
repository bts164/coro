cmake_minimum_required(VERSION 3.31)

# One build for every platform the tests run on. tests.cmake lists each test file
# once, with the builds it belongs to; CORO_PLATFORM selects which of them this
# configure run produces:
#   desktop (default) — the test executables for the development machine
#   pico              — the on-target firmware images for RP2040/RP2350
# Set by conanfile.py when settings.os == "baremetal", like the library's own
# CORO_PLATFORM. See doc/design/on_target_tests.md.
set(CORO_PLATFORM "desktop" CACHE STRING "Target platform: desktop or pico")
set_property(CACHE CORO_PLATFORM PROPERTY STRINGS desktop pico)

if(CORO_PLATFORM STREQUAL "pico")
    if(NOT DEFINED ENV{PICO_SDK_PATH} OR "$ENV{PICO_SDK_PATH}" STREQUAL "")
        message(FATAL_ERROR "PICO_SDK_PATH environment variable is not set. "
            "Export it in the shell running cmake/conan, or add it under "
            "[buildenv] in the Conan host profile.")
    endif()
    include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)
    project(coro_unit_tests LANGUAGES CXX C ASM)
    # gtest reports a failure escaping a test body as a failed test only if it can
    # catch it; EXPECT_THROW needs exceptions as well.
    set(PICO_CXX_ENABLE_EXCEPTIONS 1)
    pico_sdk_init()
else()
    project(coro_unit_tests LANGUAGES CXX C)
endif()
# Desktop: adds the sanitizer's flags to everything below. Pico: only sets
# CORO_SANITIZE_OPTIONS, which cmake/on_target_pico.cmake puts on the test sources.
include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/Sanitize.cmake)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CORO_TEST_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

find_package(coro CONFIG REQUIRED)

include(cmake/coro_test.cmake)

if(CORO_PLATFORM STREQUAL "pico")
    include(cmake/on_target_pico.cmake)
    include(tests.cmake)
    coro_add_on_target_images()
    return()
endif()

find_package(GTest CONFIG REQUIRED)
find_package(libunicorn CONFIG REQUIRED)

enable_testing()

include(cmake/host_pico_support.cmake)
include(tests.cmake)
coro_add_pico_stub_suite()

# test_skynet's coroutine-count is large enough that under TSan's
# per-thread/per-allocation shadow memory overhead it exhausts memory
# and locks up the host — still built (so it doesn't bitrot), just
# excluded from the run.
if(WITH_SANITIZE STREQUAL "tsan")
    set_tests_properties(test_skynet PROPERTIES DISABLED TRUE)
endif()

# The binary test_switch_context_pico loads into the emulator — see
# cmake/host_pico_support.cmake.
add_dependencies(test_switch_context_pico switch_context_test_bin)
target_compile_definitions(test_switch_context_pico PRIVATE
    SWITCH_CONTEXT_TEST_BIN_PATH="${SWITCH_CONTEXT_TEST_BIN}")
