// tcp_echo_server.cpp
//
// TCP echo server on localhost:8080 — the server built up section by section in
// doc/getting_started.md (section 14), with timestamped log lines in place of the
// guide's bare printf calls. For each incoming connection a task is spawned that
// reads bytes and writes them straight back until the client disconnects, stalls,
// or the server is shut down with Ctrl-C.
//
// Usage:
//   ./tcp_echo_server [threads]     (1 = single-threaded, default = all cores)
//   nc 127.0.0.1 8080

#include <coro/coro.h>
#include <coro/runtime/runtime.h>
#include <coro/runtime/current_thread_executor.h>
#include <coro/runtime/work_stealing_executor.h>
#include <coro/io/signal.h>
#include <coro/io/tcp_listener.h>
#include <coro/io/tcp_stream.h>
#include <coro/task/join_set.h>
#include <coro/sync/select.h>
#include <coro/sync/timeout.h>
#include <coro/sync/when.h>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <ctime>
#include <exception>
#include <filesystem>
#include <string>
#include <thread>
#include <variant>

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
using namespace std::chrono_literals;

#define LOG(__ID__, __MSG__, ...) \
    std::printf("[%s] %s:%d %d - " __MSG__ "\n", \
        iso8601_now().c_str(), \
        std::filesystem::path(__FILE__).filename().string().c_str(), \
        __LINE__, __ID__, ##__VA_ARGS__)

// ---------------------------------------------------------------------------
// send_goodbye / FinalNotice
//
// Last message to a client before its connection closes. send_goodbye takes
// the stream by value — the goodbye task owns it. FinalNotice spawns it from a
// destructor so it runs on every exit path of handle_connection: EOF, timeout,
// or cancellation when the server shuts down.
// ---------------------------------------------------------------------------
static Coro<void> send_goodbye(TcpStream stream) {
    try {
        // Bounded: a stalled client must not be able to hold up shutdown.
        co_await timeout(1s, stream.write(std::string("goodbye\n")));
    } catch (const std::exception&) {
        // The peer is already gone — there is nobody left to say goodbye to.
    }
}

class FinalNotice {
public:
    FinalNotice(TcpStream& stream, int id) : m_stream(stream), m_id(id) {}
    ~FinalNotice() {
        LOG(m_id, "Connection closed");
        // Not cancelled when the handle is dropped; the enclosing coroutine
        // waits for it before its own completion becomes visible.
        coro::spawn(send_goodbye(std::move(m_stream))).cancelOnDestroy(false);
    }
private:
    TcpStream& m_stream;
    int        m_id;
};

// ---------------------------------------------------------------------------
// handle_connection
//
// Owns one TcpStream for the lifetime of a single client connection.
// Reads up to 4 KiB at a time and echoes it back until EOF, a stalled client,
// or cancellation.
// ---------------------------------------------------------------------------
static Coro<void> handle_connection(TcpStream stream, int id) {
    FinalNotice notice(stream, id);
    LOG(id, "Connected");
    for (;;) {
        auto recv = co_await timeout(20s, stream.read(std::string(4096, '\0')));
        if (recv.index() != 0) {
            LOG(id, "Receive timeout");
            co_return;
        }
        auto& [n, buf] = std::get<0>(recv).value;
        if (n == 0) {
            LOG(id, "EOF");
            co_return;  // clean close from client
        }
        buf.resize(n);
        LOG(id, "Received message \"%s\"", buf.c_str());
        auto send = co_await timeout(2s, stream.write(std::move(buf)));
        if (send.index() != 0) {
            LOG(id, "Send timeout");
            co_return;  // client stopped responding; part of the reply may be sent
        }
        LOG(id, "Echoed %zu bytes", n);
    }
}

// ---------------------------------------------------------------------------
// run_server
//
// Binds a TcpListener on localhost:8080 and accepts connections in a loop.
// Each connection is handed to handle_connection() via JoinSet so that all
// active sessions are cancelled cleanly when run_server itself is cancelled.
// ---------------------------------------------------------------------------
static Coro<int> run_server() {
    LOG(-1, "Starting TCP echo server...");
    TcpListener listener = co_await TcpListener::bind("127.0.0.1", 8080);
    LOG(-1, "TCP echo server listening on 127.0.0.1:8080");

    // JoinSet tracks all active sessions. Dropping it (on cancellation) cancels
    // every in-flight session and waits for them to drain.
    JoinSet<void> sessions;

    for (int i = 0;; ++i) {
        try {
            auto sel = co_await coro::select(
                listener.accept(),
                // Reap finished sessions (rethrows if one threw). Gated: next() on
                // an empty JoinSet resolves immediately and would busy-spin.
                coro::when(!sessions.empty(), [&] { return coro::next(sessions); })
            );
            if (sel.index() == 0) {
                TcpStream& stream = std::get<0>(sel).value;
                LOG(-1, "Accepted new connection %d", i);
                sessions.spawn(handle_connection(std::move(stream), i));
            }
        } catch (const std::exception& e) {
            LOG(-1, "Session error: %s — shutting down", e.what());
            co_return 1;
        }
    }
}

// ---------------------------------------------------------------------------
// async_main
//
// Runs the server until it exits on its own or SIGINT/SIGTERM arrives.
// ---------------------------------------------------------------------------
static Coro<int> async_main() {
    auto server_handle = coro::spawn(run_server());
    auto result = co_await coro::select(
        coro::ref(server_handle),
        coro::signal(SIGINT),
        coro::signal(SIGTERM)
    );
    if (result.index() == 0) {
        co_return std::get<0>(result).value;  // run_server() exited on its own
    }
    // Cancelling run_server() drops `sessions`, which cancels and drains every
    // connection still open before cancel_and_join() resolves. Each one sends its
    // goodbye on the way out, so shutdown takes at most the 1s send_goodbye() allows.
    LOG(-1, "Signal received — shutting down");
    co_return co_await std::move(server_handle).cancel_and_join();
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
    int threads = (argc > 1) ? std::stoi(argv[1]) : 0;

    LOG(-1, "Starting runtime...");
    if (threads == 1) {
        Runtime rt(std::in_place_type<CurrentThreadExecutor>);
        return rt.block_on(async_main());
    } else {
        int n = threads > 1 ? threads : (int)std::thread::hardware_concurrency();
        Runtime rt(std::in_place_type<WorkStealingExecutor>, n);
        return rt.block_on(async_main());
    }
}
