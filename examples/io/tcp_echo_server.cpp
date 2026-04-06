// tcp_echo_server.cpp
//
// Exposition example — TcpStream is not yet implemented. This file shows what
// the API is intended to look like once the I/O layer is complete. It does not
// compile today.
//
// Starts a TCP echo server on localhost:8080. For each incoming connection a
// task is spawned that reads bytes and writes them straight back until the
// client disconnects or the server is shut down.
//
// Usage:
//   ./tcp_echo_server
//   echo "hello" | nc localhost 8080

#include <coro/coro.h>
#include <coro/runtime/runtime.h>
#include <coro/task/join_set.h>
#include <coro/io/tcp_listener.h>   // not yet implemented
#include <coro/io/tcp_stream.h>     // not yet implemented
#include <array>
#include <cstdio>

using namespace coro;

// ---------------------------------------------------------------------------
// handle_connection
//
// Owns one TcpStream for the lifetime of a single client connection.
// Reads up to 4 KiB at a time and echoes it back until EOF or error.
// ---------------------------------------------------------------------------
static Coro<void> handle_connection(TcpStream stream) {
    std::array<std::byte, 4096> buf;

    for (;;) {
        // co_await stream.read() returns the number of bytes read, or 0 on EOF.
        std::size_t n = co_await stream.read(buf);
        if (n == 0)
            break;  // clean EOF — client disconnected

        // Echo the same bytes back. write() guarantees a full write or throws.
        co_await stream.write(std::span<const std::byte>(buf.data(), n));
    }
    // TcpStream destructor closes the socket when the coroutine returns.
}

// ---------------------------------------------------------------------------
// run_server
//
// Binds a TcpListener on localhost:8080 and accepts connections in a loop.
// Each connection is handed to handle_connection() via JoinSet so that all
// active sessions are cancelled cleanly when run_server itself is cancelled.
// ---------------------------------------------------------------------------
static Coro<void> run_server() {
    // TcpListener::bind() resolves the address and calls bind()/listen()
    // via the IoService — no blocking calls on the executor thread.
    TcpListener listener = co_await TcpListener::bind("127.0.0.1", 8080);
    std::printf("Listening on 127.0.0.1:8080\n");

    // JoinSet<void> tracks all active connection tasks. Dropping the JoinSet
    // (when run_server exits) cancels every in-flight session and waits for
    // them to drain before the coroutine completes.
    JoinSet<void> sessions;

    for (;;) {
        // co_await listener.accept() suspends until a new TCP connection
        // arrives; the I/O thread wakes this task via the Waker mechanism.
        TcpStream stream = co_await listener.accept();
        sessions.spawn(handle_connection(std::move(stream)));
    }

    // Unreachable in normal operation — the loop runs until the task is
    // cancelled (e.g. on SIGINT). The JoinSet destructor handles cleanup.
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main() {
    // WorkStealingExecutor with one thread per core.
    Runtime rt;
    rt.block_on(run_server());
}
