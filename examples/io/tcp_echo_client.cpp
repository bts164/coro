// tcp_echo_client.cpp
//
// Connects to a TCP echo server, sends messages, and prints the replies.
//
// Usage:
//   ./tcp_echo_client [host [port [message]]]
//   ./tcp_echo_client 192.168.1.42 8080 "hello pico"   # connect to Pico
//   ./tcp_echo_client                                   # defaults: 127.0.0.1 8080

#include <coro/coro.h>
#include <coro/runtime/runtime.h>
#include <coro/io/tcp_stream.h>
#include <coro/sync/sleep.h>
#include <coro/sync/timeout.h>
#include <coro/task/join_set.h>

#include <chrono>
#include <cstdio>
#include <ctime>
#include <format>
#include <filesystem>
#include <random>
#include <string>

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
// run_client
// ---------------------------------------------------------------------------
static Coro<void> run_client(int id, std::string host, uint16_t port, std::string message) {
    using namespace std::chrono_literals;
    struct Defer {
        Defer(int id) : id_(id) {}
        ~Defer() { LOG(id_, "Connection %d closed", id_); }
        int id_;
    } defer(id);

    TcpStream stream = co_await TcpStream::connect(host, port);

    LOG(id, "Connected. Sending: %.*s", static_cast<int>(message.size()), message.data());

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<size_t> distr(100, 10000);

    for (size_t i = 0; i < 5; ++i) {
        size_t delay = distr(gen);
        LOG(id, "Sleeping for %zu ms...", delay);
        co_await coro::sleep_for(std::chrono::milliseconds(delay));

        std::string msg = std::format("{} from {} iteration {}", message, id, i);
        LOG(id, "Sending: %s", msg.c_str());
        // write() takes ownership of the buffer; returns it on completion.
        auto sendResult = co_await coro::timeout(2s, stream.write(std::move(msg)));
        if (0 != sendResult.index()) {
            LOG(id, "Send timeout");
            co_return;
        }

        // read() takes ownership of the buffer; returns {bytes_read, buffer}.
        auto receiveResult = co_await coro::timeout(2s, stream.read(std::string(4096, '\0')));
        if (0 != receiveResult.index()) {
            LOG(id, "Receive timeout");
            co_return;
        }
        auto [n, reply] = std::move(std::get<0>(receiveResult).value);
        reply.resize(n);
        LOG(id, "Echo: %s", reply.c_str());
    }

    // TcpStream destructor closes the socket asynchronously.
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
coro::Coro<int> async_main(int argc, char* argv[]) {
    std::string host    = (argc > 1) ? argv[1] : "127.0.0.1";
    uint16_t    port    = (argc > 2) ? static_cast<uint16_t>(std::stoi(argv[2])) : 8080;
    std::string message = (argc > 3) ? argv[3] : "hello, world";
    coro::JoinSet<void> clients;
    for (int i = 0; i < 10; ++i) {
        clients.spawn(run_client(i, host, port, message));
    }
    co_await clients.drain();
    LOG(-1, "All clients completed");
    co_return 0;
}

int main(int argc, char* argv[]) {
    Runtime rt;
    return rt.block_on(async_main(argc, argv));
}
