cmake_minimum_required(VERSION 3.21)
project(coro VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ENABLE_TSAN "Build with ThreadSanitizer" OFF)
if(ENABLE_TSAN)
    add_compile_options(-fsanitize=thread -g)
    add_link_options(-fsanitize=thread)
endif()

find_package(GTest REQUIRED)
find_package(libuv REQUIRED)

add_library(coro
    src/waker.cpp
    src/cancellation_token.cpp
    src/context.cpp
    src/executor.cpp
    src/single_threaded_executor.cpp
    src/work_sharing_executor.cpp
    src/work_stealing_executor.cpp
    src/io_service.cpp
    src/runtime.cpp
)
target_include_directories(coro PUBLIC include)
target_link_libraries(coro PUBLIC libuv::uv_a)

enable_testing()
add_subdirectory(test)
