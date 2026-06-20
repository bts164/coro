// FramePool is platform-agnostic (not gated by CORO_PICO) — exercised here on
// desktop. Its use as a coroutine promise-type allocator is Pico-only; see
// CoroPromiseBase's operator new/delete in coro.h.

#include <coro/detail/frame_pool.h>
#include <gtest/gtest.h>
#include <cstring>
#include <memory>
#include <unordered_set>

using namespace coro::detail;

TEST(FramePool, AllocateReturnsDistinctSlots) {
    FramePool<64, 4> pool;
    std::unordered_set<void*> seen;
    for (int i = 0; i < 4; ++i) {
        void* p = pool.allocate();
        ASSERT_NE(p, nullptr);
        EXPECT_TRUE(seen.insert(p).second);
    }
}

TEST(FramePool, ExhaustedPoolReturnsNullptr) {
    FramePool<64, 2> pool;
    EXPECT_NE(pool.allocate(), nullptr);
    EXPECT_NE(pool.allocate(), nullptr);
    EXPECT_EQ(pool.allocate(), nullptr);
}

TEST(FramePool, DeallocateMakesSlotReusable) {
    FramePool<64, 1> pool;
    void* p1 = pool.allocate();
    ASSERT_NE(p1, nullptr);
    EXPECT_EQ(pool.allocate(), nullptr); // exhausted
    pool.deallocate(p1);
    void* p2 = pool.allocate();
    EXPECT_EQ(p1, p2); // same slot reused
}

TEST(FramePool, OwnsDistinguishesPoolMemoryFromOutsidePointers) {
    FramePool<64, 2> pool;
    void* p = pool.allocate();
    ASSERT_NE(p, nullptr);
    EXPECT_TRUE(pool.owns(p));

    int outside;
    EXPECT_FALSE(pool.owns(&outside));

    auto heap = std::make_unique<char[]>(64);
    EXPECT_FALSE(pool.owns(heap.get()));
}

TEST(FramePool, AllocatedMemoryIsWritableForFullSlotSize) {
    FramePool<64, 1> pool;
    void* p = pool.allocate();
    ASSERT_NE(p, nullptr);
    std::memset(p, 0xAB, 64); // must not corrupt adjacent/internal state
    pool.deallocate(p);
    // Re-allocating after a full-slot write should still produce a valid,
    // distinct-from-corrupted freelist link slot.
    void* p2 = pool.allocate();
    EXPECT_EQ(p, p2);
}

#if defined(CORO_PICO) && defined(CORO_PICO_FRAME_POOL)
// Only active when the experimental CORO_PICO_FRAME_POOL opt-in is enabled,
// where CoroPromiseBase::operator new/delete (coro.h) route through FramePool
// instead of the general allocator. Confirms the hook compiles and
// round-trips correctly through real Coro<T> frames — not a performance
// check (that's the SysTick harness in the epaper example).
#include <coro/coro.h>
#include <coro/detail/context.h>
#include <coro/detail/waker.h>

namespace {

struct NoopWaker : coro::detail::Waker {
    void wake() override {}
    coro::detail::Rc<coro::detail::Waker> clone() override {
        return coro::detail::make_rc<NoopWaker>();
    }
};

coro::Coro<int> trivial_coro(int v) { co_return v; }

} // namespace

TEST(FramePool, CoroFrameRoundTripsThroughPool) {
    coro::detail::Context ctx(coro::detail::make_rc<NoopWaker>());
    for (int i = 0; i < 10; ++i) {
        auto c = trivial_coro(i);
        EXPECT_FALSE(c.poll(ctx).isPending());
        // c destructs here, returning its frame to the pool for reuse next iteration.
    }
}
#endif
