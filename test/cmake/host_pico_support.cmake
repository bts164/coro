# Host-side libraries that let the Pico configuration of coro be tested on the
# development machine: the portable core built with CORO_PICO against stubbed SDK
# calls, the HAL layer against stubbed hardware, and the lwIP backends against a
# real lwIP built for the host. Tests link these through HOST_LINK in tests.cmake;
# coro_pico_core is also what the PICO_STUB suite links.
#
# Included from test/CMakeLists.txt, so relative paths here are relative to test/.

# ---------------------------------------------------------------------------
# CurrentThreadExecutor tests — no hardware, no epoll driver.
#
# Compiles the portable coro core from source with CORO_PICO defined (selects
# CurrentThreadExecutor in runtime.cpp) and CORO_TCP_BACKEND_LWIP (selects
# lwIP TCP implementation in tcp_stream.h / tcp_listener.h).
# cyw43_arch_poll() is a no-op stub and time_us_64() reads the host clock. These
# tests run alongside the regular suite and catch regressions in core library
# changes that would break the Pico port.
#
# Links lwip_host (defined below): the Pico Runtime calls lwIP itself in its
# PicoNetwork::Lwip and Cyw43 modes, as it does on the board, so runtime.cpp needs
# lwIP's headers to compile and its timers and loopback queue to link.
# ---------------------------------------------------------------------------
# coro_pico_core — portable coro core + current_thread_executor.
# Cannot use the installed coro::coro package (desktop backends, wrong flags) and
# OBJECT libraries don't cross Conan package boundaries, so core sources are
# compiled directly from the source tree.
set(CORO_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_library(coro_pico_core STATIC
    ${CORO_SRC}/task/waker.cpp
    ${CORO_SRC}/task/task.cpp
    ${CORO_SRC}/task/context.cpp
    ${CORO_SRC}/sync/cancellation_token.cpp
    ${CORO_SRC}/sync/sleep.cpp
    ${CORO_SRC}/detail/timer_queue.cpp
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
target_link_libraries(coro_pico_core PUBLIC lwip_host)

# ---------------------------------------------------------------------------
# AsyncDmaTransfer / GpioPin tests — coro_pico_hal layer with hardware stubs.
#
# Compiles src/pico/hal/{dma,gpio}.cpp against coro_pico_core + the
# hardware/{dma,gpio}.h stubs. CORO_PICO_TEST exposes
# coro_pico_hal_dma_fire_irq0()/coro_pico_hal_gpio_fire_irq() for test use.
# Tests use std::thread to simulate IRQ completion.
# ---------------------------------------------------------------------------
add_library(coro_pico_hal_core STATIC
    ${CORO_SRC}/../src/pico/hal/dma.cpp
    ${CORO_SRC}/../src/pico/hal/gpio.cpp
    pico/stub/hardware/dma.cpp
    pico/stub/hardware/gpio.cpp
)
target_include_directories(coro_pico_hal_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    pico/stub
)
target_link_libraries(coro_pico_hal_core PUBLIC coro_pico_core)
target_compile_definitions(coro_pico_hal_core PUBLIC CORO_PICO CORO_PICO_TEST)
target_compile_features(coro_pico_hal_core PUBLIC cxx_std_20)

# ---------------------------------------------------------------------------
# Real lwIP TCP tests — host NO_SYS loopback.
#
# Downloads lwIP from source and builds it with lwipopts.h configured for
# NO_SYS mode. Links tcp_stream_lwip.cpp / tcp_listener_lwip.cpp against
# real lwIP so io/test_tcp_stream.cpp runs actual TCP through the stack, driven
# by a Runtime built with PicoNetwork::Lwip.
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
# header falls through to its #else (IoDriver) branch and pulls in the epoll
# driver, which this pico-core-based target never links against.
target_compile_definitions(coro_lwip_udp PUBLIC CORO_UDP_BACKEND_LWIP)
target_link_libraries(coro_lwip_udp PUBLIC coro_pico_core lwip_host)

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
