#include <gtest/gtest.h>
#include <coro/io/lookup_host.h>
#include <coro/io/udp_socket.h>
#include <coro/io/socket_address.h>
#include <coro/runtime/runtime.h>
#include <coro/coro.h>
#include <coro/sync/sleep.h>
#include <coro/sync/timeout.h>
#include <coro/runtime/work_sharing_executor.h>
#include <coro/runtime/current_thread_executor.h>
#include <coro/runtime/parker.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <variant>
#include <vector>

using namespace coro;
using namespace std::chrono_literals;

// ---------------------------------------------------------------------------
// Concept checks
// ---------------------------------------------------------------------------

static_assert(Future<Coro<UdpSocket>>);
static_assert(Future<Coro<void>>);
static_assert(Future<UdpSendFuture<std::string>>);
static_assert(Future<UdpRecvFuture<std::string, true>>);
static_assert(Future<UdpRecvSegmentsFuture<std::string>>);
// Leaf futures: safe to drop mid-wait, so they expose no cancel().
static_assert(!Cancellable<UdpSendFuture<std::string>>);
static_assert(!Cancellable<UdpRecvFuture<std::string, true>>);
static_assert(!Cancellable<UdpRecvSegmentsFuture<std::string>>);

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
// UdpSocket — send_to() with an explicit destination remains usable on
// Linux even after connect() has fixed a peer. Some BSD man pages document
// EISCONN for this case, but Linux's sendto(2) accepts an explicit
// destination on a connected UDP socket without error. See
// doc/design/udp_socket.md's "Known limitations" section.
// ---------------------------------------------------------------------------

TEST(UdpSocketTest, SendToStillWorksAfterConnectToSamePeer) {
    Runtime rt;
    rt.block_on([]() -> Coro<void> {
        auto server = co_await UdpSocket::bind("127.0.0.1", 30031);
        auto client = co_await UdpSocket::bind("127.0.0.1", 30032);

        auto server_addr = SocketAddress::parse("127.0.0.1", 30031).value();
        co_await client.connect(server_addr);

        co_await client.send_to(std::string("explicit-same-peer"), server_addr);
        auto [n, buf, sender] = co_await server.recv_from(std::string(64, '\0'));
        buf.resize(n);
        EXPECT_EQ(buf, "explicit-same-peer");
    }());
}

TEST(UdpSocketTest, SendToStillWorksAfterConnectToDifferentPeer) {
    Runtime rt;
    rt.block_on([]() -> Coro<void> {
        auto server = co_await UdpSocket::bind("127.0.0.1", 30031);
        auto client = co_await UdpSocket::bind("127.0.0.1", 30032);
        auto other  = co_await UdpSocket::bind("127.0.0.1", 30033);

        auto server_addr = SocketAddress::parse("127.0.0.1", 30031).value();
        co_await client.connect(server_addr);

        auto other_addr = SocketAddress::parse("127.0.0.1", 30033).value();
        co_await client.send_to(std::string("explicit"), other_addr);
        auto [n, buf, sender] = co_await other.recv_from(std::string(64, '\0'));
        buf.resize(n);
        EXPECT_EQ(buf, "explicit");
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

// ---------------------------------------------------------------------------
// UdpSocket — set_segment_size (UDP GSO)
// ---------------------------------------------------------------------------

#ifdef __linux__
TEST(UdpSocketTest, SegmentSizeSplitsOneSendIntoDatagrams) {
    Runtime rt;
    rt.block_on([]() -> Coro<void> {
        auto server = co_await UdpSocket::bind("127.0.0.1", 30071);
        auto client = co_await UdpSocket::bind("127.0.0.1", 30072);
        co_await client.connect(SocketAddress::parse("127.0.0.1", 30071).value());
        client.set_segment_size(4);

        // Three full segments and a short tail.
        co_await client.send(std::string("aaaabbbbccccd"));
        for (std::string want : {"aaaa", "bbbb", "cccc", "d"}) {
            auto [n, buf, sender] = co_await server.recv_from(std::string(64, '\0'));
            (void)sender;
            buf.resize(n);
            EXPECT_EQ(buf, want);
        }

        // A send no longer than the segment size is one datagram; 0 disables.
        co_await client.send(std::string("xyz"));
        client.set_segment_size(0);
        co_await client.send(std::string("unsegmented"));
        for (std::string want : {"xyz", "unsegmented"}) {
            auto [n, buf, sender] = co_await server.recv_from(std::string(64, '\0'));
            (void)sender;
            buf.resize(n);
            EXPECT_EQ(buf, want);
        }
    }());
}
#else
TEST(UdpSocketTest, SegmentSizeUnsupportedOffLinux) {
    Runtime rt;
    rt.block_on([]() -> Coro<void> {
        auto sock = co_await UdpSocket::bind("127.0.0.1", 30071);
        EXPECT_THROW(sock.set_segment_size(4), std::system_error);
    }());
}
#endif

// ---------------------------------------------------------------------------
// UdpSocket — set_gro / recv_segments_from (UDP GRO)
// ---------------------------------------------------------------------------

#ifdef __linux__
TEST(UdpSocketTest, GroKeepsSegmentedSendCoalesced) {
    Runtime rt;
    rt.block_on([]() -> Coro<void> {
        auto server = co_await UdpSocket::bind("127.0.0.1", 30073);
        auto client = co_await UdpSocket::bind("127.0.0.1", 30074);
        co_await client.connect(SocketAddress::parse("127.0.0.1", 30073).value());
        server.set_gro(true);
        client.set_segment_size(4);

        // Queued before the receive: the fast path reads it.
        co_await client.send(std::string("aaaabbbbccccd"));
        {
            auto [n, buf, sender, seg] = co_await server.recv_segments_from(std::string(65535, '\0'));
            EXPECT_EQ(sender, SocketAddress::parse("127.0.0.1", 30074).value());
            EXPECT_EQ(seg, 4u);
            buf.resize(n);
            EXPECT_EQ(buf, "aaaabbbbccccd");
        }

        // Receive started on an empty socket: the slow path waits for readability,
        // then reads. A lone datagram reports segment_size == size.
        auto pending = coro::spawn(server.recv_segments_from(std::string(65535, '\0')));
        co_await coro::sleep_for(std::chrono::milliseconds(50));
        co_await client.send(std::string("xyz"));
        auto [n, buf, sender, seg] = co_await pending;
        (void)sender;
        EXPECT_EQ(n, 3u);
        EXPECT_EQ(seg, 3u);
        buf.resize(n);
        EXPECT_EQ(buf, "xyz");
    }());
}

TEST(UdpSocketTest, RecvSegmentsWithoutGroIsOneDatagramPerRead) {
    Runtime rt;
    rt.block_on([]() -> Coro<void> {
        auto server = co_await UdpSocket::bind("127.0.0.1", 30075);
        auto client = co_await UdpSocket::bind("127.0.0.1", 30076);
        co_await client.connect(SocketAddress::parse("127.0.0.1", 30075).value());
        client.set_segment_size(4);

        co_await client.send(std::string("aaaabb"));
        for (std::string want : {"aaaa", "bb"}) {
            auto [n, buf, sender, seg] = co_await server.recv_segments_from(std::string(65535, '\0'));
            (void)sender;
            EXPECT_EQ(seg, n);
            buf.resize(n);
            EXPECT_EQ(buf, want);
        }
    }());
}
#else
TEST(UdpSocketTest, GroUnsupportedOffLinux) {
    Runtime rt;
    rt.block_on([]() -> Coro<void> {
        auto sock = co_await UdpSocket::bind("127.0.0.1", 30073);
        EXPECT_THROW(sock.set_gro(true), std::system_error);
    }());
}
#endif

// ---------------------------------------------------------------------------
// UdpSocket on the IoDriver (doc/design/udp_socket.md, "Desktop (IoDriver) backend")
// ---------------------------------------------------------------------------

namespace {

bool ipv6_loopback_available() {
    const int fd = ::socket(AF_INET6, SOCK_DGRAM, 0);
    if (fd < 0) return false;
    sockaddr_in6 addr{};
    addr.sin6_family = AF_INET6;
    addr.sin6_addr   = in6addr_loopback;
    const bool ok = ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
    ::close(fd);
    return ok;
}

// The receive starts on an empty socket, so it hits EAGAIN and waits on the driver.
Coro<void> recv_waits_then_completes(uint16_t port) {
    auto server = co_await UdpSocket::bind("127.0.0.1", port);
    auto client = co_await UdpSocket::bind("127.0.0.1", 0);
    auto pending = coro::spawn(server.recv_from(std::string(64, '\0')));
    co_await coro::sleep_for(50ms);
    co_await client.send_to(std::string("late"), SocketAddress::parse("127.0.0.1", port).value());
    auto [n, buf, sender] = co_await pending;
    (void)sender;
    buf.resize(n);
    EXPECT_EQ(buf, "late");
}

constexpr int kConcurrentDatagrams = 100;   // well under the default SO_RCVBUF

Coro<void> echo_n(std::shared_ptr<UdpSocket> sock, int n) {
    for (int i = 0; i < n; ++i) {
        auto [len, buf, from] = co_await sock->recv_from(std::string(16, '\0'));
        buf.resize(len);
        co_await sock->send_to(std::move(buf), from);
    }
}

Coro<void> send_n(std::shared_ptr<UdpSocket> sock, SocketAddress dest, int n) {
    for (int i = 0; i < n; ++i)
        co_await sock->send_to(std::to_string(i), dest);
}

// Returns how many distinct datagrams 0..n-1 came back.
Coro<int> recv_n(std::shared_ptr<UdpSocket> sock, int n) {
    std::vector<bool> seen(n, false);
    for (int i = 0; i < n; ++i) {
        auto [len, buf, from] = co_await sock->recv_from(std::string(16, '\0'));
        (void)from;
        buf.resize(len);
        const int v = std::stoi(buf);
        if (v >= 0 && v < n) seen[v] = true;
    }
    int distinct = 0;
    for (bool b : seen) distinct += b ? 1 : 0;
    co_return distinct;
}

} // namespace

TEST(UdpSocketTest, BindIpv6Loopback) {
    if (!ipv6_loopback_available()) GTEST_SKIP() << "no IPv6 loopback on this host";
    Runtime rt;
    rt.block_on([]() -> Coro<void> {
        auto server = co_await UdpSocket::bind("::1", 30081);
        auto client = co_await UdpSocket::bind("::1", 30082);
        co_await client.send_to(std::string("v6"), SocketAddress::parse("::1", 30081).value());
        auto [n, buf, sender] = co_await server.recv_from(std::string(64, '\0'));
        buf.resize(n);
        EXPECT_EQ(buf, "v6");
        EXPECT_EQ(sender.to_string(), "[::1]:30082");
    }());
}

TEST(UdpSocketTest, BindAddressInUseThrowsOnAwait) {
    Runtime rt;
    rt.block_on([]() -> Coro<void> {
        auto first = co_await UdpSocket::bind("127.0.0.1", 30083);
        bool in_use = false;
        try {
            auto second = co_await UdpSocket::bind("127.0.0.1", 30083);
            (void)second;
        } catch (const std::system_error& e) {
            in_use = e.code() == std::errc::address_in_use;
        }
        EXPECT_TRUE(in_use);

        // Hostnames are resolved now; an unresolvable one fails with the DNS error.
        bool unresolved = false;
        try {
            auto bad = co_await UdpSocket::bind("nonexistent.invalid", 0);
            (void)bad;
        } catch (const std::system_error& e) {
            unresolved = e.code().category() == dns_error_category();
        }
        EXPECT_TRUE(unresolved);
    }());
}

TEST(UdpSocketTest, RecvWaitsThenCompletesWorkStealing) {
    Runtime rt(4);
    rt.block_on(recv_waits_then_completes(30084));
}

TEST(UdpSocketTest, RecvWaitsThenCompletesCurrentThread) {
    Runtime rt(1);
    rt.block_on(recv_waits_then_completes(30085));
}

// A receive that loses a race to a timer is dropped mid-wait. A dropped receive leaves
// only a stale weak waker in the registration, never an armed buffer, so the next
// datagram goes to the next receive.
TEST(UdpSocketTest, DroppedRecvLosesNoDatagram) {
    Runtime rt;
    rt.block_on([]() -> Coro<void> {
        auto server = co_await UdpSocket::bind("127.0.0.1", 30086);
        auto client = co_await UdpSocket::bind("127.0.0.1", 0);
        auto timed = co_await coro::timeout(20ms, server.recv_from(std::string(64, '\0')));
        EXPECT_EQ(timed.index(), 1u);   // timed out; the receive was dropped

        co_await client.send_to(std::string("after-drop"),
                                SocketAddress::parse("127.0.0.1", 30086).value());
        auto [n, buf, sender] = co_await server.recv_from(std::string(64, '\0'));
        (void)sender;
        buf.resize(n);
        EXPECT_EQ(buf, "after-drop");
    }());
}

// The pending receive shares the socket's state, so destroying the UdpSocket handle
// neither closes the fd under it nor loses the wake.
TEST(UdpSocketTest, SocketDroppedWhileRecvPending) {
    Runtime rt;
    rt.block_on([]() -> Coro<void> {
        auto client = co_await UdpSocket::bind("127.0.0.1", 0);
        auto server = co_await UdpSocket::bind("127.0.0.1", 30087);
        auto pending = coro::spawn(server.recv_from(std::string(64, '\0')));
        co_await coro::sleep_for(20ms);   // let the receive start waiting
        { UdpSocket gone = std::move(server); }

        co_await client.send_to(std::string("still-open"),
                                SocketAddress::parse("127.0.0.1", 30087).value());
        auto [n, buf, sender] = co_await pending;
        (void)sender;
        buf.resize(n);
        EXPECT_EQ(buf, "still-open");
    }());
}

// One socket with a send task and a receive task in flight at once, likely on
// different workers: one waiter per direction.
TEST(UdpSocketTest, ConcurrentSendAndRecvOnOneSocket) {
    Runtime rt(4);
    const int distinct = rt.block_on([]() -> Coro<int> {
        auto echo_sock = std::make_shared<UdpSocket>(co_await UdpSocket::bind("127.0.0.1", 30088));
        auto sock      = std::make_shared<UdpSocket>(co_await UdpSocket::bind("127.0.0.1", 0));
        const auto echo_addr = SocketAddress::parse("127.0.0.1", 30088).value();

        auto echo = coro::spawn(echo_n(echo_sock, kConcurrentDatagrams));
        auto rx   = coro::spawn(recv_n(sock, kConcurrentDatagrams));
        auto tx   = coro::spawn(send_n(sock, echo_addr, kConcurrentDatagrams));
        co_await tx;
        // A lost wake (or a dropped datagram) shows up as a timeout, not a hang.
        auto got = co_await coro::timeout(5s, std::move(rx));
        co_await coro::timeout(5s, std::move(echo));
        if (got.index() != 0) co_return -1;
        co_return std::get<0>(got).value;
    }());
    EXPECT_EQ(distinct, kConcurrentDatagrams);
}

// A CurrentThreadExecutor with a caller-supplied parker never turns the IoDriver, so a
// driver-backed socket could never be woken there: bind() refuses instead of hanging
// later.
TEST(UdpSocketTest, BindThrowsWithoutDriver) {
    EXPECT_TRUE(Runtime(1).turns_io_driver());
    EXPECT_TRUE(Runtime(2).turns_io_driver());
    EXPECT_TRUE(Runtime(std::in_place_type<WorkSharingExecutor>, 2).turns_io_driver());

    Runtime rt(std::in_place_type<CurrentThreadExecutor>,
               std::make_unique<PollingParker>([] {}));
    EXPECT_FALSE(rt.turns_io_driver());
    EXPECT_THROW(rt.block_on(UdpSocket::bind("127.0.0.1", 0)), std::logic_error);
}

// The WorkSharing port: a receive that has to wait is woken by an idle worker turning
// the driver.
TEST(UdpSocketTest, RecvWaitsOnWorkSharing) {
    Runtime rt(std::in_place_type<WorkSharingExecutor>, 2);
    const std::size_t n = rt.block_on([]() -> Coro<std::size_t> {
        auto rx = co_await UdpSocket::bind("127.0.0.1", 30090);
        auto tx = co_await UdpSocket::bind("127.0.0.1", 0);
        auto pending = coro::spawn(rx.recv_from(std::string(64, '\0')));
        co_await coro::sleep_for(20ms);   // let the receive start waiting
        co_await tx.send_to(std::string("hello"),
                            SocketAddress::parse("127.0.0.1", 30090).value());
        auto [len, buf, sender] = co_await pending;
        (void)buf;
        (void)sender;
        co_return len;
    }());
    EXPECT_EQ(n, 5u);
}
