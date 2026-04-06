// ws_echo_client.cpp
//
// Connects to the WebSocket echo server on ws://localhost:9001, sends a
// message, receives the echo, and prints it.
//
// This example exercises the fully implemented WsStream client path and
// should compile and run once Phase 3 is complete.
//
// Usage:
//   ./ws_echo_client [message]
//   ./ws_echo_client "hello, world"

#include <coro/coro.h>
#include <coro/runtime/runtime.h>
#include <coro/io/ws_stream.h>
#include <coro/sync/sleep.h>
#include <coro/sync/timeout.h>
#include <cstdio>
#include <random>
#include <string_view>
#include <vector>
#include <span>
#include <string_view>

using namespace coro;

// ---------------------------------------------------------------------------
// run_client
//
// Connects to the local echo server, sends one message, receives the echo,
// and prints it. Demonstrates the full connect → send → receive → close cycle.
// ---------------------------------------------------------------------------
static Coro<void> run_client(size_t id, std::string_view message) {
    using namespace std::chrono_literals;
    // connect() parses the URL, submits a WsConnectRequest to IoService, and
    // suspends until lws fires LWS_CALLBACK_CLIENT_ESTABLISHED.
    WsStream ws = co_await WsStream::connect("ws://127.0.0.1:9001/");

    std::printf("%lu: Connected. Sending: %.*s\n", id,
                static_cast<int>(message.size()), message.data());
    struct Defer {
        Defer(size_t id) : id_(id) {}
        ~Defer() { std::printf("%lu: Connection closed\n", id_); }
        size_t id_;
    } defer(id);

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<size_t> distr(100, 10000);

    for (size_t i = 0; i < 5; ++i) {
        size_t delay = distr(gen);
        std::printf("%lu: Sleeping for %zu ms...\n", id, delay);
        co_await coro::sleep_for(std::chrono::milliseconds(delay));

        std::printf("%lu: Sending: %.*s\n", id,
                    static_cast<int>(message.size()), message.data());
        // send() enqueues the message and suspends until LWS_CALLBACK_CLIENT_WRITEABLE
        // fires and lws_write() completes. The string_view overload sends a text frame.
        auto sendResult = co_await coro::timeout(2s, ws.send(message));
        if (0 != sendResult.index()) {
            std::printf("%lu: Send timeout\n", id);
            co_return;
        }

        // receive() suspends until LWS_CALLBACK_CLIENT_RECEIVE assembles the full
        // message (FrameMode::Full is the default).
        auto receiveResult = co_await coro::timeout(2s, ws.receive());
        if (0 != receiveResult.index()) {
            std::printf("%lu: Receive timeout\n", id);
            co_return;
        }
        WsStream::Message &reply = std::get<0>(receiveResult).value;

        std::string_view text(reinterpret_cast<const char*>(reply.data.data()),
                              reply.data.size());
        std::printf("%lu: Echo:      %.*s\n", id,
                    static_cast<int>(text.size()), text.data());
    }

    // WsStream destructor submits a WsCloseRequest, sending a Close frame.
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
coro::Coro<int> async_main(int argc, char* argv[]) {
    std::string_view message = (argc > 1) ? argv[1] : "hello, world";
    coro::JoinSet<void> clients;
    for (size_t i = 0; i < 1; ++i) {
        clients.spawn(run_client(i, message));
    }
    co_await clients.drain();
    co_return 0;
}

int main(int argc, char* argv[]) {
    Runtime rt;
    return rt.block_on(async_main(argc, argv));
}
