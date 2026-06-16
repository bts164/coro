cmake_minimum_required(VERSION 3.13)

# Must come before project() — sets up the ARM toolchain and SDK variables.
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(coro_pico_examples VERSION 0.1.0 LANGUAGES CXX C ASM)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(PICO_CXX_ENABLE_EXCEPTIONS 1)
pico_sdk_init()

set(CORO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
include(${CORO_ROOT}/cmake/platforms/pico.cmake)
include(${CORO_ROOT}/cmake/platforms/pico_hal.cmake)
# lwipopts.h lives alongside this CMakeLists.txt — add it to coro_pico's
# compile path so lwIP finds it when building the library sources.
target_include_directories(coro_pico PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

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

# ---------------------------------------------------------------------------
# pico_tcp_echo_server
# ---------------------------------------------------------------------------
add_executable(pico_tcp_echo_server pico_tcp_echo_server.cpp)
target_include_directories(pico_tcp_echo_server PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(pico_tcp_echo_server PRIVATE coro_pico pico_stdlib pico_cyw43_arch_lwip_poll)
pico_enable_stdio_usb(pico_tcp_echo_server 1)
pico_enable_stdio_uart(pico_tcp_echo_server 0)
pico_add_extra_outputs(pico_tcp_echo_server)   # generates .uf2 / .bin / .hex

# ---------------------------------------------------------------------------
# pico_tcp_echo_client
# ---------------------------------------------------------------------------
add_executable(pico_tcp_echo_client pico_tcp_echo_client.cpp)
target_include_directories(pico_tcp_echo_client PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(pico_tcp_echo_client PRIVATE coro_pico pico_stdlib pico_cyw43_arch_lwip_poll)
pico_enable_stdio_usb(pico_tcp_echo_client 1)
pico_enable_stdio_uart(pico_tcp_echo_client 0)
pico_add_extra_outputs(pico_tcp_echo_client)

# ---------------------------------------------------------------------------
# nanopb — proto code generation + runtime
# Generator: pip install -r requirements.txt (see requirements.txt).
# Runtime C sources: fetched from GitHub at the same version tag.
# ---------------------------------------------------------------------------
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
message(STATUS "nanopb runtime: ${NANOPB_DIR}")

set(PROTO_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/proto)
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

# led/ library — depends on ws2812_proto for ws2812.pb.h, so must come after.
add_subdirectory(led)

# ---------------------------------------------------------------------------
# pico_ws2812_tcp
# ---------------------------------------------------------------------------
add_executable(pico_ws2812_tcp pico_ws2812_tcp.cpp)
target_include_directories(pico_ws2812_tcp PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(pico_ws2812_tcp PRIVATE
    coro_pico coro_pico_hal coro_pico_led ws2812_proto
    pico_stdlib pico_cyw43_arch_lwip_poll pico_lwip_mdns hardware_i2c
)
pico_enable_stdio_usb(pico_ws2812_tcp 1)
pico_enable_stdio_uart(pico_ws2812_tcp 0)
pico_add_extra_outputs(pico_ws2812_tcp)
