// TcpStream / TcpListener, on every backend: the desktop sockets on the IoDriver
// (doc/design/tcp_stream.md) and lwIP, built for the host and on the board. The tests
// connect to 127.0.0.1, which on lwIP is its loopback interface.
//
// Most tests are shared. The desktop-only and lwIP-only ones are in their own
// sections at the end.

#include <gtest/gtest.h>
#include <coro/io/tcp_listener.h>
#include <coro/io/tcp_stream.h>
#include <coro/runtime/runtime.h>
#include <coro/coro.h>
#include <coro/sync/sleep.h>
#include <coro/sync/timeout.h>
#include "net_runtime.h"
#ifdef CORO_TCP_BACKEND_LWIP
#include <lwip/opt.h>   // TCP_SND_BUF
#else
#include <coro/io/lookup_host.h>
#include <coro/runtime/work_sharing_executor.h>
#include <coro/runtime/current_thread_executor.h>
#include <coro/runtime/parker.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <format>
#include <memory>
#endif
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

using namespace coro;
using namespace std::chrono_literals;

// ---------------------------------------------------------------------------
// Concept checks
// ---------------------------------------------------------------------------

static_assert(Future<Coro<TcpStream>>);
static_assert(Future<Coro<TcpListener>>);
// read and write are hand-written futures, not coroutines: no frame per transfer.
static_assert(Future<TcpReadFuture<std::string, false>>);
static_assert(Future<TcpReadFuture<std::string, true>>);
static_assert(Future<TcpWriteFuture<std::string>>);
// Leaf futures: safe to drop mid-wait, so they expose no cancel().
static_assert(!Cancellable<TcpReadFuture<std::string, false>>);
static_assert(!Cancellable<TcpReadFuture<std::string, true>>);
static_assert(!Cancellable<TcpWriteFuture<std::string>>);
#ifndef CORO_TCP_BACKEND_LWIP
static_assert(Future<TcpAcceptFuture>);
static_assert(!Cancellable<TcpAcceptFuture>);
#endif

namespace {

// A listener plus both ends of one connection on it. Every backend completes the
// handshake and queues the connection before accept() is called, so
// connect-then-accept works on one task.
struct Connection {
    TcpListener listener;
    TcpStream   client;
    TcpStream   server;
};

Coro<Connection> connect_pair(std::string host, uint16_t port) {
    auto listener = co_await TcpListener::bind(host, port);
    auto client   = co_await TcpStream::connect(host, port);
    auto server   = co_await listener.accept();
    co_return Connection{std::move(listener), std::move(client), std::move(server)};
}

// Echoes everything it reads until EOF.
Coro<void> echo_until_eof(TcpStream stream) {
    for (;;) {
        auto [n, buf] = co_await stream.read(std::string(4096, '\0'));
        if (n == 0) co_return;
        buf.resize(n);
        co_await stream.write(std::move(buf));
    }
}

Coro<void> accept_and_echo(TcpListener listener) {
    co_await echo_until_eof(co_await listener.accept());
}

// Connects to an echo server, sends msg and returns what came back.
Coro<std::string> echo_round_trip(uint16_t port, std::string msg) {
    auto server = coro::spawn(accept_and_echo(co_await TcpListener::bind("127.0.0.1", port)));
    auto client = co_await TcpStream::connect("127.0.0.1", port);
    co_await client.write(msg);
    auto [n, buf] = co_await client.read_exact(std::string(msg.size(), '\0'));
    buf.resize(n);
    { TcpStream closing = std::move(client); }   // EOF ends the echo task
    co_await server;
    co_return buf;
}

// The large transfer is sized to be several times what the send buffer holds, so
// that one write() has to go out in pieces. The reader takes it a chunk at a time
// and checks each chunk as it arrives: only the writer's copy of the data exists.
// On a Pico every copy comes out of a heap that lwIP and the test framework share.
#ifdef CORO_TCP_BACKEND_LWIP
// From TCP_SND_BUF, because the host and firmware lwipopts.h set it differently.
constexpr std::size_t kLargeTransfer = 2 * TCP_SND_BUF + 1024;
constexpr std::size_t kReadChunk     = 2048;
#else
constexpr std::size_t kLargeTransfer = 8 * 1024 * 1024;
constexpr std::size_t kReadChunk     = 64 * 1024;
#endif

std::byte pattern_at(std::size_t i) {
    return static_cast<std::byte>((i * 31) % 251);
}

// Accepts one connection and writes `bytes` of pattern in a single write().
Coro<void> accept_and_write_pattern(TcpListener listener, std::size_t bytes) {
    auto stream = co_await listener.accept();
    std::vector<std::byte> data(bytes);
    for (std::size_t i = 0; i < bytes; ++i) data[i] = pattern_at(i);
    auto back = co_await stream.write(std::move(data));
    EXPECT_EQ(back.size(), bytes);   // write() returns the buffer intact
}

// Returns the number of bytes that matched the pattern.
Coro<std::size_t> large_transfer(uint16_t port, std::size_t bytes) {
    auto writer = coro::spawn(accept_and_write_pattern(
        co_await TcpListener::bind("127.0.0.1", port), bytes));
    auto client = co_await TcpStream::connect("127.0.0.1", port);
    std::size_t received = 0;
    std::size_t matching = 0;
    while (received < bytes) {
        const std::size_t want = std::min(bytes - received, kReadChunk);
        auto [n, buf] = co_await client.read_exact(std::vector<std::byte>(want));
        for (std::size_t i = 0; i < n; ++i)
            matching += buf[i] == pattern_at(received + i) ? 1 : 0;
        received += n;
        if (n < want) break;   // EOF before the end
    }
    co_await writer;
    co_return matching;
}

// The backends report a refused connection differently: the desktop one as
// std::system_error with ECONNREFUSED, the lwIP one as std::runtime_error naming the
// connect.
bool is_connect_failure(const std::exception& e) {
#ifdef CORO_TCP_BACKEND_LWIP
    return std::string_view(e.what()).find("connect") != std::string_view::npos;
#else
    const auto* se = dynamic_cast<const std::system_error*>(&e);
    return se != nullptr && se->code() == std::errc::connection_refused;
#endif
}

} // namespace

// ---------------------------------------------------------------------------
// Basics
// ---------------------------------------------------------------------------

TEST(TcpStreamTest, EchoRoundTrip) {
    NetRuntime rt;
    EXPECT_EQ(rt.block_on(echo_round_trip(31019, "hello over tcp")), "hello over tcp");
}

TEST(TcpStreamTest, MultipleMessagesInOrder) {
    NetRuntime rt;
    rt.block_on([]() -> Coro<void> {
        auto c = co_await connect_pair("127.0.0.1", 31004);
        for (int i = 0; i < 10; ++i) {
            const std::string msg = "message " + std::to_string(i);
            co_await c.client.write(msg);
            auto [n, buf] = co_await c.server.read_exact(std::string(msg.size(), '\0'));
            buf.resize(n);
            EXPECT_EQ(buf, msg);
        }
    }());
}

TEST(TcpStreamTest, ReadReturnsZeroAtEof) {
    NetRuntime rt;
    rt.block_on([]() -> Coro<void> {
        auto c = co_await connect_pair("127.0.0.1", 31005);
        { TcpStream gone = std::move(c.client); }
        auto [n, buf] = co_await c.server.read(std::string(16, '\0'));
        (void)buf;
        EXPECT_EQ(n, 0u);
        // EOF is sticky.
        auto [n2, buf2] = co_await c.server.read(std::string(16, '\0'));
        (void)buf2;
        EXPECT_EQ(n2, 0u);
    }());
}

TEST(TcpStreamTest, ReadExactStopsShortAtEof) {
    NetRuntime rt;
    rt.block_on([]() -> Coro<void> {
        auto c = co_await connect_pair("127.0.0.1", 31006);
        co_await c.client.write(std::string("abc"));
        { TcpStream gone = std::move(c.client); }
        auto [n, buf] = co_await c.server.read_exact(std::string(10, '\0'));
        EXPECT_EQ(n, 3u);
        EXPECT_EQ(buf.substr(0, n), "abc");
    }());
}

// read() returns what has arrived, up to the buffer's size, and leaves the rest for
// the next read.
TEST(TcpStreamTest, ReadLeavesTheRestForTheNextRead) {
    NetRuntime rt;
    rt.block_on([]() -> Coro<void> {
        auto c = co_await connect_pair("127.0.0.1", 31021);
        co_await c.server.write(std::string("hello world"));

        auto [n1, buf1] = co_await c.client.read(std::string(5, '\0'));
        buf1.resize(n1);
        EXPECT_EQ(buf1, "hello");

        auto [n2, buf2] = co_await c.client.read(std::string(32, '\0'));
        buf2.resize(n2);
        EXPECT_EQ(buf2, " world");
    }());
}

// One write() far larger than the send buffer: it completes in pieces, each waiting
// for room, and every byte arrives in order.
TEST(TcpStreamTest, LargeWriteCompletesInPieces) {
    NetRuntime rt;
    EXPECT_EQ(rt.block_on(large_transfer(31020, kLargeTransfer)), kLargeTransfer);
}

// ---------------------------------------------------------------------------
// Errors
// ---------------------------------------------------------------------------

TEST(TcpStreamTest, ConnectRefusedThrowsOnAwait) {
    NetRuntime rt;
    rt.block_on([]() -> Coro<void> {
        bool refused = false;
        try {
            auto s = co_await TcpStream::connect("127.0.0.1", 31010);   // nothing listening
            (void)s;
        } catch (const std::exception& e) {
            refused = is_connect_failure(e);
        }
        EXPECT_TRUE(refused);
    }());
}

// ---------------------------------------------------------------------------
// Waiting and dropping
// ---------------------------------------------------------------------------

// An accept() that loses a race to a timer is dropped mid-wait; it accepted nothing,
// so the next accept() gets the connection.
TEST(TcpStreamTest, DroppedAcceptAcceptsNothing) {
    NetRuntime rt;
    rt.block_on([]() -> Coro<void> {
        auto listener = co_await TcpListener::bind("127.0.0.1", 31014);
        auto timed = co_await coro::timeout(20ms, listener.accept());
        EXPECT_EQ(timed.index(), 1u);   // timed out; the accept was dropped

        auto client = co_await TcpStream::connect("127.0.0.1", 31014);
        auto server = co_await listener.accept();
        co_await client.write(std::string("after-drop"));
        auto [n, buf] = co_await server.read_exact(std::string(10, '\0'));
        EXPECT_EQ(n, 10u);
        EXPECT_EQ(buf, "after-drop");
    }());
}

// A read() dropped mid-wait consumed nothing; the next read() gets the data.
TEST(TcpStreamTest, DroppedReadLosesNoData) {
    NetRuntime rt;
    rt.block_on([]() -> Coro<void> {
        auto c = co_await connect_pair("127.0.0.1", 31015);
        auto timed = co_await coro::timeout(20ms, c.server.read(std::string(64, '\0')));
        EXPECT_EQ(timed.index(), 1u);

        co_await c.client.write(std::string("after-drop"));
        auto [n, buf] = co_await c.server.read_exact(std::string(10, '\0'));
        EXPECT_EQ(n, 10u);
        EXPECT_EQ(buf, "after-drop");
    }());
}

#ifndef CORO_TCP_BACKEND_LWIP
// ===========================================================================
// Desktop only: the other executors, IPv6, errno values, and behaviour the lwIP
// backend does not have.
// ===========================================================================

namespace {

bool ipv6_loopback_available() {
    const int fd = ::socket(AF_INET6, SOCK_STREAM, 0);
    if (fd < 0) return false;
    sockaddr_in6 addr{};
    addr.sin6_family = AF_INET6;
    addr.sin6_addr   = in6addr_loopback;
    const bool ok = ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
    ::close(fd);
    return ok;
}

constexpr int kConcurrentMessages = 1000;

Coro<void> write_numbered(std::shared_ptr<TcpStream> stream, int n) {
    for (int i = 0; i < n; ++i) {
        co_await stream->write(std::format("{:07};", i));   // 8 bytes
    }
}

// Returns how many of the n echoed messages came back in order.
Coro<int> read_numbered(std::shared_ptr<TcpStream> stream, int n) {
    int in_order = 0;
    for (int i = 0; i < n; ++i) {
        auto [len, buf] = co_await stream->read_exact(std::string(8, '\0'));
        if (len != 8) break;
        if (std::stoi(buf) == i) ++in_order;
    }
    co_return in_order;
}

} // namespace

TEST(TcpStreamTest, EchoRoundTripWorkStealing) {
    Runtime rt(4);
    EXPECT_EQ(rt.block_on(echo_round_trip(31001, "hello over tcp")), "hello over tcp");
}

TEST(TcpStreamTest, EchoRoundTripCurrentThread) {
    Runtime rt(1);
    EXPECT_EQ(rt.block_on(echo_round_trip(31002, "hello over tcp")), "hello over tcp");
}

TEST(TcpStreamTest, EchoRoundTripWorkSharing) {
    Runtime rt(std::in_place_type<WorkSharingExecutor>, 2);
    EXPECT_EQ(rt.block_on(echo_round_trip(31003, "hello over tcp")), "hello over tcp");
}

// One write() far larger than the socket buffers: it completes in pieces, each waiting
// for write readiness, and every byte arrives in order.
TEST(TcpStreamTest, LargeWriteCompletesInPiecesWorkStealing) {
    Runtime rt(4);
    EXPECT_EQ(rt.block_on(large_transfer(31007, kLargeTransfer)), kLargeTransfer);
}

TEST(TcpStreamTest, LargeWriteCompletesInPiecesCurrentThread) {
    Runtime rt(1);
    EXPECT_EQ(rt.block_on(large_transfer(31008, kLargeTransfer)), kLargeTransfer);
}

TEST(TcpStreamTest, Ipv6Loopback) {
    if (!ipv6_loopback_available()) GTEST_SKIP() << "no IPv6 loopback on this host";
    Runtime rt;
    rt.block_on([]() -> Coro<void> {
        auto c = co_await connect_pair("::1", 31009);
        co_await c.client.write(std::string("v6"));
        auto [n, buf] = co_await c.server.read_exact(std::string(2, '\0'));
        EXPECT_EQ(n, 2u);
        EXPECT_EQ(buf, "v6");
    }());
}

TEST(TcpStreamTest, BadAddressesThrowOnAwait) {
    Runtime rt;
    rt.block_on([]() -> Coro<void> {
        auto first = co_await TcpListener::bind("127.0.0.1", 31011);
        bool in_use = false;
        try {
            auto second = co_await TcpListener::bind("127.0.0.1", 31011);
            (void)second;
        } catch (const std::system_error& e) {
            in_use = e.code() == std::errc::address_in_use;
        }
        EXPECT_TRUE(in_use);

        // Hostnames are resolved now; an unresolvable one fails with the DNS error.
        // Name-based connect/bind are covered in test_lookup_host.cpp.
        bool bad_bind = false;
        try {
            auto l = co_await TcpListener::bind("nonexistent.invalid", 0);
            (void)l;
        } catch (const std::system_error& e) {
            bad_bind = e.code().category() == dns_error_category();
        }
        EXPECT_TRUE(bad_bind);
    }());
}

// Writing to a connection the peer closed fails with EPIPE or ECONNRESET. It must not
// raise SIGPIPE, which would kill the test process.
TEST(TcpStreamTest, WriteToClosedPeerThrowsWithoutSigpipe) {
    Runtime rt;
    const int err = rt.block_on([]() -> Coro<int> {
        auto c = co_await connect_pair("127.0.0.1", 31012);
        { TcpStream gone = std::move(c.server); }
        for (int i = 0; i < 1000; ++i) {
            try {
                co_await c.client.write(std::string(1024, 'x'));
            } catch (const std::system_error& e) {
                co_return e.code().value();
            }
            co_await coro::sleep_for(1ms);   // let the peer's RST arrive
        }
        co_return 0;
    }());
    EXPECT_TRUE(err == EPIPE || err == ECONNRESET) << "errno " << err;
}

// A CurrentThreadExecutor with a caller-supplied parker never turns the IoDriver, so a
// driver-backed socket could never be woken there: both entry points refuse instead
// of hanging later.
TEST(TcpStreamTest, ThrowsWithoutDriver) {
    Runtime rt(std::in_place_type<CurrentThreadExecutor>,
               std::make_unique<PollingParker>([] {}));
    EXPECT_THROW((void)rt.block_on(TcpListener::bind("127.0.0.1", 0)), std::logic_error);
    EXPECT_THROW((void)rt.block_on(TcpStream::connect("127.0.0.1", 31013)), std::logic_error);
}

// The pending read shares the stream's state, so destroying the TcpStream handle
// neither closes the fd under it nor loses the wake.
TEST(TcpStreamTest, StreamDroppedWhileReadPending) {
    Runtime rt;
    rt.block_on([]() -> Coro<void> {
        auto c = co_await connect_pair("127.0.0.1", 31016);
        auto pending = coro::spawn(c.server.read(std::string(64, '\0')));
        co_await coro::sleep_for(20ms);   // let the read start waiting
        { TcpStream gone = std::move(c.server); }

        co_await c.client.write(std::string("still-open"));
        auto [n, buf] = co_await pending;
        buf.resize(n);
        EXPECT_EQ(buf, "still-open");
    }());
}

// The accept starts with nothing queued, so it waits on the driver, here woken by an
// idle WorkSharing worker turning it.
TEST(TcpStreamTest, AcceptWaitsOnWorkSharing) {
    Runtime rt(std::in_place_type<WorkSharingExecutor>, 2);
    rt.block_on([]() -> Coro<void> {
        auto listener = co_await TcpListener::bind("127.0.0.1", 31017);
        auto pending = coro::spawn(listener.accept());
        co_await coro::sleep_for(20ms);   // let the accept start waiting
        auto client = co_await TcpStream::connect("127.0.0.1", 31017);
        auto server = co_await pending;
        co_await client.write(std::string("hi"));
        auto [n, buf] = co_await server.read_exact(std::string(2, '\0'));
        EXPECT_EQ(n, 2u);
        EXPECT_EQ(buf, "hi");
    }());
}

// One stream with a write task and a read task in flight at once, likely on different
// workers: one waiter per direction.
TEST(TcpStreamTest, ConcurrentReadAndWriteOnOneStream) {
    Runtime rt(4);
    const int in_order = rt.block_on([]() -> Coro<int> {
        auto c = co_await connect_pair("127.0.0.1", 31018);
        auto echo   = coro::spawn(echo_until_eof(std::move(c.server)));
        auto client = std::make_shared<TcpStream>(std::move(c.client));

        auto rx = coro::spawn(read_numbered(client, kConcurrentMessages));
        auto tx = coro::spawn(write_numbered(client, kConcurrentMessages));
        co_await tx;
        // A lost wake shows up as a timeout, not a hang.
        auto got = co_await coro::timeout(5s, std::move(rx));
        client.reset();   // the last owner: EOF ends the echo task
        co_await coro::timeout(5s, std::move(echo));
        if (got.index() != 0) co_return -1;
        co_return std::get<0>(got).value;
    }());
    EXPECT_EQ(in_order, kConcurrentMessages);
}

#else
// ===========================================================================
// lwIP only
// ===========================================================================

// Returns at once, although the peer has sent nothing and never will.
TEST(TcpStreamTest, EmptyBufferReadReturnsZeroAtOnce) {
    NetRuntime rt;
    rt.block_on([]() -> Coro<void> {
        auto c = co_await connect_pair("127.0.0.1", 31022);
        auto [n, buf] = co_await c.client.read(std::string());
        (void)buf;
        EXPECT_EQ(n, 0u);
    }());
}

#endif // CORO_TCP_BACKEND_LWIP
