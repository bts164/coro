// WsStream / WsListener on the lws service threads (doc/design/websocket_stream.md).
// Each test uses its own port in 30201-30299.

#include <gtest/gtest.h>
#include <coro/io/lookup_host.h>
#include <coro/io/ws_listener.h>
#include <coro/io/ws_stream.h>
#include <coro/runtime/current_thread_executor.h>
#include <coro/runtime/parker.h>
#include <coro/runtime/runtime.h>
#include <coro/sync/join.h>
#include <coro/sync/sleep.h>
#include <coro/sync/timeout.h>
#include <coro/coro.h>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
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
Coro<std::error_code> connect_error(std::string to) {
    try {
        auto result = co_await timeout(2s, WsStream::connect(std::move(to)));
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

// Waits on the server end forever; the Runtime destroys it at shutdown.
Coro<void> hold(Connection c) {
    (void)co_await c.server.receive();
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
        WsListener again = co_await WsListener::bind("127.0.0.1", 30216);
        (void)again;
    }());
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

// lws runs on its own thread, so WS needs no IoDriver. No timeouts or sleeps here: timers
// need the IoDriver.
TEST(WsStreamTest, WorksWithoutDriver) {
    Runtime rt(std::in_place_type<CurrentThreadExecutor>,
               std::make_unique<PollingParker>([] {}));
    auto echoed = rt.block_on([]() -> Coro<std::string> {
        WsListener listener = co_await WsListener::bind("127.0.0.1", 30214);
        auto [server, client] = co_await join(listener.accept(),
                                              WsStream::connect(url(30214)));
        co_await client.send("no driver");
        auto msg = co_await server.receive();
        co_return std::string(msg.as_text());
    }());
    EXPECT_EQ(echoed, "no driver");
}
