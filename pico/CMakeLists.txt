cmake_minimum_required(VERSION 3.13)

# Must come before project() — sets up the ARM toolchain and SDK variables.
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(coro_pico VERSION 0.1.0 LANGUAGES CXX C ASM)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(PICO_CXX_ENABLE_EXCEPTIONS 1)
pico_sdk_init()

include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/platforms/pico.cmake)
