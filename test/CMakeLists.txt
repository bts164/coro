cmake_minimum_required(VERSION 3.31)

project(coro_unit_tests LANGUAGES CXX C)

include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/Sanitize.cmake)

find_package(coro CONFIG REQUIRED)
find_package(GTest CONFIG REQUIRED)
find_package(libunicorn CONFIG REQUIRED)

enable_testing()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# PCIe character device PollStream example — decoder, tests, and usage example
add_library(pcie_decoder misc/pcie_decoder.cpp)
target_include_directories(pcie_decoder PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/misc)
target_link_libraries(pcie_decoder PUBLIC coro::coro)

set(TEST_NAMES
    # detail/
    test_poll_result
    test_waker_context
    test_intrusive_list
    test_rc
    test_frame_pool
    # flat (foundational types — coro.h, future.h, stream.h, co_invoke.h)
    test_future
    test_future_ref
    test_stream
    test_coro
    test_coro_stream
    test_co_invoke
    # runtime/
    test_runtime
    test_executor_task
    test_work_sharing_executor
    test_work_stealing_executor
    test_single_threaded_uv_executor
    test_uv_future
    test_spawn_on
    test_local_run_queue
    # task/
    test_join_handle
    test_join_set
    test_stream_handle
    test_coro_scope
    test_spawn_blocking
    test_fiber
    # sync/
    test_event
    test_select
    test_sleep
    test_oneshot
    test_mpsc
    test_watch
    test_broadcast
    # io/
    test_file
    # test_pipe  # disabled — see io/test_pipe.cpp
    test_circular_byte_buffer
    test_poll_stream
    test_signal
    test_udp_socket
    # stress/
    test_skynet
    # misc/
    test_pcie_decoder
    test_pcie_stream_integration
)

set(TEST_SRCS
    # detail/
    detail/test_poll_result.cpp
    detail/test_waker_context.cpp
    detail/test_intrusive_list.cpp
    detail/test_rc.cpp
    detail/test_frame_pool.cpp
    # flat
    test_future.cpp
    test_future_ref.cpp
    test_stream.cpp
    test_coro.cpp
    test_coro_stream.cpp
    test_co_invoke.cpp
    # runtime/
    runtime/test_runtime.cpp
    runtime/test_executor_task.cpp
    runtime/test_work_sharing_executor.cpp
    runtime/test_work_stealing_executor.cpp
    runtime/test_single_threaded_uv_executor.cpp
    runtime/test_uv_future.cpp
    runtime/test_spawn_on.cpp
    runtime/test_local_run_queue.cpp
    # task/
    task/test_join_handle.cpp
    task/test_join_set.cpp
    task/test_stream_handle.cpp
    task/test_coro_scope.cpp
    task/test_spawn_blocking.cpp
    task/test_fiber.cpp
    # sync/
    sync/test_event.cpp
    sync/test_select.cpp
    sync/test_sleep.cpp
    sync/test_oneshot.cpp
    sync/test_mpsc.cpp
    sync/test_watch.cpp
    sync/test_broadcast.cpp
    # io/
    io/test_file.cpp
    # io/test_pipe.cpp  # disabled
    io/test_circular_byte_buffer.cpp
    io/test_poll_stream.cpp
    io/test_signal.cpp
    io/test_udp_socket.cpp
    # stress/
    stress/test_skynet.cpp
    # misc/
    misc/test_pcie_decoder.cpp
    misc/test_pcie_stream_integration.cpp
)

foreach(t src IN ZIP_LISTS TEST_NAMES TEST_SRCS)
    add_executable(${t} ${src})
    target_include_directories(${t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(${t} PRIVATE coro::coro GTest::gtest GTest::gtest_main GTest::gmock)
    add_test(NAME ${t} COMMAND ${t})
endforeach()
# test_skynet's coroutine-count is large enough that under TSan's
# per-thread/per-allocation shadow memory overhead it exhausts memory
# and locks up the host — still built (so it doesn't bitrot), just
# excluded from the run.
if(WITH_SANITIZE STREQUAL "tsan")
    set_tests_properties(test_skynet PROPERTIES DISABLED TRUE)
endif()

target_link_libraries(test_pcie_decoder PRIVATE pcie_decoder)
target_link_libraries(test_pcie_stream_integration PRIVATE pcie_decoder)

# ---------------------------------------------------------------------------
# Unicorn test of the real ARMv6-M switch_context()/init_fiber_entry()/
# coro_pico_enable_psp_stack() primitives (src/detail/fiber_context_pico.S) --
# see doc/design/fiber.md's "Testing strategy", "Stack overflow detection",
# and "Stack model (Pico backend)". Covers the happy path (fresh entry +
# resume round trip), switch_context()'s SP-range/canary overflow checks, and
# the one-time CONTROL/PSP switch (test_fiber_context_pico_alloc.cpp above
# covers alloc_fiber_stack()/free_fiber_stack() on the host instead, since
# that part has no ARM-specific code). Requires UC_MODE_MCLASS -- plain
# UC_MODE_THUMB doesn't implement the M-profile MRS/MSR special-register
# encodings (PSP/MSP/CONTROL) at all and faults with UC_ERR_INSN_INVALID,
# discovered while adding the PSP-switch test.
#
# fiber_context_pico.S is cross-assembled together with the test-only
# switch_context_harness.S (see that file), linked at a fixed -Ttext=0x10000
# so the addresses test_switch_context_pico.cpp hardcodes stay stable, and
# objcopy'd to a flat binary the test loads into Unicorn.
# ---------------------------------------------------------------------------
set(SWITCH_CONTEXT_PICO_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../src/detail/fiber_context_pico.S)
set(SWITCH_CONTEXT_HARNESS_SRC ${CMAKE_CURRENT_SOURCE_DIR}/task/fixtures/switch_context_harness.S)
set(SWITCH_CONTEXT_TEST_BIN ${CMAKE_CURRENT_BINARY_DIR}/switch_context_test.bin)

add_custom_command(
    OUTPUT ${SWITCH_CONTEXT_TEST_BIN}
    COMMAND arm-none-eabi-gcc -mcpu=cortex-m0plus -mthumb -c ${SWITCH_CONTEXT_PICO_SRC} -o fiber_context_pico.o
    COMMAND arm-none-eabi-gcc -mcpu=cortex-m0plus -mthumb -c ${SWITCH_CONTEXT_HARNESS_SRC} -o switch_context_harness.o
    COMMAND arm-none-eabi-ld fiber_context_pico.o switch_context_harness.o -Ttext=0x10000 -o switch_context_test.elf
    COMMAND arm-none-eabi-objcopy -O binary switch_context_test.elf ${SWITCH_CONTEXT_TEST_BIN}
    DEPENDS ${SWITCH_CONTEXT_PICO_SRC} ${SWITCH_CONTEXT_HARNESS_SRC}
    COMMENT "Cross-assembling fiber_context_pico.S + test harness for Unicorn"
    VERBATIM)
add_custom_target(switch_context_test_bin DEPENDS ${SWITCH_CONTEXT_TEST_BIN})

add_executable(test_switch_context_pico task/test_switch_context_pico.cpp)
add_dependencies(test_switch_context_pico switch_context_test_bin)
target_link_libraries(test_switch_context_pico PRIVATE
    unicorn::unicorn GTest::gtest GTest::gtest_main)
target_compile_definitions(test_switch_context_pico PRIVATE
    SWITCH_CONTEXT_TEST_BIN_PATH="${SWITCH_CONTEXT_TEST_BIN}")
add_test(NAME test_switch_context_pico COMMAND test_switch_context_pico)

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
    ${CORO_SRC}/task/waker.cpp
    ${CORO_SRC}/task/task.cpp
    ${CORO_SRC}/task/context.cpp
    ${CORO_SRC}/sync/cancellation_token.cpp
    ${CORO_SRC}/runtime/executor.cpp
    ${CORO_SRC}/runtime/runtime.cpp
    ${CORO_SRC}/runtime/current_thread_executor.cpp
    ${CORO_SRC}/detail/fiber_context_pico.cpp
    pico/stub/cyw43_arch_stub.cpp
)
target_include_directories(coro_pico_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    pico/stub
)
# CORO_PICO_FRAME_POOL is the experimental, off-by-default coroutine frame
# pooling opt-in (see doc/design/pico_port.md's "Coroutine frame pooling"
# section). Enabled here so FramePool.CoroFrameRoundTripsThroughPool in
# test_frame_pool.cpp keeps exercising the integration even though real Pico
# builds (cmake/platforms/pico.cmake) don't enable it by default.
target_compile_definitions(coro_pico_core PUBLIC CORO_PICO CORO_TCP_BACKEND_LWIP CORO_PICO_FRAME_POOL)
target_compile_features(coro_pico_core PUBLIC cxx_std_20)

# alloc_fiber_stack()/free_fiber_stack() (src/detail/fiber_context_pico.cpp,
# compiled into coro_pico_core above) are ordinary, target-independent C++ --
# no ARM instructions -- so they're exercised directly on the host here rather
# than needing Unicorn (contrast with test_switch_context_pico.cpp below,
# which does need it for the hand-written assembly). See
# doc/design/fiber.md's "Stack allocation and sizing"/"Stack overflow
# detection".
add_executable(test_fiber_context_pico_alloc task/test_fiber_context_pico_alloc.cpp)
target_include_directories(test_fiber_context_pico_alloc PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_fiber_context_pico_alloc PRIVATE
    coro_pico_core GTest::gtest GTest::gtest_main)
add_test(NAME test_fiber_context_pico_alloc COMMAND test_fiber_context_pico_alloc)

# test_pico_suite — the converted async test files compiled against coro_pico_core.
# CORO_PICO narrows AllExecutors to {CurrentThreadTraits} so only the pico executor
# is exercised. Desktop-only code (spawn_blocking, timeout, etc.) is guarded with
# #ifndef CORO_PICO in each test file.
add_executable(test_pico_suite
    detail/test_rc.cpp
    detail/test_frame_pool.cpp
    sync/test_watch.cpp
    sync/test_event.cpp
    sync/test_select.cpp
    sync/test_mpsc.cpp
    sync/test_oneshot.cpp
    sync/test_broadcast.cpp
    sync/test_isr_event.cpp
    task/test_join_set.cpp
    task/test_stream_handle.cpp
    test_co_invoke.cpp
    test_future_ref.cpp
    test_stream.cpp
)
target_include_directories(test_pico_suite PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_pico_suite PRIVATE
    coro_pico_core GTest::gtest GTest::gtest_main GTest::gmock)
add_test(NAME test_pico_suite COMMAND test_pico_suite)

# ---------------------------------------------------------------------------
# AsyncDmaTransfer tests — coro_pico_hal layer with DMA hardware stubs.
#
# Compiles src/pico/hal/dma.cpp against coro_pico_core + hardware/dma.h stub.
# CORO_PICO_TEST exposes coro_pico_hal_dma_fire_irq0() for test use.
# Tests use std::thread to simulate IRQ completion.
# ---------------------------------------------------------------------------
add_library(coro_pico_hal_core STATIC
    ${CORO_SRC}/../src/pico/hal/dma.cpp
    pico/stub/hardware/dma.cpp
)
target_include_directories(coro_pico_hal_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    pico/stub
)
target_link_libraries(coro_pico_hal_core PUBLIC coro_pico_core)
target_compile_definitions(coro_pico_hal_core PUBLIC CORO_PICO CORO_PICO_TEST)
target_compile_features(coro_pico_hal_core PUBLIC cxx_std_20)

add_executable(test_async_dma pico/test_async_dma.cpp)
target_include_directories(test_async_dma PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    pico/stub
)
target_link_libraries(test_async_dma PRIVATE
    coro_pico_hal_core GTest::gtest GTest::gtest_main GTest::gmock)
add_test(NAME test_async_dma COMMAND test_async_dma)

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
    ${LWIP_DIR}/src/core/ipv4/igmp.c
    ${LWIP_DIR}/src/apps/mqtt/mqtt.c
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

# ---------------------------------------------------------------------------
# Real lwIP UDP tests — host NO_SYS loopback.
#
# Same shape as coro_lwip_tcp above: links udp_socket_lwip.cpp (plus the
# shared socket_address.cpp, which needs lwip/ip4_addr.h under CORO_PICO)
# against the same lwip_host used by the TCP tests.
# ---------------------------------------------------------------------------
add_library(coro_lwip_udp STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/io/socket_address.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/io/lwip/udp_socket_lwip.cpp
)
target_include_directories(coro_lwip_udp PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/io/lwip
)
# CORO_UDP_BACKEND_LWIP selects udp_socket.h's lwIP branch; without it, the
# header falls through to its #else (libuv) branch and pulls in uv.h, which
# this pico-core-based target never links against.
target_compile_definitions(coro_lwip_udp PUBLIC CORO_UDP_BACKEND_LWIP)
target_link_libraries(coro_lwip_udp PUBLIC coro_pico_core lwip_host)

add_executable(test_udp_socket_real pico/test_udp_socket_real.cpp)
target_link_libraries(test_udp_socket_real PRIVATE
    coro_lwip_udp GTest::gtest GTest::gtest_main GTest::gmock)
add_test(NAME test_udp_socket_real COMMAND test_udp_socket_real)

# ---------------------------------------------------------------------------
# MQTT client tests — host NO_SYS loopback, real lwIP apps/mqtt.
#
# coro_lwip_mqtt links src/pico/mqtt.cpp against the same lwip_host used above
# (now also built with apps/mqtt/mqtt.c) plus coro_lwip_tcp, since the test
# fixture's fake broker is itself a coro::TcpListener-based server that parses
# the MQTT wire format by hand (lwIP has no broker implementation to test
# against, only the client wrapped here).
# ---------------------------------------------------------------------------
add_library(coro_lwip_mqtt STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/pico/mqtt.cpp
)
target_include_directories(coro_lwip_mqtt PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/io/lwip
)
target_link_libraries(coro_lwip_mqtt PUBLIC coro_lwip_tcp)

add_executable(test_mqtt_client pico/test_mqtt_client.cpp)
target_link_libraries(test_mqtt_client PRIVATE
    coro_lwip_mqtt GTest::gtest GTest::gtest_main GTest::gmock)
add_test(NAME test_mqtt_client COMMAND test_mqtt_client)

