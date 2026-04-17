cmake_minimum_required(VERSION 3.21)
project(coro VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ENABLE_TSAN "Build with ThreadSanitizer" OFF)
if(ENABLE_TSAN)
    add_compile_options(-fsanitize=thread -g)
    add_link_options(-fsanitize=thread)
endif()

option(BUILD_TESTING "Build unit tests" OFF)
option(BUILD_EXAMPLES "Build examples" OFF)

#find_package(gperftools REQUIRED)
find_package(libuv REQUIRED)
find_package(Libwebsockets REQUIRED)

file(GLOB_RECURSE CORO_PUBLIC_HEADERS "include/coro/*.h" "include/coro/*.hpp")
message(STATUS ${CORO_PUBLIC_HEADERS})
add_library(coro
    src/waker.cpp
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
    src/ws_stream.cpp
    src/ws_listener.cpp
    src/file.cpp
    src/circular_byte_buffer.cpp
)
target_include_directories(coro PUBLIC include)
target_link_libraries(coro PUBLIC libuv::uv_a websockets cap)

if(BUILD_TESTING)
    find_package(GTest REQUIRED)
    enable_testing()
    add_subdirectory(test)
endif()

if(BUILD_EXAMPLES)
    add_subdirectory(examples/io)
endif()

install(TARGETS coro PUBLIC_HEADER DESTINATION include/coro)
install(DIRECTORY include/coro                                                                                                          
    DESTINATION include                                                                                                                 
    FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp"                                                                                        
)
