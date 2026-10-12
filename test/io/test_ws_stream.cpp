// WsStream / WsListener, with lws serviced by the Runtime's IoDriver
// (doc/design/websocket_stream.md). Each test uses its own port in 30201-30299.

#include <gtest/gtest.h>
#include <coro/io/lookup_host.h>
#include <coro/io/tcp_listener.h>
#include <coro/io/tcp_stream.h>
#include <coro/io/ws_listener.h>
#include <coro/io/ws_stream.h>
#include <coro/runtime/current_thread_executor.h>
#include <coro/runtime/parker.h>
#include <coro/runtime/runtime.h>
#include <coro/sync/join.h>
#include <coro/sync/sleep.h>
#include <coro/sync/timeout.h>
#include <coro/coro.h>
#include <coro/future.h>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

using namespace coro;
using namespace std::chrono_literals;

namespace {

// A listener plus both ends of one connection to it.
struct Connection {
    WsListener listener;
    WsStream   server;
    WsStream   client;
};

// Both ends of one connection to a listener the caller keeps.
struct Ends {
    WsStream server;
    WsStream client;
};

Coro<Ends> connect_to(WsListener& listener, uint16_t port, WsStream::Options options = {}) {
    auto [server, client] = co_await join(
        listener.accept(),
        WsStream::connect("ws://127.0.0.1:" + std::to_string(port) + "/", std::move(options)));
    co_return Ends{std::move(server), std::move(client)};
}

Coro<Connection> connect_pair(uint16_t port, WsListener::Options options = {},
                              WsStream::Options client_options = {}) {
    WsListener listener = co_await WsListener::bind("127.0.0.1", port, std::move(options));
    Ends ends = co_await connect_to(listener, port, std::move(client_options));
    co_return Connection{std::move(listener), std::move(ends.server), std::move(ends.client)};
}

// Receives one message, failing the test instead of hanging if none arrives within 2 s.
Coro<WsStream::Message> receive(WsStream& ws) {
    auto result = co_await timeout(2s, ws.receive());
    if (result.index() != 0) throw std::runtime_error("receive timed out");
    co_return std::move(std::get<0>(result).value);
}

// Accepts one connection, failing the test instead of hanging if none arrives within 2 s.
Coro<WsStream> accept(WsListener& listener) {
    auto result = co_await timeout(2s, listener.accept());
    if (result.index() != 0) throw std::runtime_error("accept timed out");
    co_return std::move(std::get<0>(result).value);
}

std::string text(const WsStream::Message& msg) { return std::string(msg.as_text()); }

std::string url(uint16_t port, const std::string& host = "127.0.0.1") {
    return "ws://" + host + ":" + std::to_string(port) + "/";
}

// Awaits connect(url) and returns the error it threw (a default error_code if none).
Coro<std::error_code> connect_error(std::string to, WsStream::Options options = {}) {
    try {
        auto result = co_await timeout(2s, WsStream::connect(std::move(to), std::move(options)));
        if (result.index() != 0) throw std::runtime_error("connect timed out");
    } catch (const std::system_error& e) {
        co_return e.code();
    }
    co_return std::error_code{};
}

// Awaits bind(host, port), keeping the listener only until it returns, and returns
// the error it threw (a default error_code if none).
Coro<std::error_code> bind_error(std::string host, uint16_t port) {
    try {
        WsListener listener = co_await WsListener::bind(std::move(host), port);
    } catch (const std::system_error& e) {
        co_return e.code();
    }
    co_return std::error_code{};
}

// Binds `port` as soon as it is free, and returns the last error if that takes more
// than 2 s. A dropped listener's port is released by a later turn of the driver,
// not by the drop itself.
Coro<std::error_code> bind_when_free(uint16_t port) {
    std::error_code code;
    for (int i = 0; i < 200; ++i) {
        code = co_await bind_error("127.0.0.1", port);
        if (!code) break;
        co_await sleep_for(10ms);
    }
    co_return code;
}

// One message each way, each within 2 s.
Coro<void> round_trip(WsStream& client, WsStream& server, std::string word) {
    co_await client.send(word);
    EXPECT_EQ(text(co_await receive(server)), word);
    co_await server.send(word);
    EXPECT_EQ(text(co_await receive(client)), word);
}

// What receive() on a closed connection throws, or "" if it returned a message.
Coro<std::string> receive_error(WsStream& ws) {
    try {
        (void)co_await receive(ws);
    } catch (const std::runtime_error& e) {
        co_return std::string(e.what());
    }
    co_return std::string();
}

// Waits on the server end forever; the Runtime destroys it at shutdown.
Coro<void> hold(Connection c) {
    (void)co_await c.server.receive();
}

// `size` bytes that differ from one 4096-byte frame to the next, so a frame lost,
// repeated or out of place shows.
std::vector<std::byte> pattern(std::size_t size) {
    std::vector<std::byte> bytes(size);
    for (std::size_t i = 0; i < size; ++i) bytes[i] = static_cast<std::byte>((i * 31 + (i >> 12)) & 0xff);
    return bytes;
}

// One message as a Partial-mode connection delivers it.
struct Fragments {
    std::vector<std::byte> data;         // the fragments' payloads, joined
    int                    count = 0;    // fragments, the final one included
    int                    text  = 0;    // those marked as text
};

// Receives fragments up to and including the final one of a message.
Coro<Fragments> receive_fragments(WsStream& ws) {
    Fragments fragments;
    for (;;) {
        WsStream::Message msg = co_await receive(ws);
        fragments.data.insert(fragments.data.end(), msg.data.begin(), msg.data.end());
        ++fragments.count;
        if (msg.is_text) ++fragments.text;
        if (msg.is_final) break;
    }
    co_return fragments;
}

// What receive() fails with, or a default error_code if it returned a message.
Coro<std::error_code> receive_code(WsStream& ws) {
    try {
        (void)co_await receive(ws);
    } catch (const std::system_error& e) {
        co_return e.code();
    }
    co_return std::error_code{};
}

// Sends `count` numbered messages, each awaited before the next.
Coro<void> send_many(WsStream& ws, std::string prefix, int count) {
    for (int i = 0; i < count; ++i) {
        const std::string word = prefix + std::to_string(i);
        co_await ws.send(word);
    }
}

// Receives `count` numbered messages and checks that none is missing or out of order.
Coro<void> receive_many(WsStream& ws, std::string prefix, int count) {
    for (int i = 0; i < count; ++i) {
        EXPECT_EQ(text(co_await receive(ws)), prefix + std::to_string(i));
    }
}

// One end of a connection sending and receiving at once.
Coro<void> talk(WsStream ws, std::string mine, std::string theirs, int count) {
    (void)co_await join(send_many(ws, mine, count), receive_many(ws, theirs, count));
}

// One task's share of a stream that several tasks use. WsStream has no split or
// clone, so tasks that share one share the object, and here they own it together.
using Shared = std::shared_ptr<WsStream>;

Coro<void> send_task(Shared ws, std::string prefix, int count) {
    co_await send_many(*ws, std::move(prefix), count);
}

Coro<void> receive_task(Shared ws, std::string prefix, int count) {
    co_await receive_many(*ws, std::move(prefix), count);
}

Coro<std::string> receive_error_task(Shared ws) {
    co_return co_await receive_error(*ws);
}

// Sends every millisecond until a send fails; false if none did in about 2 s.
Coro<bool> send_until_closed(Shared ws) {
    for (int i = 0; i < 2000; ++i) {
        try {
            co_await ws->send("tick");
        } catch (const std::runtime_error&) {
            co_return true;
        }
        co_await sleep_for(1ms);
    }
    co_return false;
}

// `count` messages each way on one connection, from four tasks: each end has one
// task that only sends and another that only receives.
Coro<void> send_and_receive_in_separate_tasks(uint16_t port, int count) {
    Connection c = co_await connect_pair(port);
    Shared server = std::make_shared<WsStream>(std::move(c.server));
    Shared client = std::make_shared<WsStream>(std::move(c.client));

    auto server_sends    = spawn(send_task(server, "s", count));
    auto server_receives = spawn(receive_task(server, "c", count));
    auto client_sends    = spawn(send_task(client, "c", count));
    auto client_receives = spawn(receive_task(client, "s", count));

    co_await std::move(server_sends);
    co_await std::move(server_receives);
    co_await std::move(client_sends);
    co_await std::move(client_receives);
}

// Reads from a plain TCP connection up to the blank line that ends an HTTP header
// block, failing the test instead of hanging if 2 s pass without data.
Coro<std::string> read_headers(TcpStream& tcp) {
    std::string headers;
    while (headers.find("\r\n\r\n") == std::string::npos) {
        auto result = co_await timeout(2s, tcp.read(std::string(1024, '\0')));
        if (result.index() != 0) throw std::runtime_error("read_headers: timed out");
        auto& [n, buf] = std::get<0>(result).value;
        if (n == 0) throw std::runtime_error("read_headers: connection closed");
        headers.append(buf.data(), n);
    }
    co_return headers;
}

// A peer that speaks WebSocket by hand, for what a WsStream can't be made to do: a
// plain TCP client that sends a canned upgrade request (the key is the one from
// RFC 6455) and takes the server's answer on trust.
Coro<TcpStream> raw_client(uint16_t port) {
    TcpStream tcp = co_await TcpStream::connect("127.0.0.1", port);
    co_await tcp.write(std::string("GET / HTTP/1.1\r\n"
                                   "Host: 127.0.0.1\r\n"
                                   "Upgrade: websocket\r\n"
                                   "Connection: Upgrade\r\n"
                                   "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                                   "Sec-WebSocket-Version: 13\r\n"
                                   "\r\n"));
    (void)co_await read_headers(tcp);
    co_return tcp;
}

// One client frame with the FIN bit, `opcode` and a two-byte payload. The mask is
// all zeros, which leaves the payload as written.
std::string raw_frame(unsigned char opcode, char first, char second) {
    return std::string{static_cast<char>(0x80 | opcode), static_cast<char>(0x82), 0, 0, 0, 0,
                       first, second};
}

// A TCP server that is not a WebSocket server. Takes one connection; with a reply,
// reads the request and answers with it; then holds the connection for `hold` and
// drops it.
Coro<void> tcp_peer(TcpListener listener, std::optional<std::string> reply,
                    std::chrono::milliseconds hold) {
    auto accepted = co_await timeout(2s, listener.accept());
    if (accepted.index() != 0) throw std::runtime_error("tcp_peer: no connection");
    TcpStream tcp = std::move(std::get<0>(accepted).value);
    if (reply) {
        (void)co_await read_headers(tcp);
        co_await tcp.write(std::move(*reply));
    }
    co_await sleep_for(hold);
}

// Runs fn(ctx) inside one poll of the calling task, for tests that poll a send by
// hand. On a single-thread Runtime the thread polling the task is the one that turns
// the driver, so no lws callback runs while fn does: a send that fn polls is still
// queued, not written, for as long as fn runs.
template<typename Fn>
class InOnePoll {
public:
    using OutputType = void;
    explicit InOnePoll(Fn fn) : m_fn(std::move(fn)) {}
    PollResult<void> poll(detail::Context& ctx) {
        m_fn(ctx);
        return PollReady;
    }

private:
    Fn m_fn;
};

// Awaits a send where it is. A SendFuture that has been polled can't be moved, so it
// can't be handed to co_await itself.
struct InPlace {
    using OutputType = void;
    WsStream::SendFuture* m_send;
    PollResult<void> poll(detail::Context& ctx) { return m_send->poll(ctx); }
};

// Awaits a send that has been polled already and returns the error it failed with (a
// default error_code if it was sent), failing the test instead of hanging if it has
// not completed within 2 s.
Coro<std::error_code> finish(WsStream::SendFuture& send) {
    try {
        auto result = co_await timeout(2s, InPlace{&send});
        if (result.index() != 0) throw std::runtime_error("send timed out");
    } catch (const std::system_error& e) {
        co_return e.code();
    }
    co_return std::error_code{};
}

// Queues "a", "b" and "c" before lws can write any of them, drops the one at
// `position`, and checks that the peer gets the other two in order and nothing else.
// Needs a single-thread Runtime (see InOnePoll).
Coro<void> drop_one_of_three(uint16_t port, std::size_t position) {
    Connection c = co_await connect_pair(port);

    const std::array<std::string, 3> words{"a", "b", "c"};
    std::array<std::optional<WsStream::SendFuture>, 3> sends;
    for (std::size_t i = 0; i < sends.size(); ++i) sends[i].emplace(c.client.send(words[i]));

    co_await InOnePoll([&](detail::Context& ctx) {
        for (auto& send : sends) EXPECT_TRUE(send->poll(ctx).isPending());
        sends[position].reset();
    });
    for (auto& send : sends) {
        if (send) {
            EXPECT_FALSE(co_await finish(*send));
        }
    }
    co_await c.client.send("end");

    for (std::size_t i = 0; i < words.size(); ++i) {
        if (i != position) {
            EXPECT_EQ(text(co_await receive(c.server)), words[i]);
        }
    }
    EXPECT_EQ(text(co_await receive(c.server)), "end");
}

}  // namespace

// ---------------------------------------------------------------------------
// Basic send / receive
// ---------------------------------------------------------------------------

TEST(WsStreamTest, TextRoundTripsBothWays) {
    Runtime rt;
    rt.block_on([]() -> Coro<void> {
        Connection c = co_await connect_pair(30201);

        co_await c.client.send("ping");
        auto ping = co_await receive(c.server);
        EXPECT_TRUE(ping.is_text);
        EXPECT_TRUE(ping.is_final);
        EXPECT_EQ(text(ping), "ping");

        co_await c.server.send("pong");
        EXPECT_EQ(text(co_await receive(c.client)), "pong");
    }());
}

TEST(WsStreamTest, BinaryMessageIsNotText) {
    Runtime rt;
    rt.block_on([]() -> Coro<void> {
        Connection c = co_await connect_pair(30202);

        const std::vector<std::byte> payload{std::byte{0x00}, std::byte{0xff}, std::byte{0x42}};
        co_await c.client.send(payload, WsStream::OpCode::Binary);
        auto msg = co_await receive(c.server);
        EXPECT_FALSE(msg.is_text);
        EXPECT_EQ(msg.data, payload);
        EXPECT_THROW((void)msg.as_text(), std::logic_error);
    }());
}

// ---------------------------------------------------------------------------
// Receive queue
// ---------------------------------------------------------------------------

// Messages that arrive before the previous one is received used to be appended into it.
TEST(WsStreamTest, BackToBackMessagesStaySeparate) {
    Runtime rt;
    rt.block_on([]() -> Coro<void> {
        Connection c = co_await connect_pair(30203);

        co_await c.client.send("one");
        co_await c.client.send("two");
        co_await c.client.send("three");
        co_await sleep_for(100ms);  // Let all three arrive before the first receive().

        EXPECT_EQ(text(co_await receive(c.server)), "one");
        EXPECT_EQ(text(co_await receive(c.server)), "two");
        EXPECT_EQ(text(co_await receive(c.server)), "three");
    }());
}

// Same, in the client's receive path.
TEST(WsStreamTest, BackToBackMessagesStaySeparateOnClient) {
    Runtime rt;
    rt.block_on([]() -> Coro<void> {
        Connection c = co_await connect_pair(30204);

        co_await c.server.send("one");
        co_await c.server.send("two");
        co_await sleep_for(100ms);

        EXPECT_EQ(text(co_await receive(c.client)), "one");
        EXPECT_EQ(text(co_await receive(c.client)), "two");
    }());
}

// A receive() dropped before it resolves (here, by a timeout) used to leave the connection
// discarding every message after it.
TEST(WsStreamTest, DroppedReceiveLosesNothing) {
    Runtime rt;
    rt.block_on([]() -> Coro<void> {
        Connection c = co_await connect_pair(30205);

        auto timed_out = co_await timeout(50ms, c.server.receive());
        EXPECT_EQ(timed_out.index(), 1u);

        co_await c.client.send("a");
        co_await c.client.send("b");
        co_await sleep_for(100ms);  // Arrive while no receive() is pending.
        EXPECT_EQ(text(co_await receive(c.server)), "a");
        EXPECT_EQ(text(co_await receive(c.server)), "b");

        (void)co_await timeout(50ms, c.server.receive());
        co_await c.client.send("c");
        EXPECT_EQ(text(co_await receive(c.server)), "c");
    }());
}

// ---------------------------------------------------------------------------
// max_message_size
// ---------------------------------------------------------------------------

TEST(WsStreamTest, OversizedMessageThrowsAndConnectionRecovers) {
    Runtime rt;
    rt.block_on([]() -> Coro<void> {
        Connection c = co_await connect_pair(30206, {.max_message_size = 8});

        co_await c.client.send(std::string(32, 'x'));
        co_await c.client.send("ok");

        bool threw = false;
        try {
            (void)co_await receive(c.server);
        } catch (const std::system_error& e) {
            threw = true;
            EXPECT_EQ(e.code().value(), EMSGSIZE);
        }
        EXPECT_TRUE(threw);
        EXPECT_EQ(text(co_await receive(c.server)), "ok");
    }());
}

// The same limit on the client end, which takes it from WsStream::Options.
TEST(WsStreamTest, OversizedMessageThrowsOnClient) {
    Runtime rt;
    rt.block_on([]() -> Coro<void> {
        Connection c = co_await connect_pair(30245, {}, {.max_message_size = 8});

        co_await c.server.send(std::string(32, 'x'));
        co_await c.server.send("ok");

        const std::error_code code = co_await receive_code(c.client);
        EXPECT_EQ(code.value(), EMSGSIZE) << code.message();
        EXPECT_EQ(text(co_await receive(c.client)), "ok");
    }());
}

// A message that passes the limit only in a later frame: what was buffered of it is
// dropped, the frames still to come are discarded, and one error stands in for it.
TEST(WsStreamTest, OversizedMessageAcrossFramesThrowsOnce) {
    Runtime rt;
    rt.block_on([]() -> Coro<void> {
        Connection c = co_await connect_pair(30246, {.max_message_size = 10'000});

        const std::vector<std::byte> payload = pattern(20'000);
        co_await c.client.send(payload);
        co_await c.client.send("ok");

        const std::error_code code = co_await receive_code(c.server);
        EXPECT_EQ(code.value(), EMSGSIZE) << code.message();
        EXPECT_EQ(text(co_await receive(c.server)), "ok");
    }());
}

// ---------------------------------------------------------------------------
// Message size
// ---------------------------------------------------------------------------

// A message of many 4096-byte frames (max_frame_size) reaches receive() as one
// message, on either end.
TEST(WsStreamTest, MessageOfManyFramesIsReassembled) {
    Runtime rt;
    rt.block_on([]() -> Coro<void> {
        Connection c = co_await connect_pair(30240);
        const std::vector<std::byte> payload = pattern(100'000);

        co_await c.client.send(payload);
        auto at_server = co_await receive(c.server);
        EXPECT_FALSE(at_server.is_text);
        EXPECT_TRUE(at_server.is_final);
        EXPECT_EQ(at_server.data.size(), payload.size());
        EXPECT_TRUE(at_server.data == payload);

        co_await c.server.send(payload);
        auto at_client = co_await receive(c.client);
        EXPECT_EQ(at_client.data.size(), payload.size());
        EXPECT_TRUE(at_client.data == payload);
    }());
}

// A message larger than the socket's send buffer: lws_write() can't hand all of it
// to the kernel and keeps the rest to send later. The send after it must wait its
// turn behind that.
TEST(WsStreamTest, MessageLargerThanSocketBufferIsSent) {
    Runtime rt;
    rt.block_on([]() -> Coro<void> {
        Connection c = co_await connect_pair(30241);
        const std::vector<std::byte> payload = pattern(8 * 1024 * 1024);

        co_await c.client.send(payload);
        co_await c.client.send("after");

        auto result = co_await timeout(20s, c.server.receive());
        if (result.index() != 0) throw std::runtime_error("receive timed out");
        const WsStream::Message& big = std::get<0>(result).value;
        EXPECT_EQ(big.data.size(), payload.size());
        EXPECT_TRUE(big.data == payload);
        EXPECT_EQ(text(co_await receive(c.server)), "after");
    }());
}

// A message with no payload is still a message.
TEST(WsStreamTest, EmptyMessageRoundTrips) {
    Runtime rt;
    rt.block_on([]() -> Coro<void> {
        Connection c = co_await connect_pair(30242);

        co_await c.client.send("");
        auto at_server = co_await receive(c.server);
        EXPECT_TRUE(at_server.is_text);
        EXPECT_TRUE(at_server.data.empty());

        co_await c.server.send(std::span<const std::byte>{});
        auto at_client = co_await receive(c.client);
        EXPECT_FALSE(at_client.is_text);
        EXPECT_TRUE(at_client.data.empty());

        co_await round_trip(c.client, c.server, "still open");
    }());
}

// ---------------------------------------------------------------------------
// FrameMode::Partial
// ---------------------------------------------------------------------------

// In Partial mode receive() returns each piece lws delivers, and only the last has
// is_final set. The message after it starts afresh.
TEST(WsStreamTest, PartialModeDeliversFragmentsOnServer) {
    Runtime rt;
    rt.block_on([]() -> Coro<void> {
        Connection c = co_await connect_pair(30243, {.frame_mode = WsStream::FrameMode::Partial});
        const std::vector<std::byte> payload = pattern(20'000);

        co_await c.client.send(payload);
        co_await c.client.send("next");

        Fragments big = co_await receive_fragments(c.server);
        EXPECT_GT(big.count, 1);
        EXPECT_EQ(big.text, 0);
        EXPECT_TRUE(big.data == payload);

        Fragments next = co_await receive_fragments(c.server);
        EXPECT_EQ(next.text, next.count);
        EXPECT_EQ(next.data.size(), 4u);
    }());
}

TEST(WsStreamTest, PartialModeDeliversFragmentsOnClient) {
    Runtime rt;
    rt.block_on([]() -> Coro<void> {
        Connection c =
            co_await connect_pair(30244, {}, {.frame_mode = WsStream::FrameMode::Partial});
        const std::vector<std::byte> payload = pattern(20'000);

        co_await c.server.send(payload);
        co_await c.server.send("next");

        Fragments big = co_await receive_fragments(c.client);
        EXPECT_GT(big.count, 1);
        EXPECT_EQ(big.text, 0);
        EXPECT_TRUE(big.data == payload);

        Fragments next = co_await receive_fragments(c.client);
        EXPECT_EQ(next.text, next.count);
        EXPECT_EQ(next.data.size(), 4u);
    }());
}

// ---------------------------------------------------------------------------
// Both directions at once
// ---------------------------------------------------------------------------

// Each end sends and receives at the same time, in a task of its own, while a third
// thread may be the one servicing lws. Here for the sanitizers as much as for the
// counts.
TEST(WsStreamTest, BothEndsSendAndReceiveAtOnce) {
    Runtime rt(4);
    rt.block_on([]() -> Coro<void> {
        constexpr int kMessages = 500;
        Connection c = co_await connect_pair(30253);

        auto server = spawn(talk(std::move(c.server), "s", "c", kMessages));
        co_await talk(std::move(c.client), "c", "s", kMessages);
        co_await std::move(server);
    }());
}

// ---------------------------------------------------------------------------
// One task sending, another receiving
// ---------------------------------------------------------------------------
//
// The supported way to share a stream: at most one send() and one receive() in
// flight, which may be in different tasks.

// On each end one task sends while another receives, and the four may each be on a
// thread of their own, with lws serviced on any of them.
TEST(WsStreamTest, SendAndReceiveInSeparateTasks) {
    Runtime rt(4);
    rt.block_on(send_and_receive_in_separate_tasks(30261, 500));
}

// The same on one thread, where the tasks and the driver take strict turns.
TEST(WsStreamTest, SendAndReceiveInSeparateTasksOnSingleThreadRuntime) {
    Runtime rt(1);
    rt.block_on(send_and_receive_in_separate_tasks(30262, 100));
}

// A receive() that stays pending while another task completes many sends on the
// same stream: the sends leave the receiver's waker alone, and it is still woken by
// the one message that comes at the end.
TEST(WsStreamTest, ReceivePendingWhileAnotherTaskSends) {
    Runtime rt(4);
    rt.block_on([]() -> Coro<void> {
        constexpr int kMessages = 200;
        Connection c = co_await connect_pair(30263);
        Shared client = std::make_shared<WsStream>(std::move(c.client));

        auto receives = spawn(receive_task(client, "done", 1));
        auto sends    = spawn(send_task(client, "c", kMessages));

        // The server answers only once it has everything.
        co_await receive_many(c.server, "c", kMessages);
        co_await c.server.send("done0");

        co_await std::move(sends);
        co_await std::move(receives);
    }());
}

// The peer closes while one task waits in receive() and another is sending: the
// receive fails, and so does the next send.
TEST(WsStreamTest, PeerCloseEndsSenderAndReceiverTasks) {
    Runtime rt(4);
    rt.block_on([]() -> Coro<void> {
        Connection c = co_await connect_pair(30264);
        Shared client = std::make_shared<WsStream>(std::move(c.client));

        auto receives = spawn(receive_error_task(client));
        auto sends    = spawn(send_until_closed(client));

        co_await sleep_for(20ms);
        { WsStream server = std::move(c.server); }   // ~WsStream closes the connection.

        EXPECT_EQ(co_await std::move(receives), "WsStream::receive: connection closed");
        EXPECT_TRUE(co_await std::move(sends));
    }());
}

// The sender and the receiver of one stream in different Runtimes: the sender in the
// Runtime that services the connection, the receiver in another, on another thread.
TEST(WsStreamTest, SendAndReceiveInSeparateRuntimes) {
    constexpr int kMessages = 200;
    Runtime owner;
    Connection c = owner.block_on(connect_pair(30265));

    Runtime other;
    std::thread receiver([&other, &c] {
        try {
            other.block_on(receive_many(c.client, "s", kMessages));
        } catch (const std::exception& e) {
            ADD_FAILURE() << e.what();
        }
    });

    owner.block_on([](Connection& c) -> Coro<void> {
        (void)co_await join(send_many(c.client, "c", kMessages),
                            send_many(c.server, "s", kMessages),
                            receive_many(c.server, "c", kMessages));
    }(c));
    receiver.join();
}

// ---------------------------------------------------------------------------
// Close
// ---------------------------------------------------------------------------

TEST(WsStreamTest, QueuedMessagesDeliveredBeforeCloseThrows) {
    Runtime rt;
    rt.block_on([]() -> Coro<void> {
        Connection c = co_await connect_pair(30207);

        {
            WsStream client = std::move(c.client);
            co_await client.send("last");
        }  // ~WsStream closes the connection.
        co_await sleep_for(100ms);  // Let the close arrive behind the message.

        EXPECT_EQ(text(co_await receive(c.server)), "last");
        bool threw = false;
        try {
            (void)co_await receive(c.server);
        } catch (const std::runtime_error& e) {
            threw = std::string(e.what()) == "WsStream::receive: connection closed";
        }
        EXPECT_TRUE(threw);
    }());
}

// A peer that goes without a Close frame: its socket is simply closed. The server
// end learns of it from the socket, and both receive() and send() report it.
TEST(WsStreamTest, ServerEndSeesPeerVanish) {
    Runtime rt;
    rt.block_on([]() -> Coro<void> {
        WsListener listener = co_await WsListener::bind("127.0.0.1", 30254);
        auto [server, tcp] = co_await join(accept(listener), raw_client(30254));

        // The hand-made client is a real one as far as the server can tell.
        co_await tcp.write(raw_frame(0x1, 'h', 'i'));
        EXPECT_EQ(text(co_await receive(server)), "hi");

        { TcpStream gone = std::move(tcp); }
        EXPECT_EQ(co_await receive_error(server), "WsStream::receive: connection closed");

        bool threw = false;
        try {
            co_await server.send("too late");
        } catch (const std::runtime_error&) {
            threw = true;
        }
        EXPECT_TRUE(threw);
    }());
}

// A frame with a reserved opcode is a protocol error: lws answers with a Close
// frame, and once the peer has gone the stream is closed, not left waiting.
TEST(WsStreamTest, ServerEndClosesOnBadFrame) {
    Runtime rt;
    rt.block_on([]() -> Coro<void> {
        WsListener listener = co_await WsListener::bind("127.0.0.1", 30255);
        auto [server, tcp] = co_await join(accept(listener), raw_client(30255));

        co_await tcp.write(raw_frame(0x3, 'n', 'o'));
        auto answer = co_await timeout(2s, tcp.read(std::string(64, '\0')));
        EXPECT_EQ(answer.index(), 0u);
        if (answer.index() == 0) {
            auto& [n, buf] = std::get<0>(answer).value;
            EXPECT_GE(n, 2u);
            if (n > 0) {
                EXPECT_EQ(static_cast<unsigned char>(buf[0]), 0x88);   // FIN + Close
            }
        }

        { TcpStream gone = std::move(tcp); }
        EXPECT_EQ(co_await receive_error(server), "WsStream::receive: connection closed");
    }());
}

// The client end, when the Runtime servicing its server is destroyed under it.
TEST(WsStreamTest, ClientEndSeesServerRuntimeGoAway) {
    std::optional<WsListener> listener;
    std::optional<WsStream>   server;
    std::optional<WsStream>   client;

    Runtime client_rt;
    {
        Runtime server_rt;
        listener.emplace(server_rt.block_on(WsListener::bind("127.0.0.1", 30256)));
        client.emplace(client_rt.block_on(WsStream::connect(url(30256))));
        server.emplace(server_rt.block_on(accept(*listener)));

        client_rt.block_on(send_many(*client, "before ", 1));
        server_rt.block_on(receive_many(*server, "before ", 1));
    }

    EXPECT_EQ(client_rt.block_on(receive_error(*client)), "WsStream::receive: connection closed");
    client.reset();
    server.reset();
    listener.reset();
}

// send() on a connection the peer has closed fails at once; nothing is queued.
TEST(WsStreamTest, SendOnClosedConnectionThrows) {
    Runtime rt;
    rt.block_on([]() -> Coro<void> {
        Connection c = co_await connect_pair(30229);

        { WsStream server = std::move(c.server); }   // ~WsStream closes the connection.
        EXPECT_EQ(co_await receive_error(c.client), "WsStream::receive: connection closed");

        bool threw = false;
        try {
            co_await c.client.send("too late");
        } catch (const std::runtime_error& e) {
            threw = std::string(e.what()) == "WsStream::send: connection closed";
        }
        EXPECT_TRUE(threw);
    }());
}

// ---------------------------------------------------------------------------
// Send queue and dropped sends
// ---------------------------------------------------------------------------
//
// A polled send is a node in its connection's send queue, and the node is a member
// of the SendFuture. These drop a send at each point of its life and check that the
// queue is left whole: the sends around it still go out, in order.
//
// Those that hold several sends of one stream in flight test what the code does
// today, not what WsStream promises: its contract is one send() at a time.

// A send that was never polled was never queued, and sends nothing.
TEST(WsStreamTest, SendDroppedBeforePollSendsNothing) {
    Runtime rt;
    rt.block_on([]() -> Coro<void> {
        Connection c = co_await connect_pair(30230);

        { auto dropped = c.client.send("never"); }
        co_await c.client.send("real");
        EXPECT_EQ(text(co_await receive(c.server)), "real");
    }());
}

// A send moved before its first poll (here into the future timeout() returns) is
// still sent.
TEST(WsStreamTest, SendMovedBeforePollIsSent) {
    Runtime rt;
    rt.block_on([]() -> Coro<void> {
        Connection c = co_await connect_pair(30231);

        auto result = co_await timeout(2s, c.client.send("moved"));
        EXPECT_EQ(result.index(), 0u);
        EXPECT_EQ(text(co_await receive(c.server)), "moved");
    }());
}

// Several sends queued before lws writes any of them go out in the order they were
// queued.
TEST(WsStreamTest, SendsQueuedTogetherArriveInOrder) {
    Runtime rt(1);
    rt.block_on([]() -> Coro<void> {
        Connection c = co_await connect_pair(30232);

        std::array<std::string, 8>                         words;
        std::array<std::optional<WsStream::SendFuture>, 8> sends;
        for (std::size_t i = 0; i < sends.size(); ++i) {
            words[i] = "message " + std::to_string(i);
            sends[i].emplace(c.client.send(words[i]));
        }

        co_await InOnePoll([&](detail::Context& ctx) {
            for (auto& send : sends) EXPECT_TRUE(send->poll(ctx).isPending());
        });
        for (auto& send : sends) EXPECT_FALSE(co_await finish(*send));

        for (const auto& word : words) EXPECT_EQ(text(co_await receive(c.server)), word);
    }());
}

// A send dropped while it is the only one queued is not sent, and the queue it
// leaves empty still takes the next send.
TEST(WsStreamTest, SendDroppedWhileQueuedIsNotSent) {
    Runtime rt(1);
    rt.block_on([]() -> Coro<void> {
        Connection c = co_await connect_pair(30233);

        co_await InOnePoll([&](detail::Context& ctx) {
            auto dropped = c.client.send("never");
            EXPECT_TRUE(dropped.poll(ctx).isPending());
        });
        co_await c.client.send("real");
        EXPECT_EQ(text(co_await receive(c.server)), "real");
    }());
}

// A send dropped from the head, the middle and the tail of the queue.
TEST(WsStreamTest, SendDroppedFromHeadOfQueue) {
    Runtime rt(1);
    rt.block_on(drop_one_of_three(30234, 0));
}

TEST(WsStreamTest, SendDroppedFromMiddleOfQueue) {
    Runtime rt(1);
    rt.block_on(drop_one_of_three(30235, 1));
}

TEST(WsStreamTest, SendDroppedFromTailOfQueue) {
    Runtime rt(1);
    rt.block_on(drop_one_of_three(30236, 2));
}

// A close behind two queued sends takes priority over them: neither is sent, and
// both are failed and taken off the queue by the close. One is then polled, and
// reports ENOTCONN; the other is dropped without being polled again.
TEST(WsStreamTest, CloseFailsQueuedSends) {
    Runtime rt(1);
    rt.block_on([]() -> Coro<void> {
        Connection c = co_await connect_pair(30237);

        std::optional<WsStream>             client(std::move(c.client));
        std::optional<WsStream::SendFuture> awaited;
        std::optional<WsStream::SendFuture> dropped;
        awaited.emplace(client->send("one"));
        dropped.emplace(client->send("two"));

        co_await InOnePoll([&](detail::Context& ctx) {
            EXPECT_TRUE(awaited->poll(ctx).isPending());
            EXPECT_TRUE(dropped->poll(ctx).isPending());
            client.reset();   // ~WsStream posts the close.
        });

        const std::error_code code = co_await finish(*awaited);
        EXPECT_EQ(code, std::errc::not_connected) << code.message();
        dropped.reset();

        EXPECT_EQ(co_await receive_error(c.server), "WsStream::receive: connection closed");
    }());
}

// A send still queued when its Runtime goes away completes all the same: sent by a
// last turn of the driver, or failed with ENOTCONN when the context is destroyed. It
// can then be polled from another Runtime, and dropped.
TEST(WsStreamTest, QueuedSendOutlivingRuntimeCompletes) {
    std::optional<Connection>           kept;
    std::optional<WsStream::SendFuture> send;
    {
        Runtime rt(1);
        kept.emplace(rt.block_on(connect_pair(30238)));
        send.emplace(kept->client.send("late"));
        rt.block_on([](WsStream::SendFuture& send) -> Coro<void> {
            co_await InOnePoll([&send](detail::Context& ctx) {
                EXPECT_TRUE(send.poll(ctx).isPending());
            });
        }(*send));
    }

    Runtime rt;
    const std::error_code code = rt.block_on(finish(*send));
    if (code) {
        EXPECT_EQ(code, std::errc::not_connected) << code.message();
    }
    send.reset();
    kept.reset();
}

// Sends dropped at every point around lws_write(): before the driver's thread has
// taken the frame, while it is writing it, and after. The delay before each drop is
// swept across that window, so which one a drop meets is a matter of timing; this
// test is here for the sanitizers. A dropped send may or may not reach the peer.
// Every send that was awaited must, in order.
TEST(WsStreamTest, SendsDroppedAroundTheWrite) {
    Runtime rt(4);
    rt.block_on([]() -> Coro<void> {
        using Clock = std::chrono::steady_clock;
        constexpr int kDrops     = 500;
        constexpr int kRealEvery = 10;

        Connection c = co_await connect_pair(30239);

        int reals = 0;
        for (int i = 0; i < kDrops; ++i) {
            co_await InOnePoll([&c, i](detail::Context& ctx) {
                auto dropped = c.client.send("drop");
                (void)dropped.poll(ctx);
                const auto until = Clock::now() + (i % 50) * 4us;
                while (Clock::now() < until) {}
            });
            if (i % kRealEvery == 0) {
                const std::string word = "real " + std::to_string(reals++);
                co_await c.client.send(word);
            }
        }

        int next = 0;
        for (int received = 0; next < reals && received < kDrops + reals; ++received) {
            const std::string word = text(co_await receive(c.server));
            if (word == "drop") continue;
            EXPECT_EQ(word, "real " + std::to_string(next));
            ++next;
        }
        EXPECT_EQ(next, reals);
    }());
}

// ---------------------------------------------------------------------------
// Connect: names and errors
// ---------------------------------------------------------------------------

// "localhost" may resolve to ::1 first. This lws build has no IPv6, so that address
// is skipped and connect() falls through to 127.0.0.1.
TEST(WsStreamTest, ConnectByName) {
    Runtime rt;
    rt.block_on([]() -> Coro<void> {
        WsListener listener = co_await WsListener::bind("127.0.0.1", 30208);
        auto [server, client] = co_await join(accept(listener),
                                              WsStream::connect(url(30208, "localhost")));
        co_await client.send("hi");
        EXPECT_EQ(text(co_await receive(server)), "hi");
    }());
}

TEST(WsStreamTest, ConnectRefusedThrowsSystemError) {
    Runtime rt;
    auto code = rt.block_on(connect_error(url(30209)));   // nothing listening
    EXPECT_EQ(code, std::errc::connection_refused) << code.message();
}

TEST(WsStreamTest, ConnectToUnresolvableNameThrowsDnsError) {
    Runtime rt;
    auto code = rt.block_on(connect_error(url(80, "nonexistent.invalid")));
    EXPECT_EQ(&code.category(), &dns_error_category()) << code.message();
}

TEST(WsStreamTest, MalformedUrlThrowsInvalidArgument) {
    Runtime rt;
    EXPECT_THROW(rt.block_on(WsStream::connect("http://127.0.0.1/")), std::invalid_argument);
}

// A connect dropped part way (here by a 1 ms timeout) may or may not have finished
// its handshake. Either way it must not leave anything broken: a connection it did
// make is closed, and the next connect works.
// A server that takes the TCP connection and closes it without a word.
TEST(WsStreamTest, ConnectToPeerThatClosesThrows) {
    Runtime rt;
    rt.block_on([]() -> Coro<void> {
        auto peer = spawn(tcp_peer(co_await TcpListener::bind("127.0.0.1", 30257), std::nullopt, 0ms));
        const std::error_code code = co_await connect_error(url(30257));
        EXPECT_EQ(code, std::errc::connection_refused) << code.message();
        co_await std::move(peer);
    }());
}

// A server that answers the upgrade request with an ordinary HTTP response.
TEST(WsStreamTest, ConnectToPlainHttpServerThrows) {
    Runtime rt;
    rt.block_on([]() -> Coro<void> {
        auto peer = spawn(tcp_peer(co_await TcpListener::bind("127.0.0.1", 30258),
                                   "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n", 100ms));
        const std::error_code code = co_await connect_error(url(30258));
        EXPECT_EQ(code, std::errc::connection_refused) << code.message();
        co_await std::move(peer);
    }());
}

// A server that takes the TCP connection and never answers: the connect is dropped
// in the middle of the handshake, by a timeout. The client context must be left fit
// for the next connect.
TEST(WsStreamTest, ConnectDroppedMidHandshakeIsHarmless) {
    Runtime rt;
    rt.block_on([]() -> Coro<void> {
        auto peer = spawn(tcp_peer(co_await TcpListener::bind("127.0.0.1", 30259), std::nullopt, 300ms));
        auto timed_out = co_await timeout(100ms, WsStream::connect(url(30259)));
        EXPECT_EQ(timed_out.index(), 1u);

        Connection c = co_await connect_pair(30260);
        co_await round_trip(c.client, c.server, "after a dropped handshake");
        co_await std::move(peer);
    }());
}

TEST(WsStreamTest, DroppedConnectIsHarmless) {
    Runtime rt;
    rt.block_on([]() -> Coro<void> {
        WsListener listener = co_await WsListener::bind("127.0.0.1", 30212);
        (void)co_await timeout(1ms, WsStream::connect(url(30212)));
        co_await sleep_for(200ms);   // let a connection it made reach the accept queue

        WsStream client = co_await WsStream::connect(url(30212));
        co_await client.send("hello");
        // The dropped connect's connection, if any, is queued first, already closed.
        bool found = false;
        for (int i = 0; i < 2 && !found; ++i) {
            WsStream server = co_await accept(listener);
            try {
                found = text(co_await receive(server)) == "hello";
            } catch (const std::runtime_error&) {
                // "connection closed": the dropped connect's connection.
            }
        }
        EXPECT_TRUE(found);
    }());
}

// ---------------------------------------------------------------------------
// Listener lifetime and bind errors
// ---------------------------------------------------------------------------

TEST(WsListenerTest, StreamsSurviveListenerDrop) {
    Runtime rt;
    rt.block_on([]() -> Coro<void> {
        Connection c = co_await connect_pair(30210);
        { WsListener gone = std::move(c.listener); }

        co_await c.client.send("still");
        EXPECT_EQ(text(co_await receive(c.server)), "still");
        co_await c.server.send("here");
        EXPECT_EQ(text(co_await receive(c.client)), "here");
    }());
}

// The accepted stream keeps the port bound, but the upgrade is refused.
TEST(WsListenerTest, DroppedListenerRejectsNewConnections) {
    Runtime rt;
    rt.block_on([]() -> Coro<void> {
        Connection c = co_await connect_pair(30211);
        { WsListener gone = std::move(c.listener); }

        auto code = co_await connect_error(url(30211));
        EXPECT_TRUE(code) << "connect to a dropped listener succeeded";
    }());
}

// Once the listener and every stream it accepted are gone, so is the socket.
TEST(WsListenerTest, PortReleasedWhenLastStreamDrops) {
    Runtime rt;
    rt.block_on([]() -> Coro<void> {
        {
            Connection c = co_await connect_pair(30216);
            (void)c;
        }
        auto code = co_await bind_when_free(30216);
        EXPECT_FALSE(code) << code.message();
    }());
}

// The last reference may go on a thread that has nothing to do with the Runtime.
// The drop doesn't block, and the driver still destroys the context.
TEST(WsListenerTest, LastHandleDroppedOnAnotherThread) {
    Runtime rt;
    Connection c = rt.block_on(connect_pair(30226));
    std::thread other([](Connection) {}, std::move(c));
    other.join();

    auto code = rt.block_on(bind_when_free(30226));
    EXPECT_FALSE(code) << code.message();
}

TEST(WsListenerTest, BindAddressInUseThrowsEaddrinuse) {
    Runtime rt;
    rt.block_on([]() -> Coro<void> {
        WsListener first = co_await WsListener::bind("127.0.0.1", 30215);
        auto code = co_await bind_error("127.0.0.1", 30215);
        EXPECT_EQ(code, std::errc::address_in_use) << code.message();
    }());
}

TEST(WsListenerTest, BindUnresolvableNameThrowsDnsError) {
    Runtime rt;
    auto code = rt.block_on(bind_error("nonexistent.invalid", 30217));
    EXPECT_EQ(&code.category(), &dns_error_category()) << code.message();
}

// An empty host listens on every interface, loopback included.
TEST(WsListenerTest, EmptyHostBindsAllInterfaces) {
    Runtime rt;
    rt.block_on([]() -> Coro<void> {
        WsListener listener = co_await WsListener::bind("", 30218);
        auto [server, client] = co_await join(accept(listener), WsStream::connect(url(30218)));
        co_await client.send("any");
        EXPECT_EQ(text(co_await receive(server)), "any");
    }());
}

// ---------------------------------------------------------------------------
// Listener: accept queue
// ---------------------------------------------------------------------------

// An accept() dropped before a client arrives (here, by a timeout) leaves nothing
// behind: the next one gets the next connection.
TEST(WsListenerTest, DroppedAcceptLosesNothing) {
    Runtime rt;
    rt.block_on([]() -> Coro<void> {
        WsListener listener = co_await WsListener::bind("127.0.0.1", 30250);

        auto timed_out = co_await timeout(50ms, listener.accept());
        EXPECT_EQ(timed_out.index(), 1u);

        WsStream client = co_await WsStream::connect(url(30250));
        WsStream server = co_await accept(listener);
        co_await round_trip(client, server, "after a dropped accept");
    }());
}

// connect() completes with the handshake, whether or not accept() has been called.
// The connections, and what their clients sent meanwhile, wait in order.
TEST(WsListenerTest, ConnectionsWaitForAcceptInOrder) {
    Runtime rt;
    rt.block_on([]() -> Coro<void> {
        WsListener listener = co_await WsListener::bind("127.0.0.1", 30251);

        std::vector<WsStream> clients;
        for (int i = 0; i < 3; ++i) {
            clients.push_back(co_await WsStream::connect(url(30251)));
            const std::string word = "client " + std::to_string(i);
            co_await clients.back().send(word);
        }

        for (int i = 0; i < 3; ++i) {
            WsStream server = co_await accept(listener);
            EXPECT_EQ(text(co_await receive(server)), "client " + std::to_string(i));
        }
    }());
}

// Several connections to one listener, open together: each stream reaches its own
// peer and no other.
TEST(WsListenerTest, ManyClientsOnOneListener) {
    Runtime rt;
    rt.block_on([]() -> Coro<void> {
        constexpr int kClients = 8;
        WsListener listener = co_await WsListener::bind("127.0.0.1", 30252);

        std::vector<Ends> ends;
        for (int i = 0; i < kClients; ++i) ends.push_back(co_await connect_to(listener, 30252));

        for (int i = kClients - 1; i >= 0; --i) {
            const std::string word = "client " + std::to_string(i);
            co_await ends[i].client.send(word);
        }
        for (int i = 0; i < kClients; ++i) {
            const std::string word = "server " + std::to_string(i);
            EXPECT_EQ(text(co_await receive(ends[i].server)), "client " + std::to_string(i));
            co_await ends[i].server.send(word);
        }
        for (int i = kClients - 1; i >= 0; --i) {
            EXPECT_EQ(text(co_await receive(ends[i].client)), "server " + std::to_string(i));
        }
    }());
}

// ---------------------------------------------------------------------------
// Listener: upgrade hooks and subprotocols
// ---------------------------------------------------------------------------

// process_request sees the request's path and headers, and decides whether the
// upgrade goes ahead. The hook runs in a turn of the driver, hence the mutex.
TEST(WsListenerTest, ProcessRequestSeesRequestAndCanReject) {
    Runtime rt;
    rt.block_on([]() -> Coro<void> {
        struct Seen {
            std::mutex  mutex;
            std::string path;
            std::string host;
        };
        auto seen = std::make_shared<Seen>();

        WsListener::Options options;
        options.process_request =
            [seen](const WsUpgradeRequest& request) -> std::optional<WsUpgradeRejection> {
            std::lock_guard lk(seen->mutex);
            seen->path = request.path();
            seen->host = request.header("Host");
            if (seen->path == "/denied") return WsUpgradeRejection{403, "denied"};
            return std::nullopt;
        };
        WsListener listener = co_await WsListener::bind("127.0.0.1", 30247, std::move(options));

        const std::error_code code = co_await connect_error(url(30247) + "denied");
        EXPECT_EQ(code, std::errc::connection_refused) << code.message();
        {
            std::lock_guard lk(seen->mutex);
            EXPECT_EQ(seen->path, "/denied");
        }

        auto [server, client] =
            co_await join(accept(listener), WsStream::connect(url(30247) + "allowed"));
        {
            std::lock_guard lk(seen->mutex);
            EXPECT_EQ(seen->path, "/allowed");
            EXPECT_EQ(seen->host.substr(0, 9), "127.0.0.1");
        }
        co_await round_trip(client, server, "allowed");
    }());
}

// A listener with one subprotocol takes a client that offers it and a client that
// offers none, and refuses a client that only offers another.
TEST(WsListenerTest, SubprotocolMustMatchWhenOffered) {
    Runtime rt;
    rt.block_on([]() -> Coro<void> {
        WsListener::Options options;
        options.subprotocols = {"chat"};
        WsListener listener = co_await WsListener::bind("127.0.0.1", 30248, std::move(options));

        Ends same = co_await connect_to(listener, 30248, {.subprotocols = {"chat"}});
        co_await round_trip(same.client, same.server, "same");

        Ends none = co_await connect_to(listener, 30248);
        co_await round_trip(none.client, none.server, "none");

        const std::error_code code =
            co_await connect_error(url(30248), {.subprotocols = {"other"}});
        EXPECT_EQ(code, std::errc::connection_refused) << code.message();
    }());
}

// select_subprotocol is given what the client offered, and refuses the connection by
// returning no name.
TEST(WsListenerTest, SelectSubprotocolSeesOfferAndCanReject) {
    Runtime rt;
    rt.block_on([]() -> Coro<void> {
        struct Seen {
            std::mutex               mutex;
            std::vector<std::string> offered;
        };
        auto seen = std::make_shared<Seen>();

        WsListener::Options options;
        options.subprotocols       = {"chat"};
        options.select_subprotocol = [seen](std::span<const std::string_view> offered) {
            std::lock_guard lk(seen->mutex);
            seen->offered.assign(offered.begin(), offered.end());
            for (std::string_view name : offered) {
                if (name == "nope") return std::string();
            }
            return std::string("chat");
        };
        WsListener listener = co_await WsListener::bind("127.0.0.1", 30249, std::move(options));

        Ends chosen = co_await connect_to(listener, 30249, {.subprotocols = {"chat"}});
        {
            std::lock_guard lk(seen->mutex);
            EXPECT_EQ(seen->offered, std::vector<std::string>{"chat"});
        }
        co_await round_trip(chosen.client, chosen.server, "chosen");

        const std::error_code code =
            co_await connect_error(url(30249), {.subprotocols = {"chat", "nope"}});
        EXPECT_EQ(code, std::errc::connection_refused) << code.message();
        {
            std::lock_guard lk(seen->mutex);
            EXPECT_EQ(seen->offered, (std::vector<std::string>{"chat", "nope"}));
        }
    }());
}

// ---------------------------------------------------------------------------
// Runtime integration
// ---------------------------------------------------------------------------

// Shutdown with a listener, both streams and a pending receive still alive in a task
// must neither hang nor wake into the destroyed executor.
TEST(WsStreamTest, RuntimeShutdownWithOpenStreams) {
    {
        Runtime rt;
        rt.block_on([]() -> Coro<void> {
            Connection c = co_await connect_pair(30213);
            (void)spawn(hold(std::move(c))).detach();
            co_await sleep_for(50ms);   // let the task start its receive
        }());
    }
    SUCCEED();
}

// lws is serviced by the IoDriver, so a Runtime whose executor doesn't turn it can't
// host a WebSocket. Reported up front rather than by a connect that never finishes.
TEST(WsStreamTest, ConnectWithoutDriverThrowsLogicError) {
    Runtime rt(std::in_place_type<CurrentThreadExecutor>,
               std::make_unique<PollingParker>([] {}));
    EXPECT_THROW(rt.block_on(WsStream::connect(url(30214))), std::logic_error);
    EXPECT_THROW(rt.block_on(WsListener::bind("127.0.0.1", 30214)), std::logic_error);
}

// One thread both runs the tasks and turns the driver, so every lws callback runs
// between two polls of the task it wakes.
TEST(WsStreamTest, RoundTripOnSingleThreadRuntime) {
    Runtime rt(1);
    rt.block_on([]() -> Coro<void> {
        Connection c = co_await connect_pair(30219);
        co_await round_trip(c.client, c.server, "one thread");
    }());
}

// A connection lives only as long as the Runtime that made it. A stream that
// outlives it finds the connection closed, and can still be dropped.
TEST(WsStreamTest, StreamOutlivingRuntimeIsClosed) {
    std::optional<Connection> kept;
    {
        Runtime rt;
        kept.emplace(rt.block_on(connect_pair(30220)));
    }

    Runtime rt;
    rt.block_on([](Connection& c) -> Coro<void> {
        EXPECT_EQ(co_await receive_error(c.client), "WsStream::receive: connection closed");
        EXPECT_EQ(co_await receive_error(c.server), "WsStream::receive: connection closed");
        bool accept_threw = false;
        try {
            (void)co_await accept(c.listener);
        } catch (const std::runtime_error& e) {
            accept_threw = std::string(e.what()) == "WsListener::accept: listener is closed";
        }
        EXPECT_TRUE(accept_threw);
    }(*kept));
    kept.reset();
}

// A listener whose Runtime goes before a connection ever reached it. lws has then
// not initialized the listener's protocol, and tears the context down without
// telling the protocol callback; accept() must still learn that it is closed.
TEST(WsStreamTest, AcceptAfterRuntimeOfUnusedListenerIsClosed) {
    std::optional<WsListener> kept;
    {
        Runtime rt(1);
        kept.emplace(rt.block_on(WsListener::bind("127.0.0.1", 30228)));
    }

    Runtime rt;
    rt.block_on([](WsListener& listener) -> Coro<void> {
        bool accept_threw = false;
        try {
            (void)co_await accept(listener);
        } catch (const std::runtime_error& e) {
            accept_threw = std::string(e.what()) == "WsListener::accept: listener is closed";
        }
        EXPECT_TRUE(accept_threw);
    }(*kept));
    kept.reset();
}

// Each Runtime has its own client context, destroyed with it. The second Runtime's
// is created after the first one's was destroyed with a connection still open,
// which lws 4.5 only allows because of the anchor context (ws_service.cpp).
TEST(WsStreamTest, ClientContextPerRuntimeInTurn) {
    for (uint16_t port : {uint16_t{30221}, uint16_t{30222}}) {
        Runtime rt;
        rt.block_on([](uint16_t port) -> Coro<void> {
            Connection c = co_await connect_pair(port);
            co_await round_trip(c.client, c.server, "again");
            (void)spawn(hold(std::move(c))).detach();   // still open at shutdown
            co_await sleep_for(20ms);
        }(port));
    }
}

// Two Runtimes with WebSocket clients at the same time: each has its own client
// context, serviced by its own driver.
TEST(WsStreamTest, TwoRuntimesConnectAtOnce) {
    Runtime a;
    Runtime b;
    Connection ca = a.block_on(connect_pair(30223));
    Connection cb = b.block_on(connect_pair(30224));
    a.block_on(round_trip(ca.client, ca.server, "a"));
    b.block_on(round_trip(cb.client, cb.server, "b"));
    a.block_on(round_trip(ca.client, ca.server, "a again"));
    // Both connections are dropped here, on a thread of neither Runtime.
}

// A stream may be used from a Runtime other than the one servicing its connection:
// the sends are posted to that one's driver, and its callbacks wake tasks here.
TEST(WsStreamTest, UsedFromAnotherRuntime) {
    Runtime owner;
    Connection c = owner.block_on(connect_pair(30225));

    Runtime other;
    other.block_on(round_trip(c.client, c.server, "from elsewhere"));
}

// A thread the Runtime doesn't own sends, while the Runtime's workers are parked in
// the driver: the post has to wake one of them.
TEST(WsStreamTest, SendFromOutsideTheRuntime) {
    Runtime rt;
    Connection c = rt.block_on(connect_pair(30227));

    blocking_wait(c.client.send("outside"));
    EXPECT_EQ(text(blocking_wait(c.server.receive())), "outside");
}
