// tcp_echo_client.cpp
//
// Companion client for tcp_echo_server — the client from doc/getting_started.md
// (section 14), with timestamped log lines added. Runs ten connections
// concurrently; each sends five messages with random delays between them and
// forwards every reply over a channel to a single collector task, which writes
// them to results.txt.
//
// Usage:
//   ./tcp_echo_client [message [threads [host [port]]]]
//   ./tcp_echo_client                                  # "hello", all cores, 127.0.0.1 8080
//   ./tcp_echo_client hi 1                             # single-threaded
//   ./tcp_echo_client "hello pico" 0 192.168.1.42 8080 # connect to a Pico

#include <coro/coro.h>
#include <coro/runtime/runtime.h>
#include <coro/runtime/current_thread_executor.h>
#include <coro/runtime/work_stealing_executor.h>
#include <coro/io/file.h>
#include <coro/io/tcp_stream.h>
#include <coro/task/join_set.h>
#include <coro/sync/mpsc.h>
#include <coro/sync/sleep.h>
#include <coro/sync/timeout.h>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <format>
#include <random>
#include <stdexcept>
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
// run_client
//
// Connects to the echo server, sends five messages, and forwards each reply
// to the collector via the channel.  Each instance runs as an independent task
// — all N clients are in flight at once.
// ---------------------------------------------------------------------------
static Coro<void> run_client(int id, std::string host, uint16_t port, std::string message,
                             MpscSender<std::string> results) {
    struct Defer {
        Defer(int id) : id_(id) {}
        ~Defer() { LOG(id_, "Connection closed"); }
        int id_;
    } defer(id);

    TcpStream stream = co_await TcpStream::connect(host, port);
    LOG(id, "Connected");

    std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<int> delay(100, 2000);

    for (int i = 0; i < 5; ++i) {
        co_await sleep_for(std::chrono::milliseconds(delay(rng)));

        std::string msg = std::format("{} [conn={} seq={}]", message, id, i);
        LOG(id, "Sending: %s", msg.c_str());

        // variant<SelectBranch<0, pair<size_t,string>>, SelectBranch<1, void>>
        auto send = co_await timeout(2s, stream.write(std::move(msg)));
        if (send.index() != 0)
            throw std::runtime_error(std::format("conn {} send timed out", id));

        // variant<SelectBranch<0, pair<size_t,string>>, SelectBranch<1, void>>
        auto recv = co_await timeout(2s, stream.read(std::string(4096, '\0')));
        if (recv.index() != 0)
            throw std::runtime_error(std::format("conn {} receive timed out", id));

        auto& [n, reply] = std::get<0>(recv).value; // std::pair<size_t, std::string>
        reply.resize(n);
        LOG(id, "Echo: %s", reply.c_str());
        co_await results.send(std::move(reply));
    }
}

// ---------------------------------------------------------------------------
// collect_results
//
// Receives reply strings from all clients and writes them to a file in arrival
// order — one writer, one file handle, no locking.  Returns the number of
// replies written.
// ---------------------------------------------------------------------------
static Coro<int> collect_results(MpscReceiver<std::string> rx) {
    File file = co_await File::open(
        "results.txt",
        FileMode::Write | FileMode::Create | FileMode::Truncate);
    int count = 0;
    while (auto reply = co_await coro::next(rx)) { // std::optional<std::string>
        co_await file.write(std::move(*reply) + "\n");
        ++count;
    }
    co_return count;
}

// ---------------------------------------------------------------------------
// async_main
// ---------------------------------------------------------------------------
static Coro<int> async_main(std::string host, uint16_t port, std::string message) {
    constexpr int num_clients = 10;

    JoinSet<void> clients;

    // Spawn all clients, each with a cloned sender.  The original tx is dropped
    // at the end of the lambda so the channel closes when the last task-owned
    // clone is dropped — i.e. when every client exits one way or another.
    MpscReceiver<std::string> rx = [&] {
        auto [tx, rx] = mpsc_channel<std::string>(num_clients * 5);
        for (int i = 0; i < num_clients; ++i)
            clients.spawn(run_client(i, host, port, message, tx.clone()));
        return rx;  // tx dropped here — channel closes when all task-owned clones drop
    }();

    // Spawn the collector as a separate task with cancelOnDestroy(false).
    // If clients.drain() throws, every client has already finished and dropped its
    // sender, but replies may still be sitting in the channel. The collector is not
    // cancelled — it stays alive, flushes whatever is left, and completes once it
    // sees the channel closed. async_main cannot exit until the collector finishes.
    JoinHandle<int> collector = coro::spawn(collect_results(std::move(rx)));
    collector.cancelOnDestroy(false);

    // Waits for every client. If any threw, the first exception is rethrown here
    // once they have all finished.
    co_await clients.drain();

    int written = co_await collector;
    LOG(-1, "All done — %d replies written to results.txt", written);
    co_return 0;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
    std::string message = (argc > 1) ? argv[1] : "hello";
    int         threads = (argc > 2) ? std::stoi(argv[2]) : 0;
    std::string host    = (argc > 3) ? argv[3] : "127.0.0.1";
    uint16_t    port    = (argc > 4) ? static_cast<uint16_t>(std::stoi(argv[4])) : 8080;

    if (threads == 1) {
        // All 12+ tasks — clients, collector, file I/O — on one OS thread.
        Runtime rt(std::in_place_type<CurrentThreadExecutor>);
        return rt.block_on(async_main(std::move(host), port, std::move(message)));
    } else {
        // Same code, now distributed across N worker threads — nothing else changes.
        int n = threads > 1 ? threads : (int)std::thread::hardware_concurrency();
        Runtime rt(std::in_place_type<WorkStealingExecutor>, n);
        return rt.block_on(async_main(std::move(host), port, std::move(message)));
    }
}
