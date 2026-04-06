// tcp_echo_client.cpp
//
// Exposition example — TcpStream is not yet implemented. This file shows what
// the API is intended to look like once the I/O layer is complete. It does not
// compile today.
//
// Connects to the echo server on localhost:8080, sends a message, reads the
// echo back, and prints it.
//
// Usage:
//   ./tcp_echo_client [message]
//   ./tcp_echo_client "hello, world"

#include <coro/coro.h>
#include <coro/runtime/runtime.h>
#include <coro/io/tcp_stream.h>   // not yet implemented
#include <array>
#include <cstdio>
#include <cstring>
#include <span>
#include <string_view>

using namespace coro;

// ---------------------------------------------------------------------------
// run_client
//
// Connects to localhost:8080, sends `message`, reads the echo, and prints it.
// ---------------------------------------------------------------------------
static Coro<void> run_client(std::string_view message) {
    // TcpStream::connect() resolves the address and performs the TCP handshake
    // asynchronously — suspends until connected or throws on failure.
    TcpStream stream = co_await TcpStream::connect("127.0.0.1", 8080);

    // Send the message. write() guarantees the full span is written or throws.
    auto bytes = std::as_bytes(std::span(message.data(), message.size()));
    co_await stream.write(bytes);

    // Read the echo back. The server reflects exactly what we sent, so one
    // read() is sufficient as long as the message fits in the buffer.
    std::array<std::byte, 4096> buf;
    std::size_t n = co_await stream.read(buf);

    std::string_view response(reinterpret_cast<const char*>(buf.data()), n);
    std::printf("echo: %.*s\n", static_cast<int>(response.size()), response.data());

    // TcpStream destructor closes the socket and sends FIN when the coroutine returns.
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
    std::string_view message = (argc > 1) ? argv[1] : "hello, world";

    Runtime rt;
    rt.block_on(run_client(message));
}
