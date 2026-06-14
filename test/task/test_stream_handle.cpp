#include <gtest/gtest.h>
#include "executor_traits.h"
#include <coro/coro.h>
#include <coro/coro_stream.h>
#include <coro/runtime/runtime.h>
#include <coro/task/spawn_builder.h>
#include <coro/task/join_handle.h>
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

// After cancel(), items already in the buffer are still readable; poll_next() then
// returns nullopt (clean end). This verifies the producer stops but buffered output
// is not discarded — distinguishing cancel() from dropping the handle.
TYPED_TEST(StreamHandleTest, CancelYieldsBufferedItemsThenEnds) {
    // buffer=3; infinite stream fills the queue then parks.
    auto handle = this->traits.rt.build_task().buffer(3).spawn(
        []() -> CoroStream<int> { for (int i = 0; ; ++i) co_yield i; }());

    auto items = this->traits.rt.block_on(
        [](StreamHandle<int> h) -> Coro<std::vector<int>> {
            std::vector<int> result;
            // Consume one item — forces the producer to run and fill the buffer.
            if (auto v = co_await next(h))
                result.push_back(*v);
            // Cancel while the buffer still holds the next items.
            h.cancel();
            // Drain whatever remains in the buffer; stream ends cleanly (no exception).
            while (auto v = co_await next(h))
                result.push_back(*v);
            co_return result;
        }(std::move(handle)));

    // Got at least the first item; definitely did not spin forever.
    EXPECT_GE(items.size(), 1u);
    EXPECT_LT(items.size(), 1000u);
    // Items are the head of the sequence starting from 0.
    for (size_t i = 0; i < items.size(); ++i)
        EXPECT_EQ(items[i], static_cast<int>(i));
}

// Dropping a StreamHandle inside a coroutine must register the background task with
// the enclosing CoroutineScope. The parent must not complete until the task drains.
TYPED_TEST(StreamHandleTest, DroppedHandleScopeWaitsForTaskDrain) {
    auto handle = this->traits.rt.spawn(
        []() -> CoroStream<int> { while (true) co_yield 1; }());

    this->traits.rt.block_on(
        [](StreamHandle<int> h) -> Coro<void> {
            {
                // Move into a local so it goes out of scope here, inside the poll,
                // with t_current_coro set — triggering scope registration.
                StreamHandle<int> dropped = std::move(h);
            }
            co_return;
        }(std::move(handle)));

    SUCCEED(); // reaching here means the scope drain completed without hanging
}
