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
    test_synchronize
    test_stream_handle
    test_coro_scope
)

foreach(t ${TEST_TARGETS})
    add_executable(${t} ${t}.cpp)
    target_link_libraries(${t} PRIVATE coro GTest::gtest GTest::gtest_main GTest::gmock)
    add_test(NAME ${t} COMMAND ${t})
endforeach()
