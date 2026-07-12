cmake_minimum_required(VERSION 3.23)
project(coro_io_examples VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(coro CONFIG REQUIRED)

add_executable(tcp_echo_client tcp_echo_client.cpp)
target_link_libraries(tcp_echo_client PRIVATE coro::coro)

add_executable(tcp_echo_server tcp_echo_server.cpp)
target_link_libraries(tcp_echo_server PRIVATE coro::coro)

add_executable(ws_echo_client ws_echo_client.cpp)
target_link_libraries(ws_echo_client PRIVATE coro::coro)

add_executable(ws_echo_server ws_echo_server.cpp)
target_link_libraries(ws_echo_server PRIVATE coro::coro)

add_executable(fibonacci fibonacci.cpp)
target_link_libraries(fibonacci PRIVATE coro::coro)

add_executable(length_prefixed_stream length_prefixed_stream.cpp)
target_link_libraries(length_prefixed_stream PRIVATE coro::coro)
