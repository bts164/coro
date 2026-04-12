find_package(GTest REQUIRED)

set(TEST_TARGETS
    test_poll_result
    test_waker_context
    test_future
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
    test_io_service
    test_oneshot
    test_mpsc
    test_watch
    test_file
    test_spawn_blocking
)

foreach(t ${TEST_TARGETS})
    add_executable(${t} ${t}.cpp)
    target_include_directories(${t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(${t} PRIVATE coro GTest::gtest GTest::gtest_main GTest::gmock)
    add_test(NAME ${t} COMMAND ${t})
endforeach()

# PCIe character device PollStream example — decoder, tests, and usage example
add_library(pcie_decoder pcie_decoder.cpp)
target_include_directories(pcie_decoder PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(pcie_decoder PUBLIC coro)

add_executable(test_pcie_decoder test_pcie_decoder.cpp)
target_include_directories(test_pcie_decoder PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_pcie_decoder PRIVATE coro pcie_decoder GTest::gtest GTest::gtest_main)
add_test(NAME PcieDecoderTest COMMAND test_pcie_decoder)

add_executable(test_pcie_stream_integration test_pcie_stream_integration.cpp)
target_include_directories(test_pcie_stream_integration PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_pcie_stream_integration PRIVATE coro pcie_decoder GTest::gtest GTest::gtest_main)
add_test(NAME PcieStreamIntegrationTest COMMAND test_pcie_stream_integration)

add_executable(pcie_reader pcie_reader.cpp)
target_include_directories(pcie_reader PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(pcie_reader PRIVATE coro pcie_decoder)
