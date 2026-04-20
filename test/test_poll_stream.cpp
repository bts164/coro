#include <gtest/gtest.h>
#include <coro/io/poll_stream.h>
#include <coro/io/decoder_concept.h>
#include <coro/runtime/runtime.h>
#include <coro/task/spawn_blocking.h>
#include <coro/coro.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <array>
#include <chrono>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace coro;

// ---------------------------------------------------------------------------
// Fixed-size decoder: one MockMessage per decode call
// ---------------------------------------------------------------------------

struct MockMessage {
    uint32_t id;
    uint32_t data;
};

class FixedSizeDecoder {
public:
    using OutputType = MockMessage;

    std::optional<MockMessage> decode(std::span<const std::byte> buffer,
                                      std::size_t& consumed) {
        if (buffer.size() < sizeof(MockMessage)) {
            consumed = 0;
            return std::nullopt;
        }
        MockMessage msg;
        std::memcpy(&msg, buffer.data(), sizeof(MockMessage));
        consumed = sizeof(MockMessage);
        return msg;
    }

    void reset() {}
};

static_assert(Decoder<FixedSizeDecoder>);

// ---------------------------------------------------------------------------
// Decoder that throws a std::runtime_error on its Nth decode call
// ---------------------------------------------------------------------------

class ThrowingDecoder {
    int m_count    = 0;
    int m_throw_on;
public:
    using OutputType = MockMessage;

    explicit ThrowingDecoder(int throw_on) : m_throw_on(throw_on) {}

    std::optional<MockMessage> decode(std::span<const std::byte> buffer,
                                      std::size_t& consumed) {
        if (buffer.size() < sizeof(MockMessage)) {
            consumed = 0;
            return std::nullopt;
        }
        if (++m_count == m_throw_on) {
            throw std::runtime_error("framing error on packet " + std::to_string(m_count));
        }
        MockMessage msg;
        std::memcpy(&msg, buffer.data(), sizeof(MockMessage));
        consumed = sizeof(MockMessage);
        return msg;
    }

    void reset() { m_count = 0; }
};

static_assert(Decoder<ThrowingDecoder>);

// ---------------------------------------------------------------------------
// Helper: owned pipe pair (read end set non-blocking)
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
        std::cout<<"PipePair destroyed, fds closed" << std::endl;
    }

    void write_message(const MockMessage& msg) {
        ::write(write_fd, &msg, sizeof(msg));
    }

    void close_write_end() {
        if (write_fd >= 0) { ::close(write_fd); write_fd = -1; }
    }
};

// ---------------------------------------------------------------------------
// Concept checks (compile-time)
// ---------------------------------------------------------------------------

static_assert(Stream<PollStream<MockMessage, FixedSizeDecoder>>);

// ---------------------------------------------------------------------------
// Basic stream operations
// ---------------------------------------------------------------------------

TEST(PollStreamTest, OpenAndClose) {
    PipePair pipe;
    Runtime rt(1);
    rt.block_on([&]() -> Coro<void> {
        auto stream = PollStream<MockMessage, FixedSizeDecoder>::open(
            pipe.read_fd, FixedSizeDecoder{});
        stream.close();
        std::cout << "Stream opened and closed successfully" << std::endl;
        co_return;
    }());
    std::cout << "Test completed" << std::endl;
}

TEST(PollStreamTest, ReadSingleMessage) {
    PipePair pipe;
    pipe.write_message({42, 100});
    pipe.close_write_end();

    std::optional<MockMessage> received;
    Runtime rt(1);
    rt.block_on([&]() -> Coro<void> {
        auto stream = PollStream<MockMessage, FixedSizeDecoder>::open(
            pipe.read_fd, FixedSizeDecoder{});
        received = co_await next(stream);
    }());

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
    rt.block_on([&]() -> Coro<void> {
        auto stream = PollStream<MockMessage, FixedSizeDecoder>::open(
            pipe.read_fd, FixedSizeDecoder{});
        while (auto item = co_await next(stream)) received.push_back(*item);
    }());

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
    rt.block_on([&]() -> Coro<void> {
        auto stream = PollStream<MockMessage, FixedSizeDecoder>::open(
            pipe.read_fd, FixedSizeDecoder{});
        while (auto item = co_await next(stream)) ++count;
    }());

    EXPECT_EQ(count, 1);
}

// ---------------------------------------------------------------------------
// Backpressure — Block mode (default)
// ---------------------------------------------------------------------------

TEST(PollStreamTest, Backpressure_PacketBufferFull) {
    // Write more messages than the packet buffer capacity before the stream starts.
    // Block mode must stop polling when the buffer fills and resume as it drains,
    // delivering all packets in order with no loss.
    PipePair pipe;
    constexpr uint32_t N = 20;
    for (uint32_t i = 0; i < N; i++) pipe.write_message({i, i});
    pipe.close_write_end();

    std::vector<uint32_t> ids;
    Runtime rt(1);
    rt.block_on([&]() -> Coro<void> {
        auto stream = PollStream<MockMessage, FixedSizeDecoder>::open(
            pipe.read_fd, FixedSizeDecoder{},
            PollStreamOptions{.packet_buffer_capacity = 4,
                              .backpressure           = BackpressureMode::Block});
        while (auto item = co_await next(stream)) ids.push_back(item->id);
    }());

    ASSERT_EQ(ids.size(), N);
    for (uint32_t i = 0; i < N; i++) EXPECT_EQ(ids[i], i) << "at index " << i;
}

// ---------------------------------------------------------------------------
// Backpressure — Overrun mode
// ---------------------------------------------------------------------------

TEST(PollStreamTest, Overrun_DropsOldestAndCountsMissed) {
    // Write 8 messages before the stream starts with a capacity-4 Overrun buffer.
    // poll_cb decodes all 8 and evicts the 4 oldest (ids 0–3, overrun_count = 4).
    // Expected delivery: PollStreamOverrunError{4}, then packets 4–7, then EOF.
    PipePair pipe;
    for (uint32_t i = 0; i < 8; i++) pipe.write_message({i, i});
    pipe.close_write_end();

    bool got_overrun       = false;
    std::size_t missed     = 0;
    std::vector<uint32_t> ids;
    bool got_eof           = false;

    Runtime rt(1);
    rt.block_on([&]() -> Coro<void> {
        auto stream = PollStream<MockMessage, FixedSizeDecoder>::open(
            pipe.read_fd, FixedSizeDecoder{},
            PollStreamOptions{.packet_buffer_capacity = 4,
                              .backpressure           = BackpressureMode::Overrun});

        try {
            co_await next(stream);
        } catch (const PollStreamOverrunError& e) {
            got_overrun = true;
            missed      = e.missed();
        }

        while (auto item = co_await next(stream)) ids.push_back(item->id);
        got_eof = true;
    }());

    EXPECT_TRUE(got_overrun);
    EXPECT_EQ(missed, 4u);
    ASSERT_EQ(ids.size(), 4u);
    for (uint32_t i = 0; i < 4; i++) EXPECT_EQ(ids[i], i + 4) << "surviving packet " << i;
    EXPECT_TRUE(got_eof);
}

TEST(PollStreamTest, Overrun_NoErrorWhenNoMisses) {
    // Write messages one at a time from a background thread, reading each before
    // the next arrives. The packet buffer never fills so no overrun occurs.
    PipePair pipe;

    int  count       = 0;
    bool got_overrun = false;

    Runtime rt(1);
    rt.block_on([&]() -> Coro<void> {
        auto stream = PollStream<MockMessage, FixedSizeDecoder>::open(
            pipe.read_fd, FixedSizeDecoder{},
            PollStreamOptions{.packet_buffer_capacity = 4,
                              .backpressure           = BackpressureMode::Overrun});

        auto writer = spawn_blocking([&pipe] {
            for (uint32_t i = 0; i < 3; i++) {
                pipe.write_message({i, i});
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            pipe.close_write_end();
        });

        try {
            while (auto item = co_await next(stream)) ++count;
        } catch (const PollStreamOverrunError&) {
            got_overrun = true;
        }

        co_await writer;
    }());

    EXPECT_FALSE(got_overrun);
    EXPECT_EQ(count, 3);
}

TEST(PollStreamTest, Overrun_ContinuesAfterError) {
    // Trigger one overrun, verify stream stays open, then write and read additional
    // messages to confirm normal operation resumes after the error notification.
    PipePair pipe;
    for (uint32_t i = 0; i < 8; i++) pipe.write_message({i, i});
    // write end left open so we can write more messages later

    bool got_overrun          = false;
    std::vector<uint32_t> ids;
    bool got_eof              = false;

    Runtime rt(1);
    rt.block_on([&]() -> Coro<void> {
        auto stream = PollStream<MockMessage, FixedSizeDecoder>::open(
            pipe.read_fd, FixedSizeDecoder{},
            PollStreamOptions{.packet_buffer_capacity = 4,
                              .backpressure           = BackpressureMode::Overrun});

        // Should get overrun notification from the initial burst of 8 packets
        try {
            co_await next(stream);
        } catch (const PollStreamOverrunError&) {
            got_overrun = true;
        }

        // Read the 4 surviving packets
        for (int i = 0; i < 4; i++) {
            auto item = co_await next(stream);
            if (item) ids.push_back(item->id);
        }

        // Write 2 more messages and signal EOF from a background thread
        auto writer = spawn_blocking([&pipe] {
            pipe.write_message({100, 100});
            pipe.write_message({101, 101});
            pipe.close_write_end();
        });

        // Read the new messages and EOF
        while (auto item = co_await next(stream)) ids.push_back(item->id);
        got_eof = true;

        co_await writer;
    }());

    EXPECT_TRUE(got_overrun);
    // Surviving packets from first burst are ids 4–7; then 100, 101
    ASSERT_EQ(ids.size(), 6u);
    EXPECT_EQ(ids[0], 4u);
    EXPECT_EQ(ids[1], 5u);
    EXPECT_EQ(ids[2], 6u);
    EXPECT_EQ(ids[3], 7u);
    EXPECT_EQ(ids[4], 100u);
    EXPECT_EQ(ids[5], 101u);
    EXPECT_TRUE(got_eof);
}

// ---------------------------------------------------------------------------
// Error handling
// ---------------------------------------------------------------------------

TEST(PollStreamTest, DecoderThrows_StreamFaults) {
    // ThrowingDecoder throws on its 3rd decode call. Packets 0 and 1 are already
    // in the packet buffer when the throw occurs and must be delivered before the
    // error surfaces (packets-before-error ordering guarantee).
    PipePair pipe;
    for (uint32_t i = 0; i < 5; i++) pipe.write_message({i, i});
    pipe.close_write_end();

    std::vector<uint32_t> ids;
    std::exception_ptr    caught;

    Runtime rt(1);
    rt.block_on([&]() -> Coro<void> {
        auto stream = PollStream<MockMessage, ThrowingDecoder>::open(
            pipe.read_fd, ThrowingDecoder{3});
        try {
            while (auto item = co_await next(stream)) ids.push_back(item->id);
        } catch (...) {
            caught = std::current_exception();
        }
    }());

    // Packets decoded before the throw arrive first
    ASSERT_GE(ids.size(), 2u);
    EXPECT_EQ(ids[0], 0u);
    EXPECT_EQ(ids[1], 1u);

    // The throw becomes a fatal error on the stream
    ASSERT_NE(caught, nullptr);
    EXPECT_THROW(std::rethrow_exception(caught), std::runtime_error);
}

TEST(PollStreamTest, ReadError_IOFailure) {
    // Establish a TCP loopback connection. Setting SO_LINGER=0 on the server side
    // and closing it sends RST, causing ECONNRESET (a genuine read() error) on the
    // client fd that PollStream is watching.
    sockaddr_in addr{};
    addr.sin_family           = AF_INET;
    addr.sin_addr.s_addr      = htonl(INADDR_LOOPBACK);
    addr.sin_port             = 0;

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

    // Close with linger=0: sends RST to client → client gets ECONNRESET on next read
    struct linger lng = {1, 0};
    ::setsockopt(conn_fd, SOL_SOCKET, SO_LINGER, &lng, sizeof(lng));
    ::close(conn_fd);

    std::exception_ptr caught;
    Runtime rt(1);
    rt.block_on([&]() -> Coro<void> {
        auto stream = PollStream<MockMessage, FixedSizeDecoder>::open(
            client_fd, FixedSizeDecoder{});
        try {
            co_await next(stream);
        } catch (...) {
            caught = std::current_exception();
        }
    }());

    ::close(client_fd);

    ASSERT_NE(caught, nullptr);
    EXPECT_THROW(std::rethrow_exception(caught), std::system_error);
}

// ---------------------------------------------------------------------------
// Partial packet handling
// ---------------------------------------------------------------------------

TEST(PollStreamTest, MessageSpansMultipleReads) {
    // Write only the first half of a message to the pipe. The stream reads those
    // bytes but cannot decode a complete packet, so it suspends. A background thread
    // writes the second half; poll_cb fires again, decodes the full message, and
    // wakes the consumer.
    PipePair pipe;
    MockMessage msg{42, 99};
    ::write(pipe.write_fd, &msg, sizeof(msg) / 2);

    std::optional<MockMessage> received;
    Runtime rt(1);
    rt.block_on([&]() -> Coro<void> {
        auto stream = PollStream<MockMessage, FixedSizeDecoder>::open(
            pipe.read_fd, FixedSizeDecoder{});

        auto writer = spawn_blocking([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            const auto* ptr = reinterpret_cast<const char*>(&msg);
            ::write(pipe.write_fd, ptr + sizeof(msg) / 2, sizeof(msg) - sizeof(msg) / 2);
        });

        received = co_await next(stream);

        co_await writer;
    }());

    ASSERT_TRUE(received.has_value());
    EXPECT_EQ(received->id,   42u);
    EXPECT_EQ(received->data, 99u);
}

// ---------------------------------------------------------------------------
// Multiple independent streams
// ---------------------------------------------------------------------------

TEST(PollStreamTest, MultipleStreamsIndependent) {
    // Three independent pipe pairs, each with distinct message IDs.
    // All three streams run concurrently and must receive only their own data.
    constexpr int      STREAMS          = 3;
    constexpr uint32_t MSGS_PER_STREAM  = 4;

    struct Pair {
        PipePair              pipe;
        std::vector<uint32_t> received;
    };
    std::array<Pair, STREAMS> pairs;

    for (int s = 0; s < STREAMS; s++) {
        for (uint32_t m = 0; m < MSGS_PER_STREAM; m++) {
            pairs[s].pipe.write_message({static_cast<uint32_t>(s * 100 + m), 0});
        }
        pairs[s].pipe.close_write_end();
    }

    Runtime rt(1);
    rt.block_on([&]() -> Coro<void> {
        auto read_stream = [&](int s) -> Coro<void> {
            auto stream = PollStream<MockMessage, FixedSizeDecoder>::open(
                pairs[s].pipe.read_fd, FixedSizeDecoder{});
            while (auto item = co_await next(stream))
                pairs[s].received.push_back(item->id);
        };

        auto h0 = spawn(read_stream(0)).submit();
        auto h1 = spawn(read_stream(1)).submit();
        auto h2 = spawn(read_stream(2)).submit();
        co_await h0;
        co_await h1;
        co_await h2;
    }());

    for (int s = 0; s < STREAMS; s++) {
        ASSERT_EQ(pairs[s].received.size(), MSGS_PER_STREAM) << "stream " << s;
        for (uint32_t m = 0; m < MSGS_PER_STREAM; m++) {
            EXPECT_EQ(pairs[s].received[m], static_cast<uint32_t>(s * 100 + m))
                << "stream " << s << " msg " << m;
        }
    }
}
