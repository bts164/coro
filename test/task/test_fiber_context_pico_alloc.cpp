// Validates alloc_fiber_stack()/free_fiber_stack() for the CORO_PICO backend
// (src/detail/fiber_context_pico.cpp) -- see doc/design/fiber.md's "Stack
// allocation and sizing" and "Stack overflow detection". This logic is
// ordinary, target-independent C++ (no ARM instructions), so it's compiled
// and run directly on the host with CORO_PICO defined for this translation
// unit only -- unlike switch_context()/init_fiber_entry() (see
// test_switch_context_pico.cpp), it needs no ARM emulation.
//
// Reads the canary/fill pattern through fc.buffer directly rather than by
// reconstructing a pointer from fc.stack_low -- that field is a uint32_t
// (correct on the real ARMv6-M target, where pointers are 32 bits) truncated
// from a 64-bit host pointer here, so it can't be turned back into a valid
// host pointer. The `stack_high - stack_low == total size` check below is
// still valid post-truncation: both are derived from the same `buffer`
// pointer, so their unsigned difference equals the real byte distance modulo
// 2^32, which is exact here since the buffer is far smaller than 4GB.

#include <coro/detail/fiber_context.h>

#include <gtest/gtest.h>

#include <cstdint>

namespace coro::detail {
namespace {

constexpr uint32_t kStackCanary  = 0xc0ffeeee;
constexpr uint32_t kFillPattern  = 0xa5a5a5a5;
constexpr size_t   kStackSize    = 256;

uint32_t word_at(const FiberContext& fc, size_t byte_offset) {
    return *reinterpret_cast<const uint32_t*>(fc.buffer + byte_offset);
}

} // namespace

TEST(FiberContextPicoAlloc, DefaultConstructedHasNoStack) {
    FiberContext fc;
    EXPECT_EQ(fc.buffer, nullptr);
    EXPECT_EQ(fc.stack_low, 0u);
    EXPECT_EQ(fc.stack_high, 0u);
}

TEST(FiberContextPicoAlloc, AllocatesBufferCoveringAtLeastRequestedSize) {
    FiberContext fc;
    alloc_fiber_stack(fc, kStackSize);

    ASSERT_NE(fc.buffer, nullptr);
    // stack_high - stack_low is the whole allocated buffer (cushion included),
    // which must be at least the requested working size -- see the file
    // comment above for why this subtraction survives the uint32_t truncation.
    EXPECT_GE(fc.stack_high - fc.stack_low, kStackSize);

    free_fiber_stack(fc);
}

TEST(FiberContextPicoAlloc, WritesCanaryAtBottomOfBuffer) {
    FiberContext fc;
    alloc_fiber_stack(fc, kStackSize);

    EXPECT_EQ(word_at(fc, 0), kStackCanary);

    free_fiber_stack(fc);
}

TEST(FiberContextPicoAlloc, PaintsRestOfBufferWithFillPattern) {
    FiberContext fc;
    alloc_fiber_stack(fc, kStackSize);

    // Word 0 is the canary; spot-check a few words above it for the fill
    // pattern used for offline high-water-mark profiling.
    EXPECT_EQ(word_at(fc, 4), kFillPattern);
    EXPECT_EQ(word_at(fc, 8), kFillPattern);
    EXPECT_EQ(word_at(fc, kStackSize), kFillPattern);

    free_fiber_stack(fc);
}

TEST(FiberContextPicoAlloc, FreeClearsBufferAndBounds) {
    FiberContext fc;
    alloc_fiber_stack(fc, kStackSize);

    free_fiber_stack(fc);

    EXPECT_EQ(fc.buffer, nullptr);
    EXPECT_EQ(fc.stack_low, 0u);
    EXPECT_EQ(fc.stack_high, 0u);
}

TEST(FiberContextPicoAlloc, FreeIsSafeOnStackNeverAllocated) {
    FiberContext fc;
    free_fiber_stack(fc); // must not crash
    SUCCEED();
}

} // namespace coro::detail
