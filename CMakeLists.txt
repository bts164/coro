cmake_minimum_required(VERSION 3.21)
project(coro VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(WITH_GPERFTOOLS "Build with gperftools" OFF)
option(CORO_USE_LOCAL_RUN_QUEUE "Use lock-free local run queue in WorkStealingExecutor (experimental)" ON)

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/Sanitize.cmake)

if(WITH_GPERFTOOLS)
    find_package(gperftools REQUIRED)
endif()
find_package(libuv REQUIRED)
find_package(Libwebsockets REQUIRED)

file(GLOB_RECURSE CORO_PUBLIC_HEADERS "include/coro/*.h" "include/coro/*.hpp")
message(STATUS ${CORO_PUBLIC_HEADERS})
add_library(coro
    # runtime/
    src/runtime/runtime.cpp
    src/runtime/executor.cpp
    src/runtime/single_threaded_uv_executor.cpp
    src/runtime/single_threaded_executor.cpp
    src/runtime/current_thread_executor.cpp
    src/runtime/work_sharing_executor.cpp
    src/runtime/work_stealing_executor.cpp
    # task/
    src/task/task.cpp
    src/task/waker.cpp
    src/task/blocking_pool.cpp
    src/task/context.cpp
    # sync/
    src/sync/cancellation_token.cpp
    # io/
    src/io/tcp_stream.cpp
    src/io/tcp_listener.cpp
    src/io/ws_stream.cpp
    src/io/ws_listener.cpp
    src/io/file.cpp
    #src/io/pipe.cpp
    src/io/circular_byte_buffer.cpp
)
if(CORO_USE_LOCAL_RUN_QUEUE)
    target_compile_definitions(coro PUBLIC CORO_USE_LOCAL_RUN_QUEUE)
endif()
target_include_directories(coro PUBLIC include)
target_link_libraries(coro PUBLIC
    libuv::uv_a
    $<$<BOOL:${WITH_GPERFTOOLS}>:gperftools::gperftools>
    websockets)

install(TARGETS coro)
# include/coro/pico/ is intentionally excluded from the installed package.
# Those headers require CORO_PICO and the Pico SDK — they have no use on
# desktop platforms and would fail to compile without the SDK headers.
# The pico unit tests (test/pico/) are a source-build-only artifact: anyone
# running them is building from source and has the headers directly. Consumers
# of the installed Conan package never need them.
install(DIRECTORY include/coro
    DESTINATION include
    FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp"
    PATTERN "pico" EXCLUDE
)
