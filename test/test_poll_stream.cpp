#include <gtest/gtest.h>
#include <coro/io/poll_stream.h>
#include <coro/io/decoder_concept.h>
#include <coro/runtime/runtime.h>
#include <coro/coro.h>
#include <unistd.h>
#include <fcntl.h>
#include <array>
#include <optional>

using namespace coro;

// ---------------------------------------------------------------------------
// Mock Fixed-Size Decoder for Testing
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
// Helper: Create pipe pair
// ---------------------------------------------------------------------------

struct PipePair {
    int read_fd = -1;
    int write_fd = -1;

    PipePair() {
        int fds[2];
        if (pipe(fds) != 0) {
            throw std::runtime_error("pipe() failed");
        }
        read_fd = fds[0];
        write_fd = fds[1];

        // Set read end to non-blocking
        int flags = fcntl(read_fd, F_GETFL);
        fcntl(read_fd, F_SETFL, flags | O_NONBLOCK);
    }

    ~PipePair() {
        if (read_fd >= 0) ::close(read_fd);
        if (write_fd >= 0) ::close(write_fd);
    }

    void write_message(const MockMessage& msg) {
        ::write(write_fd, &msg, sizeof(msg));
    }

    void close_write_end() {
        if (write_fd >= 0) {
            ::close(write_fd);
            write_fd = -1;
        }
    }
};

// ---------------------------------------------------------------------------
// Concept checks
// ---------------------------------------------------------------------------

static_assert(Stream<PollStream<MockMessage, FixedSizeDecoder>>);

// ---------------------------------------------------------------------------
// Basic stream operations
// ---------------------------------------------------------------------------

TEST(PollStreamTest, OpenAndClose) {
    // TODO: Phase 3 - Implement test
    // - Create pipe
    // - Open PollStream
    // - Close stream
    // - Verify no leaks
}

TEST(PollStreamTest, ReadSingleMessage) {
    // TODO: Phase 3 - Implement test
    // - Create pipe
    // - Write one MockMessage to pipe
    // - Open PollStream
    // - co_await stream.next()
    // - Verify message received correctly
}

TEST(PollStreamTest, ReadMultipleMessages) {
    // TODO: Phase 3 - Implement test
    // - Write 5 messages
    // - Read all 5 via stream.next() in loop
    // - Verify all correct
}

TEST(PollStreamTest, ReadEOF) {
    // TODO: Phase 3 - Implement test
    // - Write messages
    // - Close write end
    // - Read until stream.next() returns nullopt
    // - Verify EOF handled correctly
}

// ---------------------------------------------------------------------------
// Backpressure — Block mode (default)
// ---------------------------------------------------------------------------

TEST(PollStreamTest, Backpressure_PacketBufferFull) {
    // TODO: Phase 3 - Implement test
    // - Create stream with small packet buffer (e.g., 4 packets)
    // - Write many messages rapidly
    // - Verify polling stops when buffer fills
    // - Consume some packets
    // - Verify polling resumes
}

// ---------------------------------------------------------------------------
// Backpressure — Overrun mode
// ---------------------------------------------------------------------------

TEST(PollStreamTest, Overrun_DropsOldestAndCountsMissed) {
    // TODO: Phase 3 - Implement test
    // - Create stream with packet buffer capacity 4, BackpressureMode::Overrun
    // - Write 8 messages before consumer runs
    // - First poll_next call should return PollStreamOverrunError{4} (4 oldest dropped)
    // - Subsequent poll_next calls should return the 4 surviving (newest) messages
    // - Verify stream remains open and readable after the overrun error
}

TEST(PollStreamTest, Overrun_NoErrorWhenNoMisses) {
    // TODO: Phase 3 - Implement test
    // - Create stream with BackpressureMode::Overrun
    // - Write messages slowly enough that the consumer keeps up
    // - Verify no PollStreamOverrunError is delivered
    // - Verify all messages arrive correctly
}

TEST(PollStreamTest, Overrun_ContinuesAfterError) {
    // TODO: Phase 3 - Implement test
    // - Trigger one overrun
    // - Consumer catches PollStreamOverrunError and continues
    // - Write more messages
    // - Verify subsequent messages arrive correctly (stream not faulted)
}

// ---------------------------------------------------------------------------
// Error handling
// ---------------------------------------------------------------------------

TEST(PollStreamTest, DecoderThrows_StreamFaults) {
    // TODO: Phase 3 - Implement test with custom decoder that throws
    // - Write data that causes decoder to throw
    // - Verify stream.next() returns PollError
    // - Verify stream is faulted (subsequent calls also error)
}

TEST(PollStreamTest, ReadError_IOFailure) {
    // TODO: Phase 3 - Implement test
    // - Close read fd externally while stream active
    // - Verify stream detects error and faults
}

// ---------------------------------------------------------------------------
// Partial packet handling
// ---------------------------------------------------------------------------

TEST(PollStreamTest, MessageSpansMultipleReads) {
    // TODO: Phase 3 - Implement test
    // - Write first half of message
    // - Verify stream.next() returns Pending
    // - Write second half
    // - Verify stream.next() completes with full message
}

// ---------------------------------------------------------------------------
// Multiple independent streams
// ---------------------------------------------------------------------------

TEST(PollStreamTest, MultipleStreamsIndependent) {
    // TODO: Phase 3 - Implement test
    // - Create 3 pipe pairs
    // - Open 3 PollStreams
    // - Write to each at different rates
    // - Verify each stream receives correct messages independently
}
