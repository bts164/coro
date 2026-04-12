add_executable(ws_echo_client ws_echo_client.cpp)
target_link_libraries(ws_echo_client PRIVATE coro)

add_executable(ws_echo_server ws_echo_server.cpp)
target_link_libraries(ws_echo_server PRIVATE coro)

add_executable(fibonacci fibonacci.cpp)
target_link_libraries(fibonacci PRIVATE coro)

add_executable(length_prefixed_stream length_prefixed_stream.cpp)
target_link_libraries(length_prefixed_stream PRIVATE coro)