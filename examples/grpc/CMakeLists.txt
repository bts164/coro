cmake_minimum_required(VERSION 3.23)
project(coro_grpc_example VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(coro CONFIG REQUIRED)
find_package(gRPC CONFIG REQUIRED)

add_executable(hello_server helloserver.cpp helloworld.proto)
target_link_libraries(hello_server PRIVATE coro::coro gRPC::grpc++)
target_include_directories(hello_server PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

get_target_property(grpc_cpp_plugin_location gRPC::grpc_cpp_plugin LOCATION)
protobuf_generate(TARGET hello_server
    LANGUAGE cpp)
protobuf_generate(
    TARGET hello_server
    LANGUAGE grpc
    GENERATE_EXTENSIONS .grpc.pb.h .grpc.pb.cc
    PLUGIN "protoc-gen-grpc=${grpc_cpp_plugin_location}")
