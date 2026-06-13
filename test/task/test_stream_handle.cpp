#include <gtest/gtest.h>
#include "executor_traits.h"
#include <coro/coro.h>
#include <coro/coro_stream.h>
#include <coro/runtime/runtime.h>
#include <coro/task/spawn_builder.h>
#include <coro/task/stream_handle.h>
#include <stdexcept>
#include <vector>

using namespace coro;

static_assert(Stream<StreamHandle<int>>);

template<typename T>
Coro<std::vector<T>> drain(StreamHandle<T> handle) {
    std::vector<T> items;
    while (auto item = co_await next(handle))
        items.push_back(std::move(*item));
    co_return items;
}

template<typename Traits>
class StreamHandleTest : public testing::Test {
protected:
    Traits traits;
};
TYPED_TEST_SUITE(StreamHandleTest, AllExecutors);

TYPED_TEST(StreamHandleTest, EmptyStreamYieldsNothing) {
    auto handle = this->traits.rt.spawn([]() -> CoroStream<int> { co_return; }());
    auto items  = this->traits.rt.block_on(drain(std::move(handle)));
    EXPECT_TRUE(items.empty());
}

TYPED_TEST(StreamHandleTest, SingleItemStream) {
    auto handle = this->traits.rt.spawn([]() -> CoroStream<int> { co_yield 42; }());
    auto items  = this->traits.rt.block_on(drain(std::move(handle)));
    ASSERT_EQ(items.size(), 1u);
    EXPECT_EQ(items[0], 42);
}

TYPED_TEST(StreamHandleTest, MultipleItems) {
    auto handle = this->traits.rt.spawn([]() -> CoroStream<int> {
        co_yield 1; co_yield 2; co_yield 3;
    }());
    auto items = this->traits.rt.block_on(drain(std::move(handle)));
    ASSERT_EQ(items.size(), 3u);
    EXPECT_EQ(items[0], 1);
    EXPECT_EQ(items[1], 2);
    EXPECT_EQ(items[2], 3);
}

TYPED_TEST(StreamHandleTest, SmallBufferDrainsCorrectly) {
    auto handle = this->traits.rt.build_task().buffer(2).spawn([]() -> CoroStream<int> {
        for (int i = 0; i < 10; ++i) co_yield i;
    }());
    auto items = this->traits.rt.block_on(drain(std::move(handle)));
    ASSERT_EQ(items.size(), 10u);
    for (int i = 0; i < 10; ++i)
        EXPECT_EQ(items[i], i);
}

TYPED_TEST(StreamHandleTest, ExceptionPropagatesFromStream) {
    auto handle = this->traits.rt.spawn([]() -> CoroStream<int> {
        co_yield 1;
        throw std::runtime_error("stream error");
        co_yield 2;
    }());
    EXPECT_THROW(this->traits.rt.block_on(drain(std::move(handle))), std::runtime_error);
}

TYPED_TEST(StreamHandleTest, DefaultConstructedHandleIsExhausted) {
    StreamHandle<int> handle;
    auto items = this->traits.rt.block_on(drain(std::move(handle)));
    EXPECT_TRUE(items.empty());
}
