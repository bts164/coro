#include <gtest/gtest.h>
#include <coro/co_invoke.h>
#include <coro/coro.h>
#include <coro/coro_stream.h>
#include <coro/stream.h>
#include <coro/runtime/runtime.h>
#include <coro/task/spawn_builder.h>
#include <memory>
#include <vector>

using namespace coro;

// A future that requires two polls to complete, forcing a suspension.
// On the first poll it self-wakes (so the executor re-queues immediately) and
// returns Pending. This ensures the calling coroutine suspends and resumes,
// which is exactly the point at which a raw rvalue lambda would be dangling.
struct TwoPollFuture {
    using OutputType = void;
    int* poll_count;
    PollResult<void> poll(detail::Context& ctx) {
        ++(*poll_count);
        if (*poll_count >= 2) return PollReady;
        ctx.getWaker()->wake();
        return PollPending;
    }
};

// --- CoInvokeFuture: value capture ---

TEST(CoInvokeTest, ValueCaptureAccessedAfterSuspension) {
    // The captured value must be accessible after the coroutine suspends and resumes.
    // Without co_invoke, the lambda temporary would be destroyed before resumption.
    Runtime rt(1);
    int result = 0;
    int polls  = 0;

    rt.block_on(co_invoke([&result, &polls]() -> Coro<void> {
        co_await TwoPollFuture{&polls};  // suspends once — lambda would be dead without co_invoke
        result = 42;
    }));

    EXPECT_EQ(result, 42);
    EXPECT_EQ(polls, 2);
}

TEST(CoInvokeTest, ValueCaptureByValue) {
    // Captures an integer by value; verifies the copy is owned by the wrapper.
    Runtime rt(1);
    int captured = 99;
    int result   = 0;
    int polls    = 0;

    rt.block_on(co_invoke([captured, &result, &polls]() -> Coro<void> {
        co_await TwoPollFuture{&polls};
        result = captured;   // 'captured' is a member of the lambda stored on the heap
    }));

    EXPECT_EQ(result, 99);
}

TEST(CoInvokeTest, ReturnsValue) {
    Runtime rt(1);
    int polls = 0;

    int result = rt.block_on(co_invoke([&polls]() -> Coro<int> {
        co_await TwoPollFuture{&polls};
        co_return 7;
    }));

    EXPECT_EQ(result, 7);
}

TEST(CoInvokeTest, IsMoveConstructible) {
    // Future concept requires move constructibility.
    // Moving CoInvokeFuture must not invalidate the coroutine's this pointer.
    Runtime rt(1);
    int result = 0;
    int polls  = 0;

    auto wrapper = co_invoke([&result, &polls]() -> Coro<void> {
        co_await TwoPollFuture{&polls};
        result = 55;
    });

    // Move the wrapper before submitting it.
    auto moved = std::move(wrapper);
    rt.block_on(std::move(moved));

    EXPECT_EQ(result, 55);
}

// --- Composition with spawn ---
static_assert(Future<JoinHandle<void>>);
TEST(CoInvokeTest, ComposesWithSpawn) {
    Runtime rt(1);
    int result = 0;
    int polls  = 0;

    rt.block_on([&]() -> Coro<void> {
        auto handle = spawn(co_invoke([&result, &polls]() -> Coro<void> {
            co_await TwoPollFuture{&polls};
            result = 77;
        }));
        co_await handle;
    }());

    EXPECT_EQ(result, 77);
}

// --- CoInvokeStream: stream-returning lambda ---

TEST(CoInvokeStreamTest, YieldsItemsAfterSuspension) {
    // Verifies that a CoroStream lambda accessed after suspension works correctly.
    Runtime rt(1);
    int polls = 0;
    std::vector<int> items;

    rt.block_on([&]() -> Coro<void> {
        auto s = co_invoke([&polls]() -> CoroStream<int> {
            co_yield 1;
            co_yield 2;
            co_yield 3;
        });
        while (auto item = co_await next(s))
            items.push_back(*item);
    }());

    EXPECT_EQ(items, (std::vector<int>{1, 2, 3}));
}

TEST(CoInvokeStreamTest, ValueCaptureInStream) {
    Runtime rt(1);
    int base = 10;
    std::vector<int> items;

    rt.block_on([&]() -> Coro<void> {
        auto s = co_invoke([base]() -> CoroStream<int> {
            co_yield base + 1;
            co_yield base + 2;
            co_yield base + 3;
        });
        while (auto item = co_await next(s))
            items.push_back(*item);
    }());

    EXPECT_EQ(items, (std::vector<int>{11, 12, 13}));
}

// --- Capture lifetime: ensures captures are released on completion, not on wrapper destruction ---
//
// The wrapper (CoInvokeFuture / CoInvokeStream) may outlive the coroutine if it is
// stored as a local variable. Without the fix, captured values (e.g. RAII guards,
// shared_ptr) would be held until the wrapper itself is destroyed, violating the
// coroutine scope invariant. These tests keep the wrapper alive in scope AFTER the
// coroutine/stream completes and verify the captures are already released.

TEST(CoInvokeTest, ReleasesCaptures_OnCompletion) {
    Runtime rt(1);
    auto resource = std::make_shared<int>(0);
    std::weak_ptr<int> weak = resource;
    bool still_held_after_await = true;

    rt.block_on([&]() -> Coro<void> {
        // Move resource into the lambda — it becomes the sole owner (refcount = 1).
        auto child = co_invoke([r = std::move(resource)]() -> Coro<void> {
            co_return;
        });
        co_await child;
        // 'child' (the CoInvokeFuture) is still in scope here. Without the fix,
        // m_lambda has not been reset, so 'r' is still alive (weak not expired).
        still_held_after_await = !weak.expired();
    }());

    EXPECT_FALSE(still_held_after_await); // captures must be released on completion
}

TEST(CoInvokeStreamTest, ReleasesCaptures_WhenExhausted) {
    Runtime rt(1);
    auto resource = std::make_shared<int>(0);
    std::weak_ptr<int> weak = resource;
    bool still_held_after_drain = true;

    rt.block_on([&]() -> Coro<void> {
        auto s = co_invoke([r = std::move(resource)]() -> CoroStream<int> {
            co_yield 1;
        });
        while (auto item = co_await next(s)) {}
        // 's' (the CoInvokeStream) is still in scope. Without the fix, m_lambda
        // has not been reset, so 'r' is still alive (weak not expired).
        still_held_after_drain = !weak.expired();
    }());

    EXPECT_FALSE(still_held_after_drain); // captures must be released on exhaustion
}
