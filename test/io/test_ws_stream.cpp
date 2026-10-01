#include <gtest/gtest.h>
#include <coro/io/ws_listener.h>
#include <coro/io/ws_stream.h>
#include <coro/runtime/runtime.h>
#include <coro/sync/join.h>
#include <coro/sync/sleep.h>
#include <coro/sync/timeout.h>
#include <coro/coro.h>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <system_error>
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

Coro<Connection> connect_pair(uint16_t port, WsListener::Options options = {}) {
    WsListener listener = co_await WsListener::bind("127.0.0.1", port, std::move(options));
    auto [server, client] = co_await join(
        listener.accept(), WsStream::connect("ws://127.0.0.1:" + std::to_string(port) + "/"));
    co_return Connection{std::move(listener), std::move(server), std::move(client)};
}

// Receives one message, failing the test instead of hanging if none arrives within 2 s.
Coro<WsStream::Message> receive(WsStream& ws) {
    auto result = co_await timeout(2s, ws.receive());
    if (result.index() != 0) throw std::runtime_error("receive timed out");
    co_return std::move(std::get<0>(result).value);
}

std::string text(const WsStream::Message& msg) { return std::string(msg.as_text()); }

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
