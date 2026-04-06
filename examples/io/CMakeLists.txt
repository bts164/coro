add_executable(ws_echo_client ws_echo_client.cpp)
target_link_libraries(ws_echo_client PRIVATE coro)

add_executable(ws_echo_server ws_echo_server.cpp)
target_link_libraries(ws_echo_server PRIVATE coro)
