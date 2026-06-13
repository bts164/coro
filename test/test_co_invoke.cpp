#include <gtest/gtest.h>
#include "executor_traits.h"
#include <coro/co_invoke.h>
#include <coro/coro.h>
#include <coro/coro_stream.h>
#include <coro/stream.h>
#include <coro/runtime/runtime.h>
#include <coro/task/spawn_builder.h>
#include <memory>
#include <vector>

using namespace coro;

namespace {

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

}  // namespace

template<typename Traits>
class CoInvokeTest : public testing::Test {
protected:
    Traits traits;
};
TYPED_TEST_SUITE(CoInvokeTest, AllExecutors);

template<typename Traits>
class CoInvokeStreamTest : public testing::Test {
protected:
    Traits traits;
};
TYPED_TEST_SUITE(CoInvokeStreamTest, AllExecutors);

TYPED_TEST(CoInvokeTest, ValueCaptureAccessedAfterSuspension) {
    int result = 0, polls = 0;
    this->traits.rt.block_on(co_invoke([&result, &polls]() -> Coro<void> {
        co_await TwoPollFuture{&polls};
        result = 42;
    }));
    EXPECT_EQ(result, 42);
    EXPECT_EQ(polls, 2);
}

TYPED_TEST(CoInvokeTest, ValueCaptureByValue) {
    int captured = 99, result = 0, polls = 0;
    this->traits.rt.block_on(co_invoke([captured, &result, &polls]() -> Coro<void> {
        co_await TwoPollFuture{&polls};
        result = captured;
    }));
    EXPECT_EQ(result, 99);
}

TYPED_TEST(CoInvokeTest, ReturnsValue) {
    int polls = 0;
    int result = this->traits.rt.block_on(co_invoke([&polls]() -> Coro<int> {
        co_await TwoPollFuture{&polls};
        co_return 7;
    }));
    EXPECT_EQ(result, 7);
}

TYPED_TEST(CoInvokeTest, IsMoveConstructible) {
    int result = 0, polls = 0;
    auto wrapper = co_invoke([&result, &polls]() -> Coro<void> {
        co_await TwoPollFuture{&polls};
        result = 55;
    });
    auto moved = std::move(wrapper);
    this->traits.rt.block_on(std::move(moved));
    EXPECT_EQ(result, 55);
}

static_assert(Future<JoinHandle<void>>);

TYPED_TEST(CoInvokeTest, ComposesWithSpawn) {
    int result = 0, polls = 0;
    this->traits.rt.block_on([](int& result, int& polls) -> Coro<void> {
        auto handle = spawn(co_invoke([&result, &polls]() -> Coro<void> {
            co_await TwoPollFuture{&polls};
            result = 77;
        }));
        co_await handle;
    }(result, polls));
    EXPECT_EQ(result, 77);
}

TYPED_TEST(CoInvokeStreamTest, YieldsItemsAfterSuspension) {
    std::vector<int> items;
    this->traits.rt.block_on([](std::vector<int>& items) -> Coro<void> {
        auto s = co_invoke([]() -> CoroStream<int> {
            co_yield 1; co_yield 2; co_yield 3;
        });
        while (auto item = co_await next(s))
            items.push_back(*item);
    }(items));
    EXPECT_EQ(items, (std::vector<int>{1, 2, 3}));
}

TYPED_TEST(CoInvokeStreamTest, ValueCaptureInStream) {
    int base = 10;
    std::vector<int> items;
    this->traits.rt.block_on([](int base, std::vector<int>& items) -> Coro<void> {
        auto s = co_invoke([base]() -> CoroStream<int> {
            co_yield base + 1; co_yield base + 2; co_yield base + 3;
        });
        while (auto item = co_await next(s))
            items.push_back(*item);
    }(base, items));
    EXPECT_EQ(items, (std::vector<int>{11, 12, 13}));
}

TYPED_TEST(CoInvokeTest, ReleasesCaptures_OnCompletion) {
    auto resource = std::make_shared<int>(0);
    std::weak_ptr<int> weak = resource;
    bool still_held_after_await = true;
    this->traits.rt.block_on([](std::shared_ptr<int> resource, std::weak_ptr<int>& weak,
                                bool& still_held) -> Coro<void> {
        auto child = co_invoke([r = std::move(resource)]() -> Coro<void> {
            co_return;
        });
        co_await child;
        still_held = !weak.expired();
    }(std::move(resource), weak, still_held_after_await));
    EXPECT_FALSE(still_held_after_await);
}

TYPED_TEST(CoInvokeStreamTest, ReleasesCaptures_WhenExhausted) {
    auto resource = std::make_shared<int>(0);
    std::weak_ptr<int> weak = resource;
    bool still_held_after_drain = true;
    this->traits.rt.block_on([](std::shared_ptr<int> resource, std::weak_ptr<int>& weak,
                                bool& still_held) -> Coro<void> {
        auto s = co_invoke([r = std::move(resource)]() -> CoroStream<int> {
            co_yield 1;
        });
        while (auto item = co_await next(s)) {}
        still_held = !weak.expired();
    }(std::move(resource), weak, still_held_after_drain));
    EXPECT_FALSE(still_held_after_drain);
}
