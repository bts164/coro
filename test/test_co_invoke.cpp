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
    Runtime rt;
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
    Runtime rt;
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
    Runtime rt;
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
    Runtime rt;
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
    Runtime rt;
    int result = 0;
    int polls  = 0;

    rt.block_on([&]() -> Coro<void> {
        auto handle = spawn(co_invoke([&result, &polls]() -> Coro<void> {
            co_await TwoPollFuture{&polls};
            result = 77;
        })).submit();
        co_await handle;
    }());

    EXPECT_EQ(result, 77);
}

// --- CoInvokeStream: stream-returning lambda ---

TEST(CoInvokeStreamTest, YieldsItemsAfterSuspension) {
    // Verifies that a CoroStream lambda accessed after suspension works correctly.
    Runtime rt;
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
    Runtime rt;
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
