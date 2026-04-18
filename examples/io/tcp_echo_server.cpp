// tcp_echo_server.cpp
//
// TCPt echo server on localhost:8080. For each incoming connection a
// task is spawned that reads bytes and writes them straight back until the
// client disconnects or the server is shut down.
//
// Usage:
//   ./tcp_echo_server
//   echo "hello" | nc localhost 8080

#include <coro/coro.h>
#include <coro/runtime/runtime.h>
#include <coro/task/join_set.h>
#include <coro/io/tcp_listener.h>
#include <coro/io/tcp_stream.h>
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
// Owns one TcpStream for the lifetime of a single client connection.
// Reads up to 4 KiB at a time and echoes it back until EOF or error.
// ---------------------------------------------------------------------------
static Coro<void> handle_connection(TcpStream stream, int id) {
    using namespace std::chrono_literals;
    struct Defer {
        Defer(int id) : id_(id) {}
        ~Defer() { LOG(id_, "Connection closed"); }
        int id_;
    } defer(id);
    for (;;) {
        auto receiveResult = co_await coro::timeout(20s, stream.read(std::string(4096, '\0')));
        if (0 != receiveResult.index()) {
            LOG(id, "Receive timeout");
            co_return;
        }
        auto [n, buf] = std::move(std::get<0>(receiveResult).value);
        if (n == 0) {
            LOG(id, "EOF");
            co_return;  // clean close from client
        }
        buf.resize(n);
        LOG(id, "Received message \"%s\"", buf.c_str());
        auto sendResult = co_await coro::timeout(2s, stream.write(std::move(buf)));
        if (0 != sendResult.index()) {
            LOG(id, "Send timeout");
            co_return;  // client stopped responding
        }
        LOG(id, "Echoed message \"%s\"", std::get<0>(sendResult).value.c_str());
    }
}

// ---------------------------------------------------------------------------
// run_server
//
// Binds a TcpListener on localhost:8080 and accepts connections in a loop.
// Each connection is handed to handle_connection() via JoinSet so that all
// active sessions are cancelled cleanly when run_server itself is cancelled.
// ---------------------------------------------------------------------------
static Coro<void> run_server() {
    LOG(-1, "Starting TCP echo server...");
    TcpListener listener = co_await TcpListener::bind("127.0.0.1", 8080);
    LOG(-1, "TCP echo server listening on 127.0.0.1:8080");

    // JoinSet tracks all active sessions. Dropping it (on cancellation) cancels
    // every in-flight session and waits for them to drain.
    JoinSet<void> sessions;

    for (int i = 0;; ++i) {
        TcpStream stream = co_await listener.accept();
        LOG(-1, "Accepted new connection %d", i);
        sessions.spawn(handle_connection(std::move(stream), i));
    }
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main() {
    Runtime rt;
    LOG(-1, "Starting runtime...");
    rt.block_on(run_server());
}
