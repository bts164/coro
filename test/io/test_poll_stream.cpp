#include <gtest/gtest.h>
#include <coro/io/poll_stream.h>
#include <coro/io/decoder_stream.h>
#include <coro/io/byte_source.h>
#include <coro/runtime/runtime.h>
#include <coro/task/spawn_blocking.h>
#include <coro/coro.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

using namespace coro;

// ---------------------------------------------------------------------------
// MockMessage
// ---------------------------------------------------------------------------

struct MockMessage {
    uint32_t id;
    uint32_t data;
};

// ---------------------------------------------------------------------------
// fixed_decoder: one MockMessage per sizeof(MockMessage) bytes
// ---------------------------------------------------------------------------

DecoderStream<MockMessage> fixed_decoder(ByteSource& src) {
    for (;;) {
        MockMessage msg;
        if (co_await src.memcpy(&msg, sizeof(msg)) == 0) co_return;
        co_yield msg;
    }
}

// ---------------------------------------------------------------------------
// throwing_decoder: throws a runtime_error on the Nth packet
// ---------------------------------------------------------------------------

DecoderStream<MockMessage> throwing_decoder(ByteSource& src, int throw_on) {
    int count = 0;
    for (;;) {
        MockMessage msg;
        if (co_await src.memcpy(&msg, sizeof(msg)) == 0) co_return;
        if (++count == throw_on)
            throw std::runtime_error("framing error on packet " + std::to_string(count));
        co_yield msg;
    }
}

// ---------------------------------------------------------------------------
// Pipe helper
// ---------------------------------------------------------------------------

struct PipePair {
    int read_fd  = -1;
    int write_fd = -1;

    PipePair() {
        int fds[2];
        if (pipe(fds) != 0) throw std::runtime_error("pipe() failed");
        read_fd  = fds[0];
        write_fd = fds[1];
        int flags = fcntl(read_fd, F_GETFL);
        fcntl(read_fd, F_SETFL, flags | O_NONBLOCK);
    }

    ~PipePair() {
        if (read_fd  >= 0) ::close(read_fd);
        if (write_fd >= 0) ::close(write_fd);
    }

    void write_message(const MockMessage& msg) {
        // MockMessage is well under PIPE_BUF, so POSIX guarantees this write is atomic
        // (no short write) as long as it doesn't fail outright.
        ssize_t n = ::write(write_fd, &msg, sizeof(msg));
        if (n != static_cast<ssize_t>(sizeof(msg)))
            throw std::system_error(errno, std::generic_category(), "write_message: write() failed");
    }

    void close_write_end() {
        if (write_fd >= 0) { ::close(write_fd); write_fd = -1; }
    }
};

// ---------------------------------------------------------------------------
// Concept check
// ---------------------------------------------------------------------------

static_assert(Stream<PollStream<MockMessage>>);

// ---------------------------------------------------------------------------
// Basic stream operations
// ---------------------------------------------------------------------------

TEST(PollStreamTest, OpenAndClose) {
    PipePair pipe;
    Runtime rt(1);
    rt.block_on([](int fd) -> Coro<void> {
        auto stream = PollStream<MockMessage>::open(fd, fixed_decoder);
        stream.close();
        co_return;
    }(pipe.read_fd));
}

TEST(PollStreamTest, ReadSingleMessage) {
    PipePair pipe;
    pipe.write_message({42, 100});
    pipe.close_write_end();

    std::optional<MockMessage> received;
    Runtime rt(1);
    rt.block_on([](int fd, std::optional<MockMessage>& out) -> Coro<void> {
        auto stream = PollStream<MockMessage>::open(fd, fixed_decoder);
        out = co_await next(stream);
    }(pipe.read_fd, received));

    ASSERT_TRUE(received.has_value());
    EXPECT_EQ(received->id,   42u);
    EXPECT_EQ(received->data, 100u);
}

TEST(PollStreamTest, ReadMultipleMessages) {
    PipePair pipe;
    for (uint32_t i = 0; i < 5; i++) pipe.write_message({i, i * 10});
    pipe.close_write_end();

    std::vector<MockMessage> received;
    Runtime rt(1);
    rt.block_on([](int fd, std::vector<MockMessage>& out) -> Coro<void> {
        auto stream = PollStream<MockMessage>::open(fd, fixed_decoder);
        while (auto item = co_await next(stream)) out.push_back(*item);
    }(pipe.read_fd, received));

    ASSERT_EQ(received.size(), 5u);
    for (uint32_t i = 0; i < 5; i++) {
        EXPECT_EQ(received[i].id,   i);
        EXPECT_EQ(received[i].data, i * 10);
    }
}

TEST(PollStreamTest, ReadEOF) {
    PipePair pipe;
    pipe.write_message({1, 2});
    pipe.close_write_end();

    int count = 0;
    Runtime rt(1);
    rt.block_on([](int fd, int& count) -> Coro<void> {
        auto stream = PollStream<MockMessage>::open(fd, fixed_decoder);
        while (auto item = co_await next(stream)) ++count;
    }(pipe.read_fd, count));

    EXPECT_EQ(count, 1);
}

// ---------------------------------------------------------------------------
// Backpressure — Block mode
// ---------------------------------------------------------------------------

TEST(PollStreamTest, Backpressure_PacketBufferFull) {
    PipePair pipe;
    constexpr uint32_t N = 20;
    for (uint32_t i = 0; i < N; i++) pipe.write_message({i, i});
    pipe.close_write_end();

    std::vector<uint32_t> ids;
    Runtime rt(1);
    rt.block_on([](int fd, std::vector<uint32_t>& ids) -> Coro<void> {
        auto stream = PollStream<MockMessage>::open(fd,
            PollStreamOptions{.packet_buffer_capacity = 4,
                              .backpressure           = BackpressureMode::Block},
            fixed_decoder);
        while (auto item = co_await next(stream)) ids.push_back(item->id);
    }(pipe.read_fd, ids));

    ASSERT_EQ(ids.size(), N);
    for (uint32_t i = 0; i < N; i++) EXPECT_EQ(ids[i], i) << "at index " << i;
}

// ---------------------------------------------------------------------------
// Backpressure — Overrun mode
// ---------------------------------------------------------------------------

TEST(PollStreamTest, Overrun_DropsOldestAndCountsMissed) {
    PipePair pipe;
    for (uint32_t i = 0; i < 8; i++) pipe.write_message({i, i});
    pipe.close_write_end();

    bool got_overrun   = false;
    std::size_t missed = 0;
    std::vector<uint32_t> ids;

    Runtime rt(1);
    rt.block_on([](int fd, bool& got_overrun, std::size_t& missed, std::vector<uint32_t>& ids) -> Coro<void> {
        auto stream = PollStream<MockMessage>::open(fd,
            PollStreamOptions{.packet_buffer_capacity = 4,
                              .backpressure           = BackpressureMode::Overrun},
            fixed_decoder);
        try { co_await next(stream); }
        catch (const PollStreamOverrunError& e) { got_overrun = true; missed = e.missed(); }

        while (auto item = co_await next(stream)) ids.push_back(item->id);
    }(pipe.read_fd, got_overrun, missed, ids));

    EXPECT_TRUE(got_overrun);
    EXPECT_EQ(missed, 4u);
    ASSERT_EQ(ids.size(), 4u);
    for (uint32_t i = 0; i < 4; i++) EXPECT_EQ(ids[i], i + 4) << "surviving packet " << i;
}

TEST(PollStreamTest, Overrun_NoErrorWhenNoMisses) {
    PipePair pipe;
    int count       = 0;
    bool got_overrun = false;

    Runtime rt(1);
    rt.block_on([](int fd, PipePair& pipe, int& count, bool& got_overrun) -> Coro<void> {
        auto stream = PollStream<MockMessage>::open(fd,
            PollStreamOptions{.packet_buffer_capacity = 4,
                              .backpressure           = BackpressureMode::Overrun},
            fixed_decoder);

        auto writer = spawn_blocking([&pipe] {
            for (uint32_t i = 0; i < 3; i++) {
                pipe.write_message({i, i});
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            pipe.close_write_end();
        });

        try { while (auto item = co_await next(stream)) ++count; }
        catch (const PollStreamOverrunError&) { got_overrun = true; }

        co_await writer;
    }(pipe.read_fd, pipe, count, got_overrun));

    EXPECT_FALSE(got_overrun);
    EXPECT_EQ(count, 3);
}

TEST(PollStreamTest, Overrun_ContinuesAfterError) {
    PipePair pipe;
    for (uint32_t i = 0; i < 8; i++) pipe.write_message({i, i});

    bool got_overrun = false;
    std::vector<uint32_t> ids;

    Runtime rt(1);
    rt.block_on([](int fd, PipePair& pipe, bool& got_overrun, std::vector<uint32_t>& ids) -> Coro<void> {
        auto stream = PollStream<MockMessage>::open(fd,
            PollStreamOptions{.packet_buffer_capacity = 4,
                              .backpressure           = BackpressureMode::Overrun},
            fixed_decoder);

        try { co_await next(stream); }
        catch (const PollStreamOverrunError&) { got_overrun = true; }

        for (int i = 0; i < 4; i++) {
            auto item = co_await next(stream);
            if (item) ids.push_back(item->id);
        }

        auto writer = spawn_blocking([&pipe] {
            pipe.write_message({100, 100});
            pipe.write_message({101, 101});
            pipe.close_write_end();
        });

        while (auto item = co_await next(stream)) ids.push_back(item->id);
        co_await writer;
    }(pipe.read_fd, pipe, got_overrun, ids));

    EXPECT_TRUE(got_overrun);
    ASSERT_EQ(ids.size(), 6u);
    EXPECT_EQ(ids[0], 4u);  EXPECT_EQ(ids[1], 5u);
    EXPECT_EQ(ids[2], 6u);  EXPECT_EQ(ids[3], 7u);
    EXPECT_EQ(ids[4], 100u); EXPECT_EQ(ids[5], 101u);
}

// ---------------------------------------------------------------------------
// Error handling
// ---------------------------------------------------------------------------

TEST(PollStreamTest, DecoderThrows_StreamFaults) {
    PipePair pipe;
    for (uint32_t i = 0; i < 5; i++) pipe.write_message({i, i});
    pipe.close_write_end();

    std::vector<uint32_t> ids;
    std::exception_ptr caught;

    Runtime rt(1);
    rt.block_on([](int fd, std::vector<uint32_t>& ids, std::exception_ptr& caught) -> Coro<void> {
        auto stream = PollStream<MockMessage>::open(fd, throwing_decoder, 3);
        try { while (auto item = co_await next(stream)) ids.push_back(item->id); }
        catch (...) { caught = std::current_exception(); }
    }(pipe.read_fd, ids, caught));

    ASSERT_GE(ids.size(), 2u);
    EXPECT_EQ(ids[0], 0u);
    EXPECT_EQ(ids[1], 1u);
    ASSERT_NE(caught, nullptr);
    EXPECT_THROW(std::rethrow_exception(caught), std::runtime_error);
}

TEST(PollStreamTest, ReadError_IOFailure) {
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = 0;

    int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(listen_fd, 0);
    int one = 1;
    ::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    ASSERT_EQ(::bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
    ASSERT_EQ(::listen(listen_fd, 1), 0);
    socklen_t alen = sizeof(addr);
    ::getsockname(listen_fd, reinterpret_cast<sockaddr*>(&addr), &alen);

    int client_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(client_fd, 0);
    ASSERT_EQ(::connect(client_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
    ::fcntl(client_fd, F_SETFL, O_NONBLOCK);

    int conn_fd = ::accept(listen_fd, nullptr, nullptr);
    ASSERT_GE(conn_fd, 0);
    ::close(listen_fd);

    struct linger lng = {1, 0};
    ::setsockopt(conn_fd, SOL_SOCKET, SO_LINGER, &lng, sizeof(lng));
    ::close(conn_fd);

    std::exception_ptr caught;
    {
        // Runtime must be destroyed (joining the uv thread, which finishes
        // PollStream::close()'s detached close_impl task) before client_fd is
        // closed directly below — otherwise the uv thread's epoll_ctl teardown
        // races with the close() on the main thread (caught by TSan as a real
        // fd-lifetime race, not a false positive).
        Runtime rt(1);
        rt.block_on([](int fd, std::exception_ptr& out) -> Coro<void> {
            auto stream = PollStream<MockMessage>::open(fd, fixed_decoder);
            try { co_await next(stream); }
            catch (...) { out = std::current_exception(); }
        }(client_fd, caught));
    }

    ::close(client_fd);
    ASSERT_NE(caught, nullptr);
    EXPECT_THROW(std::rethrow_exception(caught), std::system_error);
}

// ---------------------------------------------------------------------------
// Partial packet handling
// ---------------------------------------------------------------------------

TEST(PollStreamTest, MessageSpansMultipleReads) {
    PipePair pipe;
    MockMessage msg{42, 99};
    ssize_t n1 = ::write(pipe.write_fd, &msg, sizeof(msg) / 2);
    if (n1 != static_cast<ssize_t>(sizeof(msg) / 2))
        throw std::system_error(errno, std::generic_category(), "write() failed");

    std::optional<MockMessage> received;
    Runtime rt(1);
    rt.block_on([](int fd, PipePair& pipe, MockMessage& msg, std::optional<MockMessage>& out) -> Coro<void> {
        auto stream = PollStream<MockMessage>::open(fd, fixed_decoder);

        auto writer = spawn_blocking([&pipe, &msg] {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            const auto* ptr = reinterpret_cast<const char*>(&msg);
            ssize_t n2 = ::write(pipe.write_fd, ptr + sizeof(msg) / 2, sizeof(msg) - sizeof(msg) / 2);
            if (n2 != static_cast<ssize_t>(sizeof(msg) - sizeof(msg) / 2))
                throw std::system_error(errno, std::generic_category(), "write() failed");
        });

        out = co_await next(stream);
        co_await writer;
    }(pipe.read_fd, pipe, msg, received));

    ASSERT_TRUE(received.has_value());
    EXPECT_EQ(received->id,   42u);
    EXPECT_EQ(received->data, 99u);
}

// ---------------------------------------------------------------------------
// Multiple independent streams
// ---------------------------------------------------------------------------

TEST(PollStreamTest, MultipleStreamsIndependent) {
    constexpr int      STREAMS         = 3;
    constexpr uint32_t MSGS_PER_STREAM = 4;

    struct Pair { PipePair pipe; std::vector<uint32_t> received; };
    std::array<Pair, STREAMS> pairs;

    for (int s = 0; s < STREAMS; s++) {
        for (uint32_t m = 0; m < MSGS_PER_STREAM; m++)
            pairs[s].pipe.write_message({static_cast<uint32_t>(s * 100 + m), 0});
        pairs[s].pipe.close_write_end();
    }

    Runtime rt(1);
    rt.block_on([](std::array<Pair, STREAMS>& pairs) -> Coro<void> {
        auto read_stream = [](int fd, std::vector<uint32_t>& out) -> Coro<void> {
            auto stream = PollStream<MockMessage>::open(fd, fixed_decoder);
            while (auto item = co_await next(stream))
                out.push_back(item->id);
        };

        auto h0 = spawn(read_stream(pairs[0].pipe.read_fd, pairs[0].received));
        auto h1 = spawn(read_stream(pairs[1].pipe.read_fd, pairs[1].received));
        auto h2 = spawn(read_stream(pairs[2].pipe.read_fd, pairs[2].received));
        co_await h0; co_await h1; co_await h2;
    }(pairs));

    for (int s = 0; s < STREAMS; s++) {
        ASSERT_EQ(pairs[s].received.size(), MSGS_PER_STREAM) << "stream " << s;
        for (uint32_t m = 0; m < MSGS_PER_STREAM; m++)
            EXPECT_EQ(pairs[s].received[m], static_cast<uint32_t>(s * 100 + m))
                << "stream " << s << " msg " << m;
    }
}
