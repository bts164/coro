cmake_minimum_required(VERSION 3.21)
project(coro VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(ENABLE_TSAN "Build with ThreadSanitizer" OFF)
option(WITH_GPERFTOOLS "Build with gperftools" OFF)
option(CORO_USE_LOCAL_RUN_QUEUE "Use lock-free local run queue in WorkStealingExecutor (experimental)" ON)
if(ENABLE_TSAN)
    add_compile_options(-fsanitize=thread -g)
    add_link_options(-fsanitize=thread)
endif()

if(WITH_GPERFTOOLS)
    find_package(gperftools REQUIRED)
endif()
find_package(libuv REQUIRED)
find_package(Libwebsockets REQUIRED)

file(GLOB_RECURSE CORO_PUBLIC_HEADERS "include/coro/*.h" "include/coro/*.hpp")
message(STATUS ${CORO_PUBLIC_HEADERS})
add_library(coro
    src/waker.cpp
    src/task.cpp
    src/blocking_pool.cpp
    src/cancellation_token.cpp
    src/context.cpp
    src/executor.cpp
    src/single_threaded_executor.cpp
    src/work_sharing_executor.cpp
    src/work_stealing_executor.cpp
    src/single_threaded_uv_executor.cpp
    src/runtime.cpp
    src/tcp_stream.cpp
    src/tcp_listener.cpp
    src/ws_stream.cpp
    src/ws_listener.cpp
    src/file.cpp
    src/circular_byte_buffer.cpp
)
if(CORO_USE_LOCAL_RUN_QUEUE)
    target_compile_definitions(coro PUBLIC CORO_USE_LOCAL_RUN_QUEUE)
endif()
target_include_directories(coro PUBLIC include)
target_link_libraries(coro PUBLIC
    libuv::uv_a
    $<$<BOOL:${WITH_GPERFTOOLS}>:gperftools::gperftools>
    websockets)

install(TARGETS coro PUBLIC_HEADER DESTINATION include/coro)
install(DIRECTORY include/coro
    DESTINATION include
    FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp"
)
