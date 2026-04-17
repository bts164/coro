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
#include <chrono>
#include <cstdio>
#include <ctime>
#include <string>
#include <filesystem>

// Returns the current system time as an ISO 8601 string with milliseconds,
// e.g. "2026-04-06T21:34:56.123Z".
static std::string iso8601_now() {
    auto now = std::chrono::system_clock::now();
    auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(
                   now.time_since_epoch()) % 1000;
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", std::gmtime(&t));
    char result[40];
    std::snprintf(result, sizeof(result), "%s.%03dZ", buf, (int)ms.count());
    return result;
}

using namespace coro;

#define LOG(__ID__, __MSG__, ...) \
    std::printf("[%s] %s:%d %d - " __MSG__ "\n", \
        iso8601_now().c_str(), \
        std::filesystem::path(__FILE__).filename().string().c_str(), \
        __LINE__, __ID__, ##__VA_ARGS__)

// ---------------------------------------------------------------------------
// handle_connection
//
// Owns one WsStream for the lifetime of a single client connection. Receives
// messages and echoes them back with the same opcode until EOF or error.
// ---------------------------------------------------------------------------
static Coro<void> handle_connection(WsStream ws, int id) {
    using namespace std::chrono_literals;
    struct Defer {
        Defer(int id) : id_(id) {}
        ~Defer() { LOG(id_, "Connection closed"); }
        int id_;
    } defer(id);
    for (;;) {
        auto receiveResult = co_await coro::timeout(20s, ws.receive());
        if (0 != receiveResult.index()) {
            LOG(id, "Receive timeout");
            co_return;
        }
        WsStream::Message &msg = std::get<0>(receiveResult).value;
        //WsStream::Message msg = co_await ws.receive();
        if (msg.data.empty()) {
            LOG(id, "EOF");
            co_return;  // clean close from client
        }
        LOG(id, "Received message \"%s\"", std::string(msg.as_text()).c_str());
        auto sendResult = co_await coro::timeout(
            2s, ws.send(msg.data, msg.is_text ? WsStream::OpCode::Text
                                              : WsStream::OpCode::Binary));
        if (0 != sendResult.index()) {
            LOG(id, "Send timeout");
            co_return;  // client stopped responding
        }
        LOG(id, "Echoed message \"%s\"", std::string(msg.as_text()).c_str());
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
    LOG(-1, "Starting WebSocket echo server...");
    // WsListener::bind() registers a listening socket with lws via IoService.
    WsListener listener = co_await WsListener::bind("127.0.0.1", 9001);
    LOG(-1, "WebSocket echo server listening on ws://127.0.0.1:9001");

    // JoinSet tracks all active sessions. Dropping it (on cancellation) cancels
    // every in-flight session and waits for them to drain.
    JoinSet<void> sessions;

    for (int i = 0;; ++i) {
        // accept() suspends until a client completes the WebSocket handshake.
        WsStream ws = co_await listener.accept();
        LOG(-1, "Accepted new connection %d", i);
        sessions.spawn(handle_connection(std::move(ws), i));
    }
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main() {
    Runtime rt(1);
    LOG(-1, "Starting runtime...");
    rt.block_on(run_server());
}
