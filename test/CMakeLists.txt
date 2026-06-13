cmake_minimum_required(VERSION 3.31)

option(ENABLE_ASAN "Build with AddressSanitizer + LeakSanitizer + UBSan" OFF)
option(ENABLE_TSAN "Build with ThreadSanitizer" OFF)

if(ENABLE_ASAN AND ENABLE_TSAN)
    message(FATAL_ERROR "ENABLE_ASAN and ENABLE_TSAN are mutually exclusive")
endif()
if(ENABLE_ASAN)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address,undefined)
endif()
if(ENABLE_TSAN)
    add_compile_options(-fsanitize=thread -g)
    add_link_options(-fsanitize=thread)
endif()

find_package(coro CONFIG REQUIRED)
find_package(GTest CONFIG REQUIRED)

enable_testing()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# PCIe character device PollStream example — decoder, tests, and usage example
add_library(pcie_decoder pcie_decoder.cpp)
target_include_directories(pcie_decoder PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(pcie_decoder PUBLIC coro::coro)

set(TEST_TARGETS
    test_poll_result
    test_waker_context
    test_future
    test_future_ref
    test_stream
    test_coro
    test_coro_stream
    test_join_handle
    test_executor_task
    test_runtime
    test_stream_handle
    test_coro_scope
    test_select
    test_co_invoke
    test_join_set
    test_work_sharing_executor
    test_work_stealing_executor
    test_sleep
    test_single_threaded_uv_executor
    test_oneshot
    test_mpsc
    test_watch
    test_file
    #test_pipe
    test_spawn_blocking
    test_poll_stream
    test_circular_byte_buffer
    test_spawn_on
    test_uv_future
    test_event
    test_skynet
    test_pcie_decoder
    test_pcie_stream_integration
    test_intrusive_list
    test_local_run_queue
)

foreach(t ${TEST_TARGETS})
    add_executable(${t} ${t}.cpp)
    target_include_directories(${t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(${t} PRIVATE coro::coro GTest::gtest GTest::gtest_main GTest::gmock)
    add_test(NAME ${t} COMMAND ${t})
endforeach()

target_link_libraries(test_pcie_decoder PRIVATE pcie_decoder)
target_link_libraries(test_pcie_stream_integration PRIVATE pcie_decoder)

# ---------------------------------------------------------------------------
# CurrentThreadExecutor tests — no hardware, no libuv, no lwIP.
#
# Compiles the portable coro core from source with CORO_PICO defined (selects
# CurrentThreadExecutor in runtime.cpp) and CORO_TCP_BACKEND_LWIP (selects
# lwIP TCP implementation in tcp_stream.h / tcp_listener.h).
# cyw43_arch_poll() and time_us_64() are no-op stubs. These tests run alongside
# the regular suite and catch regressions in core library changes that would
# break the Pico port.
# ---------------------------------------------------------------------------
# coro_pico_core — portable coro core + current_thread_executor.
# Cannot use the installed coro::coro package (links libuv, wrong flags) and
# OBJECT libraries don't cross Conan package boundaries, so core sources are
# compiled directly from the source tree.
set(CORO_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_library(coro_pico_core STATIC
    ${CORO_SRC}/waker.cpp
    ${CORO_SRC}/task.cpp
    ${CORO_SRC}/cancellation_token.cpp
    ${CORO_SRC}/context.cpp
    ${CORO_SRC}/executor.cpp
    ${CORO_SRC}/runtime.cpp
    ${CORO_SRC}/current_thread_executor.cpp
    pico/stub/cyw43_arch_stub.cpp
)
target_include_directories(coro_pico_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    pico/stub
)
target_compile_definitions(coro_pico_core PUBLIC CORO_PICO CORO_TCP_BACKEND_LWIP)
target_compile_features(coro_pico_core PUBLIC cxx_std_20)

# test_pico_suite — the converted async test files compiled against coro_pico_core.
# CORO_PICO narrows AllExecutors to {CurrentThreadTraits} so only the pico executor
# is exercised. Desktop-only code (spawn_blocking, timeout, etc.) is guarded with
# #ifndef CORO_PICO in each test file.
add_executable(test_pico_suite
    test_stream_handle.cpp
    test_watch.cpp
    test_future_ref.cpp
    test_stream.cpp
    test_event.cpp
    test_select.cpp
    test_co_invoke.cpp
    test_join_set.cpp
    test_mpsc.cpp
    test_oneshot.cpp
)
target_include_directories(test_pico_suite PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_pico_suite PRIVATE
    coro_pico_core GTest::gtest GTest::gtest_main GTest::gmock)
add_test(NAME test_pico_suite COMMAND test_pico_suite)

# ---------------------------------------------------------------------------
# TCP stream tests — lwIP backend, stub networking.
#
# Links against coro_pico_core + stub implementations of TcpStream/TcpListener
# (no real lwIP required). Tests verify compile-time API properties and
# executor integration. Actual loopback networking tests are commented out in
# test_tcp_stream.cpp pending host lwIP NO_SYS infrastructure setup.
# ---------------------------------------------------------------------------
add_library(coro_lwip_tcp_stubs STATIC
    pico/stub/lwip_tcp_stubs.cpp
)
target_link_libraries(coro_lwip_tcp_stubs PUBLIC coro_pico_core)

add_executable(test_tcp_stream pico/test_tcp_stream.cpp)
target_link_libraries(test_tcp_stream PRIVATE
    coro_lwip_tcp_stubs GTest::gtest GTest::gtest_main GTest::gmock)
add_test(NAME test_tcp_stream COMMAND test_tcp_stream)

# ---------------------------------------------------------------------------
# Real lwIP TCP tests — host NO_SYS loopback.
#
# Downloads lwIP from source and builds it with lwipopts.h configured for
# NO_SYS mode. Links tcp_stream_lwip.cpp / tcp_listener_lwip.cpp against
# real lwIP so the loopback tests run actual TCP through the stack.
#
# Requires: cmake 3.14+ (FetchContent). No extra system packages needed.
# ---------------------------------------------------------------------------
include(FetchContent)
FetchContent_Declare(
    lwip_src
    GIT_REPOSITORY https://github.com/lwip-tcpip/lwip.git
    GIT_TAG        STABLE-2_2_0_RELEASE
    GIT_SHALLOW    TRUE
    EXCLUDE_FROM_ALL   # populate sources only; don't build lwIP's own targets
)
FetchContent_MakeAvailable(lwip_src)

set(LWIP_DIR ${lwip_src_SOURCE_DIR})

add_library(lwip_host STATIC
    ${LWIP_DIR}/src/core/def.c
    ${LWIP_DIR}/src/core/dns.c
    ${LWIP_DIR}/src/core/inet_chksum.c
    ${LWIP_DIR}/src/core/init.c
    ${LWIP_DIR}/src/core/ip.c
    ${LWIP_DIR}/src/core/mem.c
    ${LWIP_DIR}/src/core/memp.c
    ${LWIP_DIR}/src/core/netif.c
    ${LWIP_DIR}/src/core/pbuf.c
    ${LWIP_DIR}/src/core/stats.c
    ${LWIP_DIR}/src/core/timeouts.c
    ${LWIP_DIR}/src/core/tcp.c
    ${LWIP_DIR}/src/core/tcp_in.c
    ${LWIP_DIR}/src/core/tcp_out.c
    ${LWIP_DIR}/src/core/udp.c
    ${LWIP_DIR}/src/core/ipv4/ip4.c
    ${LWIP_DIR}/src/core/ipv4/ip4_addr.c
    ${LWIP_DIR}/src/core/ipv4/ip4_frag.c
    ${LWIP_DIR}/src/core/ipv4/icmp.c
    pico/lwip_host/sys_now.c
)
# Note: loopif.c was removed in lwIP 2.2.0 — loopback netif support is now
# built into netif.c and activated via LWIP_HAVE_LOOPIF in lwipopts.h.
target_include_directories(lwip_host PUBLIC
    ${LWIP_DIR}/src/include
    pico/lwip_host         # lwipopts.h lives here
)

# coro lwIP TCP sources compiled against real lwIP.
add_library(coro_lwip_tcp STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/io/lwip/tcp_stream_lwip.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/io/lwip/tcp_listener_lwip.cpp
)
target_include_directories(coro_lwip_tcp PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/io/lwip
)
target_link_libraries(coro_lwip_tcp PUBLIC coro_pico_core lwip_host)

add_executable(test_tcp_stream_real pico/test_tcp_stream_real.cpp)
target_link_libraries(test_tcp_stream_real PRIVATE
    coro_lwip_tcp GTest::gtest GTest::gtest_main GTest::gmock)
add_test(NAME test_tcp_stream_real COMMAND test_tcp_stream_real)

