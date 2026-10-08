#include <coro/io/udp_socket.h>
#include <coro/io/socket_address.h>
#include <coro/runtime/runtime.h>
#include <coro/coro.h>
#include <coro/future.h>
#include <coro/sync/select.h>
#include <gtest/gtest.h>

#include <lwip/init.h>
#include <lwip/timeouts.h>
#include <lwip/netif.h>

// send and recv are hand-written futures, not coroutines: no frame per datagram.
static_assert(coro::Future<coro::UdpSendFuture<std::string>>);
static_assert(coro::Future<coro::UdpRecvFuture<std::string, true>>);
static_assert(coro::Future<coro::UdpRecvFuture<std::string, false>>);

// A future that is ready on its first poll. select(pending, ReadyNow{}) polls the
// pending future once, then drops it.
struct ReadyNow {
    using OutputType = void;
    coro::PollResult<void> poll(coro::detail::Context&) { return coro::PollReady; }
};

// ---------------------------------------------------------------------------
// Fixture — initialises lwIP once per process.
//
// lwip_init() automatically creates a loopback netif at 127.0.0.1 when
// LWIP_HAVE_LOOPIF=1 (set in lwipopts.h). No manual netif_add() needed.
//
// Note: multicast (join_multicast/leave_multicast) is exercised only in the
// desktop test suite (test/io/test_udp_socket.cpp) — lwIP's default
// loopback netif (netif_loopif_init) does not set NETIF_FLAG_IGMP, so
// igmp_joingroup_netif() would fail against it here regardless of the
// UdpSocket implementation.
// ---------------------------------------------------------------------------

class LwipUdpLoopback : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        lwip_init();
    }

    static void poll_until(coro::Runtime& rt, bool& flag, int max_iters = 100'000) {
        for (int i = 0; i < max_iters && !flag; ++i) {
            rt.poll();
            sys_check_timeouts();
            netif_poll_all();
        }
    }
};

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST_F(LwipUdpLoopback, SendToRecvFromRoundTrip) {
    coro::Runtime rt;
    bool done = false;
    std::string received;
    std::string sender_str;

    rt.spawn([](coro::Runtime&, bool& done, std::string& received, std::string& sender_str)
            -> coro::Coro<void> {
        auto server = co_await coro::UdpSocket::bind("127.0.0.1", 21001);
        auto client = co_await coro::UdpSocket::bind("127.0.0.1", 21002);

        auto server_addr = coro::SocketAddress::parse("127.0.0.1", 21001).value();
        co_await client.send_to(std::string("hello"), server_addr);

        auto [n, buf, sender] = co_await server.recv_from(std::string(64, '\0'));
        buf.resize(n);
        received   = buf;
        sender_str = sender.to_string();
        done = true;
    }(rt, done, received, sender_str)).detach();

    poll_until(rt, done);
    ASSERT_TRUE(done) << "test timed out";
    EXPECT_EQ(received, "hello");
    EXPECT_EQ(sender_str, "127.0.0.1:21002");
}

TEST_F(LwipUdpLoopback, RecvFromTruncatesOversizedDatagram) {
    coro::Runtime rt;
    bool done = false;
    std::size_t n_out = 0;
    std::string received;

    rt.spawn([](coro::Runtime&, bool& done, std::size_t& n_out, std::string& received)
            -> coro::Coro<void> {
        auto server = co_await coro::UdpSocket::bind("127.0.0.1", 21011);
        auto client = co_await coro::UdpSocket::bind("127.0.0.1", 21012);

        auto server_addr = coro::SocketAddress::parse("127.0.0.1", 21011).value();
        co_await client.send_to(std::string("0123456789"), server_addr);

        auto [n, buf, sender] = co_await server.recv_from(std::string(4, '\0'));
        (void)sender;
        buf.resize(n);
        n_out = n;
        received = buf;
        done = true;
    }(rt, done, n_out, received)).detach();

    poll_until(rt, done);
    ASSERT_TRUE(done) << "test timed out";
    EXPECT_EQ(n_out, 4u);
    EXPECT_EQ(received, "0123");
}

TEST_F(LwipUdpLoopback, ConnectSendRecvRoundTrip) {
    coro::Runtime rt;
    bool done = false;
    std::string received;
    std::string reply;

    rt.spawn([](coro::Runtime&, bool& done, std::string& received, std::string& reply)
            -> coro::Coro<void> {
        auto server = co_await coro::UdpSocket::bind("127.0.0.1", 21021);
        auto client = co_await coro::UdpSocket::bind("127.0.0.1", 21022);

        auto server_addr = coro::SocketAddress::parse("127.0.0.1", 21021).value();
        co_await client.connect(server_addr);
        co_await client.send(std::string("connected-hello"));

        auto [n, buf, sender] = co_await server.recv_from(std::string(64, '\0'));
        buf.resize(n);
        received = buf;

        co_await server.send_to(std::string("reply"), sender);
        auto [rn, rbuf] = co_await client.recv(std::string(64, '\0'));
        rbuf.resize(rn);
        reply = rbuf;
        done = true;
    }(rt, done, received, reply)).detach();

    poll_until(rt, done);
    ASSERT_TRUE(done) << "test timed out";
    EXPECT_EQ(received, "connected-hello");
    EXPECT_EQ(reply, "reply");
}

TEST_F(LwipUdpLoopback, SetBroadcastIsNoOp) {
    coro::Runtime rt;
    bool done = false;

    rt.spawn([](coro::Runtime&, bool& done) -> coro::Coro<void> {
        auto sock = co_await coro::UdpSocket::bind("0.0.0.0", 21031);
        // No-op on the lwIP backend (see doc/design/udp_socket.md's "Multicast and
        // broadcast" section) — just confirms it doesn't throw.
        co_await sock.set_broadcast(true);
        co_await sock.set_broadcast(false);
        done = true;
    }(rt, done)).detach();

    poll_until(rt, done);
    ASSERT_TRUE(done) << "test timed out";
}

TEST_F(LwipUdpLoopback, SendWithoutConnectThrows) {
    coro::Runtime rt;
    bool done = false;
    std::exception_ptr caught;

    rt.spawn([](coro::Runtime&, bool& done, std::exception_ptr& caught) -> coro::Coro<void> {
        auto sock = co_await coro::UdpSocket::bind("127.0.0.1", 21041);
        try {
            co_await sock.send(std::string("no peer"));
        } catch (...) {
            caught = std::current_exception();
        }
        done = true;
    }(rt, done, caught)).detach();

    poll_until(rt, done);
    ASSERT_TRUE(done) << "test timed out";
    ASSERT_NE(caught, nullptr) << "expected send() to throw without connect()";
}

TEST_F(LwipUdpLoopback, DroppedRecvDeregistersAndNextRecvWorks) {
    // A receive dropped mid-wait must take its buffer back out of the socket: the
    // datagram sent afterwards has to land in the second receive's buffer, not the
    // first one's (which is freed by then).
    coro::Runtime rt;
    bool done = false;
    std::size_t branch = 0;
    std::string received;

    rt.spawn([](coro::Runtime&, bool& done, std::size_t& branch, std::string& received)
            -> coro::Coro<void> {
        auto server = co_await coro::UdpSocket::bind("127.0.0.1", 21051);
        auto client = co_await coro::UdpSocket::bind("127.0.0.1", 21052);

        auto lost = co_await coro::select(server.recv_from(std::string(64, '\0')), ReadyNow{});
        branch = lost.index();

        auto server_addr = coro::SocketAddress::parse("127.0.0.1", 21051).value();
        co_await client.send_to(std::string("after drop"), server_addr);

        auto [n, buf, sender] = co_await server.recv_from(std::string(64, '\0'));
        (void)sender;
        buf.resize(n);
        received = buf;
        done = true;
    }(rt, done, branch, received)).detach();

    poll_until(rt, done);
    ASSERT_TRUE(done) << "test timed out";
    EXPECT_EQ(branch, 1u);
    EXPECT_EQ(received, "after drop");
}

TEST_F(LwipUdpLoopback, RecvWithoutConnectThrows) {
    coro::Runtime rt;
    bool done = false;
    std::exception_ptr caught;

    rt.spawn([](coro::Runtime&, bool& done, std::exception_ptr& caught) -> coro::Coro<void> {
        auto sock = co_await coro::UdpSocket::bind("127.0.0.1", 21061);
        try {
            co_await sock.recv(std::string(16, '\0'));
        } catch (...) {
            caught = std::current_exception();
        }
        done = true;
    }(rt, done, caught)).detach();

    poll_until(rt, done);
    ASSERT_TRUE(done) << "test timed out";
    ASSERT_NE(caught, nullptr) << "expected recv() to throw without connect()";
}
