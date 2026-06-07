cmake_minimum_required(VERSION 3.31)

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
# Pico executor tests — no hardware, no libuv, no lwIP.
#
# Compiles the portable coro core from source with CORO_PICO defined, then
# links pico_executor.cpp against it. cyw43_arch_poll() is a no-op stub.
# These tests run alongside the regular suite and catch regressions in core
# library changes that would break the pico port.
# ---------------------------------------------------------------------------
# coro_pico_core — portable coro core + pico_executor, compiled with CORO_PICO.
# Cannot use the installed coro::coro package (links libuv, no CORO_PICO) and
# OBJECT libraries don't cross Conan package boundaries, so core sources are
# compiled directly from the source tree. cyw43_arch_poll() is a no-op stub.
set(CORO_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_library(coro_pico_core STATIC
    ${CORO_SRC}/waker.cpp
    ${CORO_SRC}/task.cpp
    ${CORO_SRC}/cancellation_token.cpp
    ${CORO_SRC}/context.cpp
    ${CORO_SRC}/executor.cpp
    ${CORO_SRC}/runtime.cpp
    ${CORO_SRC}/pico_executor.cpp
    pico/stub/cyw43_arch_stub.cpp
)
target_include_directories(coro_pico_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    pico/stub
)
target_compile_definitions(coro_pico_core PUBLIC CORO_PICO)
target_compile_features(coro_pico_core PUBLIC cxx_std_20)

add_executable(test_pico_executor pico/test_pico_executor.cpp)
target_link_libraries(test_pico_executor PRIVATE
    coro_pico_core GTest::gtest GTest::gtest_main GTest::gmock)
add_test(NAME test_pico_executor COMMAND test_pico_executor)

