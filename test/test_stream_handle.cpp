#include <gtest/gtest.h>
#include <coro/coro.h>
#include <coro/coro_stream.h>
#include <coro/runtime/runtime.h>
#include <coro/task/spawn_builder.h>
#include <coro/sync/stream_handle.h>
#include <stdexcept>
#include <vector>

using namespace coro;

// --- Concept check (compile-time) ---

static_assert(Stream<StreamHandle<int>>);

// Helper: drain a StreamHandle into a vector via block_on + a Coro.
template<typename T>
Coro<std::vector<T>> drain(StreamHandle<T> handle) {
    std::vector<T> items;
    while (auto item = co_await next(handle)) {
        items.push_back(std::move(*item));
    }
    co_return items;
}

// --- Basic functionality ---

TEST(StreamHandleTest, EmptyStreamYieldsNothing) {
    Runtime rt(1);

    auto make_empty = []() -> CoroStream<int> {
        co_return;
    };

    auto handle = rt.spawn(make_empty()).submit();
    auto items  = rt.block_on(drain(std::move(handle)));
    EXPECT_TRUE(items.empty());
}

TEST(StreamHandleTest, SingleItemStream) {
    Runtime rt(1);

    auto make_stream = []() -> CoroStream<int> {
        co_yield 42;
    };

    auto handle = rt.spawn(make_stream()).submit();
    auto items  = rt.block_on(drain(std::move(handle)));
    ASSERT_EQ(items.size(), 1u);
    EXPECT_EQ(items[0], 42);
}

TEST(StreamHandleTest, MultipleItems) {
    Runtime rt(1);

    auto make_stream = []() -> CoroStream<int> {
        co_yield 1;
        co_yield 2;
        co_yield 3;
    };

    auto handle = rt.spawn(make_stream()).submit();
    auto items  = rt.block_on(drain(std::move(handle)));
    ASSERT_EQ(items.size(), 3u);
    EXPECT_EQ(items[0], 1);
    EXPECT_EQ(items[1], 2);
    EXPECT_EQ(items[2], 3);
}

// --- Buffer size ---

TEST(StreamHandleTest, SmallBufferDrainsCorrectly) {
    Runtime rt(1);

    auto make_stream = []() -> CoroStream<int> {
        for (int i = 0; i < 10; ++i)
            co_yield i;
    };

    // Buffer smaller than number of items forces backpressure.
    auto handle = rt.spawn(make_stream()).buffer(2).submit();
    auto items  = rt.block_on(drain(std::move(handle)));
    ASSERT_EQ(items.size(), 10u);
    for (int i = 0; i < 10; ++i)
        EXPECT_EQ(items[i], i);
}

// --- Exception propagation ---

TEST(StreamHandleTest, ExceptionPropagatesFromStream) {
    Runtime rt(1);

    auto make_stream = []() -> CoroStream<int> {
        co_yield 1;
        throw std::runtime_error("stream error");
        co_yield 2;
    };

    auto handle = rt.spawn(make_stream()).submit();

    EXPECT_THROW(rt.block_on(drain(std::move(handle))), std::runtime_error);
}

// --- Default-constructed handle ---

TEST(StreamHandleTest, DefaultConstructedHandleIsExhausted) {
    Runtime rt(1);

    StreamHandle<int> handle;
    auto items = rt.block_on(drain(std::move(handle)));
    EXPECT_TRUE(items.empty());
}
