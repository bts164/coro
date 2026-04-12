#include <gtest/gtest.h>
#include <coro/io/circular_byte_buffer.h>
#include <array>
#include <cstddef>

using namespace coro;

// ---------------------------------------------------------------------------
// Basic operations
// ---------------------------------------------------------------------------

TEST(CircularByteBufferTest, ConstructEmpty) {
    CircularByteBuffer buffer(1024);

    EXPECT_EQ(buffer.capacity(), 1024);
    EXPECT_EQ(buffer.readable_bytes(), 0);
    EXPECT_EQ(buffer.writable_bytes(), 1024);
    EXPECT_TRUE(buffer.readable_span().empty());
}

TEST(CircularByteBufferTest, WriteAndRead) {
    // TODO: Phase 3 - Implement test
    // - Get writable_span()
    // - Write some bytes
    // - commit_write(n)
    // - Verify readable_bytes() == n
    // - Read via readable_span()
    // - consume(n)
    // - Verify empty again
}

TEST(CircularByteBufferTest, Wraparound) {
    // TODO: Phase 3 - Implement test
    // - Write to near end of buffer
    // - Consume some bytes
    // - Write more (should wrap around)
    // - Verify data is correct
}

TEST(CircularByteBufferTest, FillCompletely) {
    // TODO: Phase 3 - Implement test
    // - Write until writable_bytes() == 0
    // - Verify readable_bytes() == capacity()
    // - Verify writable_span().empty()
}

TEST(CircularByteBufferTest, Clear) {
    // TODO: Phase 3 - Implement test
    // - Write some data
    // - clear()
    // - Verify empty state
}

TEST(CircularByteBufferTest, MultipleWriteReadCycles) {
    // TODO: Phase 3 - Implement test
    // - Simulate continuous streaming: write, read partial, write more, etc.
    // - Verify no data corruption
}
