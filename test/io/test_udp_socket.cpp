#include <gtest/gtest.h>
#include <coro/io/udp_socket.h>
#include <coro/io/socket_address.h>
#include <coro/runtime/runtime.h>
#include <coro/coro.h>
#include <string>
#include <system_error>
#include <variant>

using namespace coro;

// ---------------------------------------------------------------------------
// Concept checks
// ---------------------------------------------------------------------------

static_assert(Future<JoinHandle<UdpSocket>>);
static_assert(Future<JoinHandle<void>>);

// ---------------------------------------------------------------------------
// SocketAddress
// ---------------------------------------------------------------------------

TEST(SocketAddressTest, ParseAndFormatIpv4) {
    auto addr = SocketAddress::parse("127.0.0.1", 9001);
    ASSERT_TRUE(addr.has_value());
    EXPECT_EQ(addr->to_string(), "127.0.0.1:9001");
}

TEST(SocketAddressTest, ParseAndFormatIpv6) {
    auto addr = SocketAddress::parse("::1", 9001);
    ASSERT_TRUE(addr.has_value());
    EXPECT_EQ(addr->to_string(), "[::1]:9001");
}

TEST(SocketAddressTest, ParseIpv6WithScope) {
    auto addr = SocketAddress::parse("fe80::1%3", 9001);
    ASSERT_TRUE(addr.has_value());
    EXPECT_EQ(addr->to_string(), "[fe80::1%3]:9001");
}

TEST(SocketAddressTest, ParseInvalidReturnsNullopt) {
    EXPECT_FALSE(SocketAddress::parse("not-an-address", 9001).has_value());
    EXPECT_FALSE(SocketAddress::parse("999.999.999.999", 9001).has_value());
}

TEST(SocketAddressTest, EqualityCompares) {
    auto a = SocketAddress::parse("127.0.0.1", 9001);
    auto b = SocketAddress::parse("127.0.0.1", 9001);
    auto c = SocketAddress::parse("127.0.0.1", 9002);
    ASSERT_TRUE(a.has_value() && b.has_value() && c.has_value());
    EXPECT_EQ(*a, *b);
    EXPECT_NE(*a, *c);
}

// ---------------------------------------------------------------------------
// UdpSocket — send_to / recv_from
// ---------------------------------------------------------------------------

TEST(UdpSocketTest, SendToRecvFromRoundTrip) {
    Runtime rt;
    rt.block_on([]() -> Coro<void> {
        auto server = co_await UdpSocket::bind("127.0.0.1", 30001);
        auto client = co_await UdpSocket::bind("127.0.0.1", 30002);

        auto server_addr = SocketAddress::parse("127.0.0.1", 30001).value();
        co_await client.send_to(std::string("hello"), server_addr);

        auto [n, buf, sender] = co_await server.recv_from(std::string(64, '\0'));
        buf.resize(n);
        EXPECT_EQ(buf, "hello");
        EXPECT_EQ(sender.to_string(), "127.0.0.1:30002");
    }());
}

TEST(UdpSocketTest, RecvFromTruncatesOversizedDatagram) {
    Runtime rt;
    rt.block_on([]() -> Coro<void> {
        auto server = co_await UdpSocket::bind("127.0.0.1", 30011);
        auto client = co_await UdpSocket::bind("127.0.0.1", 30012);

        auto server_addr = SocketAddress::parse("127.0.0.1", 30011).value();
        co_await client.send_to(std::string("0123456789"), server_addr);

        auto [n, buf, sender] = co_await server.recv_from(std::string(4, '\0'));
        (void)sender;
        buf.resize(n);
        EXPECT_EQ(n, 4u);
        EXPECT_EQ(buf, "0123");
    }());
}

// ---------------------------------------------------------------------------
// UdpSocket — connect / send / recv (fixed-peer mode)
// ---------------------------------------------------------------------------

TEST(UdpSocketTest, ConnectSendRecvRoundTrip) {
    Runtime rt;
    rt.block_on([]() -> Coro<void> {
        auto server = co_await UdpSocket::bind("127.0.0.1", 30021);
        auto client = co_await UdpSocket::bind("127.0.0.1", 30022);

        auto server_addr = SocketAddress::parse("127.0.0.1", 30021).value();
        co_await client.connect(server_addr);
        co_await client.send(std::string("connected-hello"));

        auto [n, buf, sender] = co_await server.recv_from(std::string(64, '\0'));
        buf.resize(n);
        EXPECT_EQ(buf, "connected-hello");

        // Reply using the client's fixed peer, filtering to only that sender.
        co_await server.send_to(std::string("reply"), sender);
        auto [rn, rbuf] = co_await client.recv(std::string(64, '\0'));
        rbuf.resize(rn);
        EXPECT_EQ(rbuf, "reply");
    }());
}

// ---------------------------------------------------------------------------
// UdpSocket — send_to() throws on the libuv/Linux backend once connect() has
// fixed a peer, *regardless* of whether the explicit destination matches the
// connected peer or not: per sendto(2), Linux returns EISCONN whenever a
// recipient is specified at all on an already-connected socket, not only when
// it differs from the connected peer. This is a real platform-specific
// behavior difference from the lwIP backend (whose udp_sendto() is never
// filtered by the connected state) — see doc/design/udp_socket.md's "Known
// limitations" section.
// ---------------------------------------------------------------------------

TEST(UdpSocketTest, SendToThrowsAfterConnectEvenToSamePeer) {
    Runtime rt;
    rt.block_on([]() -> Coro<void> {
        auto server = co_await UdpSocket::bind("127.0.0.1", 30031);
        auto client = co_await UdpSocket::bind("127.0.0.1", 30032);

        auto server_addr = SocketAddress::parse("127.0.0.1", 30031).value();
        co_await client.connect(server_addr);

        bool threw = false;
        try {
            co_await client.send_to(std::string("explicit-same-peer"), server_addr);
        } catch (const std::system_error&) {
            threw = true;
        }
        EXPECT_TRUE(threw);
        (void)server;
    }());
}

TEST(UdpSocketTest, SendToThrowsAfterConnectToDifferentPeer) {
    Runtime rt;
    rt.block_on([]() -> Coro<void> {
        auto server = co_await UdpSocket::bind("127.0.0.1", 30031);
        auto client = co_await UdpSocket::bind("127.0.0.1", 30032);
        auto other  = co_await UdpSocket::bind("127.0.0.1", 30033);

        auto server_addr = SocketAddress::parse("127.0.0.1", 30031).value();
        co_await client.connect(server_addr);

        auto other_addr = SocketAddress::parse("127.0.0.1", 30033).value();
        bool threw = false;
        try {
            co_await client.send_to(std::string("explicit"), other_addr);
        } catch (const std::system_error&) {
            threw = true;
        }
        EXPECT_TRUE(threw);
        (void)other;
        (void)server;
    }());
}

// ---------------------------------------------------------------------------
// UdpSocket — set_broadcast / join_multicast / leave_multicast
// ---------------------------------------------------------------------------

TEST(UdpSocketTest, SetBroadcastDoesNotThrow) {
    Runtime rt;
    rt.block_on([]() -> Coro<void> {
        auto sock = co_await UdpSocket::bind("0.0.0.0", 30041);
        co_await sock.set_broadcast(true);
        co_await sock.set_broadcast(false);
    }());
}

TEST(UdpSocketTest, JoinAndLeaveMulticastDoesNotThrow) {
    Runtime rt;
    rt.block_on([]() -> Coro<void> {
        auto sock = co_await UdpSocket::bind("0.0.0.0", 30051);
        auto group = SocketAddress::parse("239.255.0.1", 0).value();
        auto iface = SocketAddress::parse("127.0.0.1", 0).value();
        co_await sock.join_multicast(std::get<Ipv4Address>(group.address),
                                      std::get<Ipv4Address>(iface.address));
        co_await sock.leave_multicast(std::get<Ipv4Address>(group.address),
                                       std::get<Ipv4Address>(iface.address));
    }());
}

TEST(UdpSocketTest, MulticastLoopbackDelivery) {
    Runtime rt;
    rt.block_on([]() -> Coro<void> {
        auto receiver = co_await UdpSocket::bind("0.0.0.0", 30061);
        auto sender   = co_await UdpSocket::bind("0.0.0.0", 30062);

        auto group_addr = SocketAddress::parse("239.255.0.2", 0).value();
        auto group = std::get<Ipv4Address>(group_addr.address);
        co_await receiver.join_multicast(group, Ipv4Address{});

        auto dest = SocketAddress::parse("239.255.0.2", 30061).value();
        co_await sender.send_to(std::string("multicast-hello"), dest);

        auto [n, buf, sender_addr] = co_await receiver.recv_from(std::string(64, '\0'));
        (void)sender_addr;
        buf.resize(n);
        EXPECT_EQ(buf, "multicast-hello");

        co_await receiver.leave_multicast(group, Ipv4Address{});
    }());
}
