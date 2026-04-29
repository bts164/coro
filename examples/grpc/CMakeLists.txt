cmake_minimum_required(VERSION 3.23)
project(coro_grpc_example VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(coro CONFIG REQUIRED)
find_package(gRPC CONFIG REQUIRED)
find_package(xtensor CONFIG REQUIRED)

get_target_property(grpc_cpp_plugin_location gRPC::grpc_cpp_plugin LOCATION)

add_executable(hello_server src/helloserver.cpp src/helloworld.proto)
target_link_libraries(hello_server PRIVATE coro::coro gRPC::grpc++ absl::log absl::log_initialize)
target_include_directories(hello_server PRIVATE include ${CMAKE_CURRENT_BINARY_DIR})
protobuf_generate(TARGET hello_server
    LANGUAGE cpp
    IMPORT_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/src
    PROTOC_OUT_DIR ${CMAKE_CURRENT_BINARY_DIR})
protobuf_generate(
    TARGET hello_server
    LANGUAGE grpc
    GENERATE_EXTENSIONS .grpc.pb.h .grpc.pb.cc
    PLUGIN "protoc-gen-grpc=${grpc_cpp_plugin_location}"
    IMPORT_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/src
    PROTOC_OUT_DIR ${CMAKE_CURRENT_BINARY_DIR})

add_executable(gendata src/gendata.cpp)
target_link_libraries(gendata PRIVATE coro::coro absl::log absl::log_initialize xtensor)
target_include_directories(gendata PRIVATE include)

add_executable(signal_server src/signalserver.cpp src/plotsignal.proto)
target_link_libraries(signal_server PRIVATE coro::coro gRPC::grpc++ absl::log absl::log_initialize xtensor)
target_include_directories(signal_server PRIVATE include ${CMAKE_CURRENT_BINARY_DIR})
protobuf_generate(TARGET signal_server
    LANGUAGE cpp
    IMPORT_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/src
    PROTOC_OUT_DIR ${CMAKE_CURRENT_BINARY_DIR})
protobuf_generate(
    TARGET signal_server
    LANGUAGE grpc
    GENERATE_EXTENSIONS .grpc.pb.h .grpc.pb.cc
    PLUGIN "protoc-gen-grpc=${grpc_cpp_plugin_location}"
    IMPORT_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/src
    PROTOC_OUT_DIR ${CMAKE_CURRENT_BINARY_DIR})
