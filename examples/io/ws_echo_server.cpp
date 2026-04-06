// ws_echo_server.cpp
//
// WebSocket echo server on ws://localhost:9001. For each incoming connection a
// task is spawned that reads messages and writes them straight back until the
// client disconnects or the server is shut down.
//
// Usage:
//   ./ws_echo_server
//   (then run ./ws_echo_client in another terminal)

#include <coro/coro.h>
#include <coro/runtime/runtime.h>
#include <coro/task/join_set.h>
#include <coro/io/ws_listener.h>
#include <coro/io/ws_stream.h>
#include <coro/sync/timeout.h>
#include <cstdio>

using namespace coro;

// ---------------------------------------------------------------------------
// handle_connection
//
// Owns one WsStream for the lifetime of a single client connection. Receives
// messages and echoes them back with the same opcode until EOF or error.
// ---------------------------------------------------------------------------
static Coro<void> handle_connection(WsStream ws, size_t id) {
    using namespace std::chrono_literals;
    struct Defer {
        Defer(size_t id) : id_(id) {}
        ~Defer() { std::printf("%lu: Connection closed\n", id_); }
        size_t id_;
    } defer(id);
    for (;;) {
        auto receiveResult = co_await coro::timeout(20s, ws.receive());
        if (0 != receiveResult.index()) {
            std::printf("%lu: Receive timeout\n", id);
            co_return;
        }
        WsStream::Message &msg = std::get<0>(receiveResult).value;

        if (msg.data.empty()) {
            std::printf("%lu: EOF\n", id);
            co_return;  // clean close from client
        }
        std::printf("%lu: Received message (%zu bytes, %s)\n",
            id, msg.data.size(),
            msg.is_text ? "text" : "binary");
        auto sendResult = co_await coro::timeout(
            2s, ws.send(msg.data, msg.is_text ? WsStream::OpCode::Text
                                              : WsStream::OpCode::Binary));
        if (0 != sendResult.index()) {
            std::printf("%lu: Send timeout\n", id);
            co_return;  // client stopped responding
        }
        std::printf("%lu: Echoed message\n", id);
    }
    // WsStream destructor sends a Close frame when the coroutine returns.
}

// ---------------------------------------------------------------------------
// run_server
//
// Binds a WsListener on localhost:9001 and accepts connections in a loop.
// All active sessions are owned by a JoinSet so they are cancelled cleanly
// when run_server exits.
// ---------------------------------------------------------------------------
static Coro<void> run_server() {
    std::printf("Starting WebSocket echo server...\n");
    // WsListener::bind() registers a listening socket with lws via IoService.
    WsListener listener = co_await WsListener::bind("127.0.0.1", 9001);
    std::printf("WebSocket echo server listening on ws://127.0.0.1:9001\n");

    // JoinSet tracks all active sessions. Dropping it (on cancellation) cancels
    // every in-flight session and waits for them to drain.
    JoinSet<void> sessions;

    for (size_t i = 0;; ++i) {
        // accept() suspends until a client completes the WebSocket handshake.
        WsStream ws = co_await listener.accept();
        std::printf("Accepted new connection %lu\n", i);
        sessions.spawn(handle_connection(std::move(ws), i));
    }
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main() {
    Runtime rt;
    std::printf("Starting runtime...\n");
    rt.block_on(run_server());
}
