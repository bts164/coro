// TcpStream / TcpListener, on every backend: the desktop sockets on the IoDriver
// (doc/design/tcp_stream.md) and lwIP, built for the host and on the board. The tests
// connect to 127.0.0.1, which on lwIP is its loopback interface.
//
// Most tests are shared. The lwIP-only ones, over a test link (test_netif.h), and the
// desktop-only ones are in their own sections at the end.

#include <gtest/gtest.h>
#include <coro/io/tcp_listener.h>
#include <coro/io/tcp_stream.h>
#include <coro/runtime/runtime.h>
#include <coro/coro.h>
#include <coro/future.h>
#include <coro/sync/sleep.h>
#include <coro/sync/timeout.h>
#include "net_runtime.h"
#ifdef CORO_SYS_LWIP
#include <lwip/opt.h>   // TCP_SND_BUF, MEMP_NUM_TCP_PCB
#include <lwip/prot/ip.h>
#include <lwip/prot/tcp.h>
#include <coro/io/lookup_host.h>
#include <coro/io/udp_socket.h>
#include <coro/runtime/clock.h>
#include "test_netif.h"
#else
#include <coro/io/lookup_host.h>
#include <coro/runtime/work_sharing_executor.h>
#include <coro/runtime/current_thread_executor.h>
#include <coro/runtime/parker.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
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
static_assert(Future<TcpAcceptFuture>);
static_assert(!Cancellable<TcpAcceptFuture>);

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
#ifdef CORO_SYS_LWIP
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

// Returns the number of bytes that matched the pattern. The reader starts after
// `reader_delay`. The listener, the writer, binds `listen_host`, and the reader
// connects to `connect_host`.
Coro<std::size_t> large_transfer(uint16_t port, std::size_t bytes,
                                 std::chrono::milliseconds reader_delay = 0ms,
                                 std::string listen_host = "127.0.0.1",
                                 std::string connect_host = "127.0.0.1") {
    auto writer = coro::spawn(accept_and_write_pattern(
        co_await TcpListener::bind(listen_host, port), bytes));
    auto client = co_await TcpStream::connect(connect_host, port);
    if (reader_delay > 0ms) co_await coro::sleep_for(reader_delay);
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

// More than the receiver's window and the sender's buffer hold together, so that with
// nobody reading, a write of it has to wait.
#ifdef CORO_SYS_LWIP
constexpr std::size_t kBlockedWrite = kLargeTransfer;
#else
constexpr std::size_t kBlockedWrite = 64 * 1024 * 1024;   // past Linux's loopback autotuning
#endif

// Over loopback on lwIP, every connection holds two pcbs, and the board has
// MEMP_NUM_TCP_PCB of them.
#ifdef CORO_SYS_LWIP
constexpr int kQueuedConnections     = MEMP_NUM_TCP_PCB / 2;
constexpr int kSequentialConnections = 3 * MEMP_NUM_TCP_PCB;
#else
constexpr int kQueuedConnections     = 4;
constexpr int kSequentialConnections = 50;
#endif

// Reads until EOF and returns how many bytes came.
Coro<std::size_t> read_until_eof(TcpStream stream) {
    std::size_t total = 0;
    for (;;) {
        auto [n, buf] = co_await stream.read(std::vector<std::byte>(kReadChunk));
        (void)buf;
        if (n == 0) co_return total;
        total += n;
    }
}

// What the stream's next read ends in: an error, or none for EOF or data.
Coro<std::error_code> next_read_error(TcpStream stream) {
    try {
        auto [n, buf] = co_await stream.read(std::string(16, '\0'));
        (void)n;
        (void)buf;
    } catch (const std::system_error& e) {
        co_return e.code();
    }
    co_return std::error_code{};
}

constexpr int kConcurrentMessages = 1000;

// Message i is i in seven zero-padded digits and a ';': 8 bytes.
std::string numbered(int i) {
    std::string s = std::to_string(i);
    s.insert(0, 7 - s.size(), '0');
    return s + ';';
}

Coro<void> write_numbered(std::shared_ptr<TcpStream> stream, int n) {
    for (int i = 0; i < n; ++i) {
        co_await stream->write(numbered(i));
    }
}

// Returns how many of the n echoed messages came back in order.
Coro<int> read_numbered(std::shared_ptr<TcpStream> stream, int n) {
    int in_order = 0;
    for (int i = 0; i < n; ++i) {
        auto [len, buf] = co_await stream->read_exact(std::string(8, '\0'));
        if (len != 8) break;
        if (buf == numbered(i)) ++in_order;
    }
    co_return in_order;
}

// Runs `body` to completion within `bound`, so that a lost wake fails the test rather
// than hanging the run. The body checks its own results.
bool finishes_within(NetRuntime& rt, std::chrono::seconds bound, Coro<void> body) {
    return rt.block_on(coro::timeout(bound, std::move(body))).index() == 0;
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

// The reader starts late, so the writer fills the receiver's window (on lwIP; a
// desktop kernel may buffer all of it) and waits until the reads reopen it.
TEST(TcpStreamTest, SlowReaderClosesAndReopensWindow) {
    NetRuntime rt;
    auto got = rt.block_on(coro::timeout(10s, large_transfer(31023, kLargeTransfer, 200ms)));
    ASSERT_EQ(got.index(), 0u) << "timed out";
    EXPECT_EQ(std::get<0>(got).value, kLargeTransfer);
}

// With nobody reading, a write too large for the window and the send buffer waits.
// Once the peer reads, it completes, and every byte arrives.
TEST(TcpStreamTest, WriteWaitsUntilPeerReads) {
    NetRuntime rt;
    EXPECT_TRUE(finishes_within(rt, 10s, []() -> Coro<void> {
        auto c = co_await connect_pair("127.0.0.1", 31024);
        JoinHandle<std::size_t> reader;
        {
            auto write = c.client.write(std::vector<std::byte>(kBlockedWrite));
            auto early = co_await coro::timeout(100ms, coro::ref(write));
            EXPECT_EQ(early.index(), 1u) << "the write did not wait for room";
            if (early.index() == 0) co_return;   // spent: it cannot be polled again
            reader = coro::spawn(read_until_eof(std::move(c.server)));
            co_await coro::ref(write);
        }
        { TcpStream gone = std::move(c.client); }   // EOF ends the reader
        EXPECT_EQ(co_await reader, kBlockedWrite);
    }()));
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
        } catch (const std::system_error& e) {
            // Every backend reports ECONNREFUSED (on lwIP, a reset during the handshake). A catch
            // clause, unlike dynamic_cast, works in the -fno-rtti Pico build.
            refused = e.code() == std::errc::connection_refused;
        }
        EXPECT_TRUE(refused);
    }());
}

// Writing to a connection the peer closed fails with EPIPE or ECONNRESET: the peer
// resets it when the data arrives. On the desktop the write must not raise SIGPIPE,
// which would kill the test process.
TEST(TcpStreamTest, WriteToClosedPeerThrowsWithoutSigpipe) {
    NetRuntime rt;
    const auto got = rt.block_on(coro::timeout(10s, []() -> Coro<int> {
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
    }()));
    ASSERT_EQ(got.index(), 0u) << "timed out";
    const int err = std::get<0>(got).value;
    EXPECT_TRUE(err == EPIPE || err == ECONNRESET) << "errno " << err;
}

// Closing with bytes still unread resets the connection rather than ending it with a
// FIN, so the peer's read fails.
TEST(TcpStreamTest, CloseWithUnreadDataResetsPeer) {
    NetRuntime rt;
    EXPECT_TRUE(finishes_within(rt, 10s, []() -> Coro<void> {
        auto c = co_await connect_pair("127.0.0.1", 31025);
        co_await c.client.write(std::string("ab"));
        auto [n, buf] = co_await c.server.read(std::string(1, '\0'));   // "b" stays unread
        (void)buf;
        EXPECT_EQ(n, 1u);
        { TcpStream gone = std::move(c.server); }
        const std::error_code err = co_await next_read_error(std::move(c.client));
        EXPECT_TRUE(err == std::errc::connection_reset) << err.message();
    }()));
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

// The pending read shares the stream's state, so destroying the TcpStream handle
// neither closes the socket under it nor loses the wake.
TEST(TcpStreamTest, StreamDroppedWhileReadPending) {
    NetRuntime rt;
    EXPECT_TRUE(finishes_within(rt, 10s, []() -> Coro<void> {
        auto c = co_await connect_pair("127.0.0.1", 31016);
        auto pending = coro::spawn(c.server.read(std::string(64, '\0')));
        co_await coro::sleep_for(20ms);   // let the read start waiting
        { TcpStream gone = std::move(c.server); }

        co_await c.client.write(std::string("still-open"));
        auto [n, buf] = co_await pending;
        buf.resize(n);
        EXPECT_EQ(buf, "still-open");
    }()));
}

// One stream with a write task and a read task in flight at once (on the desktop,
// likely on different workers): one waiter per direction.
TEST(TcpStreamTest, ConcurrentReadAndWriteOnOneStream) {
    NetRuntime rt;
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

// ---------------------------------------------------------------------------
// Listeners and many connections
// ---------------------------------------------------------------------------

// Connections complete and queue before accept() is called, and accept() hands them
// out in the order they connected.
TEST(TcpStreamTest, BacklogQueuesConnectionsBeforeAccept) {
    NetRuntime rt;
    EXPECT_TRUE(finishes_within(rt, 10s, []() -> Coro<void> {
        auto listener = co_await TcpListener::bind("127.0.0.1", 31026);
        std::vector<TcpStream> clients;
        for (int i = 0; i < kQueuedConnections; ++i) {
            clients.push_back(co_await TcpStream::connect("127.0.0.1", 31026));
            co_await clients.back().write(std::string(1, static_cast<char>('0' + i)));
        }
        for (int i = 0; i < kQueuedConnections; ++i) {
            auto server = co_await listener.accept();
            auto [n, buf] = co_await server.read_exact(std::string(1, '\0'));
            EXPECT_EQ(n, 1u);
            EXPECT_EQ(buf[0], static_cast<char>('0' + i));
        }
    }()));
}

// A connection still queued when its listener closes is reset, as a kernel resets
// its backlog.
TEST(TcpStreamTest, ListenerDroppedResetsQueuedConnections) {
    NetRuntime rt;
    EXPECT_TRUE(finishes_within(rt, 10s, []() -> Coro<void> {
        auto listener = co_await TcpListener::bind("127.0.0.1", 31027);
        auto client   = co_await TcpStream::connect("127.0.0.1", 31027);
        { TcpListener gone = std::move(listener); }
        const std::error_code err = co_await next_read_error(std::move(client));
        EXPECT_TRUE(err == std::errc::connection_reset) << err.message();
    }()));
}

// Streams closed while their peers' data and FIN are still on the way to them. On
// lwIP those segments are queued on the loopback interface until the next service
// pass, so they arrive for pcbs already closed. The peer sees its connection end,
// and the listener goes on working.
TEST(TcpStreamTest, StreamsDroppedWithSegmentsInFlight) {
    NetRuntime rt;
    EXPECT_TRUE(finishes_within(rt, 10s, []() -> Coro<void> {
        auto listener = co_await TcpListener::bind("127.0.0.1", 31028);
        auto client_a = co_await TcpStream::connect("127.0.0.1", 31028);
        auto server_a = co_await listener.accept();
        auto client_b = co_await TcpStream::connect("127.0.0.1", 31028);
        auto server_b = co_await listener.accept();

        // None of these waits, so nothing is delivered between them.
        co_await client_b.write(std::string("b"));
        { TcpStream gone = std::move(client_b); }
        co_await client_a.write(std::string("a"));
        {
            TcpStream gone_a = std::move(server_a);
            TcpStream gone_b = std::move(server_b);
        }

        // Ended, by a reset or a FIN, whichever the backend sends.
        const std::error_code err = co_await next_read_error(std::move(client_a));
        EXPECT_TRUE(!err || err == std::errc::connection_reset) << err.message();

        auto client_c = co_await TcpStream::connect("127.0.0.1", 31028);
        auto server_c = co_await listener.accept();
        co_await client_c.write(std::string("c"));
        auto [n, buf] = co_await server_c.read_exact(std::string(1, '\0'));
        EXPECT_EQ(n, 1u);
        EXPECT_EQ(buf, "c");
    }()));
}

// Many connections one after another, each closed before the next: on the board,
// several times its pcbs, so each connect reuses the pcbs earlier connections left
// in TIME_WAIT or LAST_ACK.
TEST(TcpStreamTest, ManySequentialConnections) {
    NetRuntime rt;
    EXPECT_TRUE(finishes_within(rt, 20s, []() -> Coro<void> {
        auto listener = co_await TcpListener::bind("127.0.0.1", 31029);
        int completed = 0;
        for (int i = 0; i < kSequentialConnections; ++i) {
            auto client = co_await TcpStream::connect("127.0.0.1", 31029);
            auto server = co_await listener.accept();
            co_await client.write(std::string("x"));
            auto [n, buf] = co_await server.read_exact(std::string(1, '\0'));
            (void)buf;
            { TcpStream gone = std::move(client); }
            auto [eof, rest] = co_await server.read(std::string(1, '\0'));
            (void)rest;
            if (n == 1 && eof == 0) ++completed;
        }
        EXPECT_EQ(completed, kSequentialConnections);
    }()));
}

#ifdef CORO_SYS_LWIP
// ===========================================================================
// lwIP only: over a link (test_netif.h) rather than loopback. Frames arrive from an
// lwIP timer while the runtime sleeps, and the link can delay, lose or refuse them.
// ===========================================================================

namespace {

using Verdict = TestNetif::Verdict;
using Frame   = TestNetif::Frame;

Coro<Connection> connect_over_link(uint16_t port) {
    auto listener = co_await TcpListener::bind(TestNetif::kLocal, port);
    auto client   = co_await TcpStream::connect(TestNetif::kPeer, port);
    auto server   = co_await listener.accept();
    co_return Connection{std::move(listener), std::move(client), std::move(server)};
}

// A policy for the TCP data frames one side of the connections on `listener_port`
// sends: the listener's side, or the connecting side's. It gives `verdict` to data
// frame n, numbered from 1, where `pick(n)` is true, and delivers every other frame.
template <typename Pick>
TestNetif::Policy data_frames(uint16_t listener_port, bool from_listener, Pick pick,
                              Verdict verdict) {
    auto count = std::make_shared<int>(0);
    return [=](const Frame& f) {
        const uint16_t port = from_listener ? f.src_port : f.dst_port;
        if (f.protocol != IP_PROTO_TCP || port != listener_port || f.payload == 0)
            return Verdict::deliver();
        return pick(++*count) ? verdict : Verdict::deliver();
    };
}

} // namespace

// Every frame takes 100 ms to cross, so the runtime sleeps after each send until
// lwIP's timer delivers it. A round trip is at least four crossings: the SYN, the
// SYN-ACK, the message and the reply.
TEST(TcpStreamTest, LinkDelayWakesSleepingRuntime) {
    NetRuntime rt;
    TestNetif link;
    link.set_policy([](const Frame&) { return Verdict::deliver(100ms); });
    EXPECT_TRUE(finishes_within(rt, 10s, []() -> Coro<void> {
        const auto start = Clock::now();
        auto c = co_await connect_over_link(31030);
        co_await c.client.write(std::string("ping"));
        auto [n, buf] = co_await c.server.read_exact(std::string(4, '\0'));
        EXPECT_EQ(n, 4u);
        co_await c.server.write(std::move(buf));
        auto [m, back] = co_await c.client.read_exact(std::string(4, '\0'));
        EXPECT_EQ(back, "ping");
        // lwIP's clock counts whole milliseconds, so each crossing can end up to 1 ms
        // short of 100 ms on Clock.
        EXPECT_GE(Clock::now() - start, 396ms);
    }()));
}

// The one segment of a message is lost, and the retransmission timer sends it again.
TEST(TcpStreamTest, LostSegmentIsRetransmitted) {
    NetRuntime rt;
    TestNetif link;
    link.set_policy(data_frames(31031, false, [](int n) { return n == 1; }, Verdict::drop()));
    EXPECT_TRUE(finishes_within(rt, 15s, []() -> Coro<void> {
        auto c = co_await connect_over_link(31031);
        co_await c.client.write(std::string("hello"));
        auto [n, buf] = co_await c.server.read_exact(std::string(5, '\0'));
        EXPECT_EQ(n, 5u);
        EXPECT_EQ(buf, "hello");
    }()));
    EXPECT_EQ(link.dropped(), 1);
}

// A segment lost mid-transfer: the ones after it arrive out of order, and every byte
// still arrives once, in order.
TEST(TcpStreamTest, LostSegmentMidTransferIsRecovered) {
    NetRuntime rt;
    TestNetif link;
    link.set_policy(data_frames(31032, true, [](int n) { return n == 3; }, Verdict::drop()));
    auto got = rt.block_on(coro::timeout(20s, large_transfer(31032, kLargeTransfer, 0ms,
                                                             TestNetif::kLocal, TestNetif::kPeer)));
    ASSERT_EQ(got.index(), 0u) << "timed out";
    EXPECT_EQ(std::get<0>(got).value, kLargeTransfer);
    EXPECT_EQ(link.dropped(), 1);
}

// The reader starts late, so its window closes. Every window update it sends while
// closed is lost, and only the writer's persist timer, probing the closed window,
// learns that it has opened.
TEST(TcpStreamTest, PersistTimerRecoversLostWindowUpdates) {
    struct Window {
        bool closed  = false;
        int  probes  = 0;   // writer frames sent into a closed window
        int  updates = 0;   // window updates lost
    };
    constexpr uint16_t kPort = 31033;   // the writer's
    NetRuntime rt;
    TestNetif link;
    auto window = std::make_shared<Window>();
    link.set_policy([window](const Frame& f) {
        if (f.protocol != IP_PROTO_TCP) return Verdict::deliver();
        if (f.src_port == kPort) {
            if (window->closed) ++window->probes;
            window->closed = false;   // the reply to a probe gets through
            return Verdict::deliver();
        }
        if (f.window == 0) {
            window->closed = true;
        } else if (window->closed && f.payload == 0 &&
                   (f.tcp_flags & (TCP_SYN | TCP_FIN | TCP_RST)) == 0) {
            ++window->updates;
            return Verdict::drop();
        }
        return Verdict::deliver();
    });
    auto got = rt.block_on(coro::timeout(30s, large_transfer(kPort, kLargeTransfer, 500ms,
                                                             TestNetif::kLocal, TestNetif::kPeer)));
    ASSERT_EQ(got.index(), 0u) << "timed out";
    EXPECT_EQ(std::get<0>(got).value, kLargeTransfer);
    EXPECT_GE(window->updates, 1) << "the window never closed";
    EXPECT_GE(window->probes, 1);
}

// The link refuses a message's segment three times, as a driver out of buffers
// does. lwIP keeps it unsent, nothing is in flight to bring a `sent` callback, and
// the poll callback's retry sends it.
TEST(TcpStreamTest, RefusedSegmentIsSentAgain) {
    NetRuntime rt;
    TestNetif link;
    link.set_policy(data_frames(31035, false, [](int n) { return n <= 3; }, Verdict::refuse()));
    EXPECT_TRUE(finishes_within(rt, 15s, []() -> Coro<void> {
        auto c = co_await connect_over_link(31035);
        co_await c.client.write(std::string("hello"));
        auto [n, buf] = co_await c.server.read_exact(std::string(5, '\0'));
        EXPECT_EQ(n, 5u);
        EXPECT_EQ(buf, "hello");
    }()));
    EXPECT_EQ(link.refused(), 3);
}

// A connect nobody answers waits until timeout() drops it, which aborts its pcb.
// The SYN is not sent again, so a listener that appears afterwards accepts nothing
// until a new connect.
TEST(TcpStreamTest, UnansweredConnectEndsAtTimeout) {
    NetRuntime rt;
    TestNetif link;
    link.set_policy([](const Frame&) { return Verdict::drop(); });
    EXPECT_TRUE(finishes_within(rt, 15s, [](TestNetif& link) -> Coro<void> {
        auto attempt = co_await coro::timeout(500ms, TcpStream::connect(TestNetif::kPeer, 31034));
        EXPECT_EQ(attempt.index(), 1u) << "connected with every frame dropped";
        EXPECT_GE(link.dropped(), 1);

        link.set_policy(nullptr);
        auto listener = co_await TcpListener::bind(TestNetif::kLocal, 31034);
        // lwIP resends a SYN after 3 s: a pcb left behind would connect within this.
        auto stray = co_await coro::timeout(5s, listener.accept());
        EXPECT_EQ(stray.index(), 1u) << "the dropped connect's SYN was sent again";
        auto client = co_await TcpStream::connect(TestNetif::kPeer, 31034);
        auto server = co_await listener.accept();
        (void)client;
        (void)server;
    }(link)));
}

// ===========================================================================
// lwIP only: without a network. A PicoNetwork::None runtime never starts lwIP, and
// a PicoNetwork::Lwip one has no DNS server: nothing configures one without DHCP.
// ===========================================================================

namespace {

// Awaits `op` and returns the error it threw, or a default error_code.
template <typename Op>
Coro<std::error_code> error_of(Op op) {
    try {
        auto result = co_await std::move(op);
        (void)result;
    } catch (const std::system_error& e) {
        co_return e.code();
    }
    co_return std::error_code{};
}

} // namespace

// Every socket call fails at once with ENETDOWN rather than touching a stopped
// lwIP. A numeric lookup_host needs no network, so it still answers.
TEST(TcpStreamTest, NoneModeSocketsThrowEnetdown) {
    Runtime rt(PicoNetwork::None);
    auto done = rt.block_on(coro::timeout(5s, []() -> Coro<void> {
        auto connect = co_await error_of(TcpStream::connect("127.0.0.1", 31035));
        EXPECT_EQ(connect, std::errc::network_down) << connect.message();
        auto bind = co_await error_of(UdpSocket::bind("127.0.0.1", 0));
        EXPECT_EQ(bind, std::errc::network_down) << bind.message();
        auto lookup = co_await error_of(lookup_host("example.invalid", 80));
        EXPECT_EQ(lookup, std::errc::network_down) << lookup.message();

        auto numeric = co_await lookup_host("127.0.0.1", 80);
        EXPECT_EQ(numeric, std::vector{SocketAddress::parse("127.0.0.1", 80).value()});
    }()));
    EXPECT_EQ(done.index(), 0u) << "timed out";
}

// With no DNS server, a name fails at once as not found rather than waiting on
// lwIP's retries. A numeric address still resolves to itself.
TEST(TcpStreamTest, LookupWithoutDnsServerFailsPromptly) {
    NetRuntime rt;
    EXPECT_TRUE(finishes_within(rt, 2s, []() -> Coro<void> {
        auto code = co_await error_of(lookup_host("example.invalid", 80));
        EXPECT_EQ(&code.category(), &dns_error_category()) << code.message();

        auto numeric = co_await lookup_host("127.0.0.1", 80);
        EXPECT_EQ(numeric, std::vector{SocketAddress::parse("127.0.0.1", 80).value()});
    }()));
}

#else
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

// A CurrentThreadExecutor with a caller-supplied parker never turns the IoDriver, so a
// driver-backed socket could never be woken there: both entry points refuse instead
// of hanging later.
TEST(TcpStreamTest, ThrowsWithoutDriver) {
    Runtime rt(std::in_place_type<CurrentThreadExecutor>,
               std::make_unique<PollingParker>([] {}));
    EXPECT_THROW((void)rt.block_on(TcpListener::bind("127.0.0.1", 0)), std::logic_error);
    EXPECT_THROW((void)rt.block_on(TcpStream::connect("127.0.0.1", 31013)), std::logic_error);
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

#endif // CORO_SYS_LWIP

