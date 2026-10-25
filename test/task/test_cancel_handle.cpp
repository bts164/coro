#include <gtest/gtest.h>
#include "executor_traits.h"
#include "co_assert.h"
#include <coro/coro.h>
#include <coro/coro_stream.h>
#include <coro/future.h>
#include <coro/runtime/runtime.h>
#include <coro/stream.h>
#include <coro/task/cancel_handle.h>
#include <coro/task/join_handle.h>
#include <coro/task/join_set.h>
#include <coro/task/yield_now.h>
#include <atomic>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

#ifndef CORO_PICO
#include <coro/sync/sleep.h>
#include <coro/task/spawn_blocking.h>
#include <chrono>
#include <memory>
#include <thread>
#endif

using namespace coro;

namespace {

static_assert(std::is_copy_constructible_v<CancelHandle>);
static_assert(std::is_copy_assignable_v<CancelHandle>);
static_assert(std::is_nothrow_move_constructible_v<CancelHandle>);
static_assert(!Future<CancelHandle>);

struct ReadyFuture {
    using OutputType = int;
    int value;
    PollResult<int> poll(detail::Context&) { return value; }
};

// Counts the destruction of the coroutine frame it is passed into.
class DropToken {
public:
    explicit DropToken(std::atomic<int>& dropped) : m_dropped(&dropped) {}
    DropToken(DropToken&& other) noexcept : m_dropped(std::exchange(other.m_dropped, nullptr)) {}
    ~DropToken() { if (m_dropped) m_dropped->fetch_add(1); }
private:
    std::atomic<int>* m_dropped;
};

Coro<int> wait_forever([[maybe_unused]] DropToken token) {
    co_await never<void>();
    co_return 0;
}

CoroStream<int> stream_forever([[maybe_unused]] DropToken token) {
    co_await never<void>();
    co_yield 0;
}

// Yields until `handle` sees its task finished. A task cancelled on another worker
// finishes at its next poll, so this always ends.
Coro<void> until_finished(CancelHandle handle) {
    while (!handle.is_finished()) co_await yield_now();
}

}  // namespace

// ---------------------------------------------------------------------------
// States, copy and move
// ---------------------------------------------------------------------------

TEST(CancelHandleTest, DefaultIsEmpty) {
    CancelHandle handle;
    EXPECT_FALSE(handle.valid());
    EXPECT_FALSE(static_cast<bool>(handle));
    EXPECT_THROW((void)handle.is_finished(), std::logic_error);
    handle.cancel();  // does nothing
    EXPECT_FALSE(handle.valid());
}

TEST(CancelHandleTest, KnownFinishedTaskIsFinished) {
    CancelHandle handle(detail::Weak<detail::TaskBase>{}, true);
    EXPECT_TRUE(handle.valid());
    EXPECT_TRUE(handle.is_finished());
    handle.cancel();
    EXPECT_TRUE(handle.is_finished());
}

TEST(CancelHandleTest, ExpiredTaskCountsAsFinished) {
    CancelHandle handle{detail::Weak<detail::TaskBase>{}};
    EXPECT_TRUE(handle.valid());
    EXPECT_TRUE(handle.is_finished());
    EXPECT_TRUE(handle.valid());
}

TEST(CancelHandleTest, MovedFromIsEmptyAndCopyKeepsState) {
    CancelHandle finished(detail::Weak<detail::TaskBase>{}, true);
    CancelHandle copy = finished;
    EXPECT_TRUE(copy.is_finished());
    EXPECT_TRUE(finished.is_finished());

    CancelHandle moved = std::move(finished);
    EXPECT_TRUE(moved.is_finished());
    EXPECT_FALSE(finished.valid());  // NOLINT(bugprone-use-after-move)

    CancelHandle assigned;
    assigned = std::move(moved);
    EXPECT_TRUE(assigned.is_finished());
    EXPECT_FALSE(moved.valid());  // NOLINT(bugprone-use-after-move)

    assigned = CancelHandle{};
    EXPECT_FALSE(assigned.valid());
}

// ---------------------------------------------------------------------------
// From JoinSet::spawn()
// ---------------------------------------------------------------------------

template<typename Traits>
class CancelHandleRuntimeTest : public testing::Test {
protected:
    Traits traits;
};
TYPED_TEST_SUITE(CancelHandleRuntimeTest, AllExecutors);

TYPED_TEST(CancelHandleRuntimeTest, JoinSetSpawnHandleCancelsTask) {
    bool tracking = false, finished = false, cancelled = false;
    std::atomic<int> dropped = 0;
    this->traits.rt.block_on([](bool& tracking, bool& finished, bool& cancelled,
                                std::atomic<int>& dropped) -> Coro<void> {
        JoinSet<int> js;
        CancelHandle handle = js.spawn(wait_forever(DropToken{dropped}));
        tracking = handle.valid() && !handle.is_finished();
        handle.cancel();
        auto item = co_await next(js);
        CO_ASSERT(item.has_value());
        cancelled = item->is_cancelled();
        finished = handle.is_finished();
    }(tracking, finished, cancelled, dropped));
    EXPECT_TRUE(tracking);
    EXPECT_TRUE(cancelled);
    EXPECT_TRUE(finished);
    EXPECT_EQ(dropped, 1);
}

TYPED_TEST(CancelHandleRuntimeTest, CancelsOnlyItsOwnTask) {
    int cancelled = 0, values = 0;
    std::atomic<int> dropped = 0;
    this->traits.rt.block_on([](int& cancelled, int& values, std::atomic<int>& dropped) -> Coro<void> {
        JoinSet<int, int> js;
        CancelHandle first = js.spawn(1, wait_forever(DropToken{dropped}));
        js.spawn(2, ReadyFuture{2});
        CancelHandle copy = first;
        copy.cancel();
        while (auto item = co_await next(js)) {
            if (item->is_cancelled() && item->tag() == 1) ++cancelled;
            if (item->has_value() && item->tag() == 2) ++values;
        }
    }(cancelled, values, dropped));
    EXPECT_EQ(cancelled, 1);
    EXPECT_EQ(values, 1);
}

TYPED_TEST(CancelHandleRuntimeTest, BuilderSpawnReturnsHandle) {
    bool cancelled = false;
    std::atomic<int> dropped = 0;
    this->traits.rt.block_on([](bool& cancelled, std::atomic<int>& dropped) -> Coro<void> {
        JoinSet<int> js;
        CancelHandle handle = js.build_task(wait_forever(DropToken{dropped})).name("w").spawn();
        handle.cancel();
        for (auto& r : co_await js.join_all()) cancelled = r.is_cancelled();
    }(cancelled, dropped));
    EXPECT_TRUE(cancelled);
}

TYPED_TEST(CancelHandleRuntimeTest, CancelAfterFinishChangesNothing) {
    int value = 0;
    bool finished = false;
    this->traits.rt.block_on([](int& value, bool& finished) -> Coro<void> {
        JoinSet<int> js;
        CancelHandle handle = js.spawn(ReadyFuture{7});
        auto item = co_await next(js);
        CO_ASSERT(item.has_value());
        handle.cancel();
        handle.cancel();
        value = item->value();
        finished = handle.is_finished();
    }(value, finished));
    EXPECT_EQ(value, 7);
    EXPECT_TRUE(finished);
}

TYPED_TEST(CancelHandleRuntimeTest, OutlivesItsSet) {
    bool finished = false;
    std::atomic<int> dropped = 0;
    this->traits.rt.block_on([](bool& finished, std::atomic<int>& dropped) -> Coro<void> {
        CancelHandle handle;
        {
            JoinSet<int> js;
            handle = js.spawn(wait_forever(DropToken{dropped}));
        }
        // The set's destructor cancelled the task.
        co_await until_finished(handle);
        handle.cancel();  // nothing left to cancel
        finished = handle.is_finished();
    }(finished, dropped));
    EXPECT_TRUE(finished);
    EXPECT_EQ(dropped, 1);
}

// ---------------------------------------------------------------------------
// JoinHandle::cancel_handle()
// ---------------------------------------------------------------------------

TEST(CancelHandleTest, EmptyJoinHandleGivesEmpty) {
    JoinHandle<int> handle;
    EXPECT_FALSE(handle.cancel_handle().valid());
}

TYPED_TEST(CancelHandleRuntimeTest, StopsDetachedTask) {
    bool tracking = false, detached_empty = false;
    std::atomic<int> dropped = 0;
    this->traits.rt.block_on([](bool& tracking, bool& detached_empty,
                                std::atomic<int>& dropped) -> Coro<void> {
        JoinHandle<int> handle = spawn(wait_forever(DropToken{dropped}));
        CancelHandle stop = handle.cancel_handle();
        tracking = stop.valid() && !stop.is_finished();
        std::move(handle).detach();
        detached_empty = !handle.cancel_handle().valid();  // NOLINT(bugprone-use-after-move)
        stop.cancel();
        co_await until_finished(stop);
    }(tracking, detached_empty, dropped));
    EXPECT_TRUE(tracking);
    EXPECT_TRUE(detached_empty);
    EXPECT_EQ(dropped, 1);
}

TYPED_TEST(CancelHandleRuntimeTest, JoinHandleCancelHandleCancelsAwaitedTask) {
    bool dropped_result = false;
    std::atomic<int> dropped = 0;
    this->traits.rt.block_on([](bool& dropped_result, std::atomic<int>& dropped) -> Coro<void> {
        JoinHandle<int> handle = spawn(wait_forever(DropToken{dropped}));
        handle.cancel_handle().cancel();
        // cancel_and_join() would cancel too; wait for the task through the stop handle.
        co_await until_finished(handle.cancel_handle());
        std::optional<int> result = co_await std::move(handle).cancel_and_join();
        dropped_result = !result.has_value();
    }(dropped_result, dropped));
    EXPECT_TRUE(dropped_result);
    EXPECT_EQ(dropped, 1);
}

// Awaiting consumes the JoinHandle, so the CancelHandle is taken first.
TYPED_TEST(CancelHandleRuntimeTest, SeesAwaitedTaskFinish) {
    bool finished = false, empty = false;
    this->traits.rt.block_on([](bool& finished, bool& empty) -> Coro<void> {
        JoinHandle<int> handle = spawn(ReadyFuture{3});
        CancelHandle stop = handle.cancel_handle();
        int value = co_await handle;
        CO_ASSERT_EQ(value, 3);
        finished = stop.is_finished();
        empty = !handle.cancel_handle().valid();  // NOLINT(bugprone-use-after-move)
    }(finished, empty));
    EXPECT_TRUE(finished);
    EXPECT_TRUE(empty);
}

// ---------------------------------------------------------------------------
// StreamHandle::cancel_handle()
// ---------------------------------------------------------------------------

TEST(CancelHandleTest, EmptyStreamHandleGivesEmpty) {
    StreamHandle<int> handle;
    EXPECT_FALSE(handle.cancel_handle().valid());
}

TYPED_TEST(CancelHandleRuntimeTest, StreamHandleCancelHandleEndsStream) {
    bool ended = false, finished = false;
    std::atomic<int> dropped = 0;
    this->traits.rt.block_on([](bool& ended, bool& finished, std::atomic<int>& dropped) -> Coro<void> {
        StreamHandle<int> handle = spawn(stream_forever(DropToken{dropped}));
        CancelHandle stop = handle.cancel_handle();
        CO_ASSERT(stop.valid());
        stop.cancel();
        // A cancelled stream ends cleanly.
        auto item = co_await next(handle);
        ended = !item.has_value();
        co_await until_finished(stop);
        finished = stop.is_finished();
    }(ended, finished, dropped));
    EXPECT_TRUE(ended);
    EXPECT_TRUE(finished);
    EXPECT_EQ(dropped, 1);
}

// ---------------------------------------------------------------------------
// BlockingHandle::cancel_handle()
// ---------------------------------------------------------------------------

#ifndef CORO_PICO

using namespace std::chrono_literals;

namespace {

// Sets `flag` when destroyed: the blocking callable unwound.
struct SetOnDestroy {
    std::shared_ptr<std::atomic<bool>> flag;
    ~SetOnDestroy() { flag->store(true); }
};

}  // namespace

TEST(CancelHandleBlockingTest, MovedFromBlockingHandleGivesEmpty) {
    Runtime rt(1);
    bool empty = false;
    rt.block_on([](bool& empty) -> Coro<void> {
        auto handle = spawn_blocking([] { return 1; });
        auto other = std::move(handle);
        empty = !handle.cancel_handle().valid();  // NOLINT(bugprone-use-after-move)
        CancelHandle stop = other.cancel_handle();
        int value = co_await other;
        CO_ASSERT_EQ(value, 1);
        CO_ASSERT(stop.is_finished());
    }(empty));
    EXPECT_TRUE(empty);
}

// Unlike JoinHandle, a detached BlockingHandle keeps its task, so it still gives a
// working handle.
TEST(CancelHandleBlockingTest, StopsDetachedBlockingTask) {
    Runtime rt(1);
    auto started = std::make_shared<std::atomic<bool>>(false);
    auto unwound = std::make_shared<std::atomic<bool>>(false);
    bool tracking = false;
    rt.block_on([](std::shared_ptr<std::atomic<bool>> started,
                   std::shared_ptr<std::atomic<bool>> unwound, bool& tracking) -> Coro<void> {
        auto handle = spawn_blocking([started, unwound] {
            SetOnDestroy guard{unwound};
            started->store(true);
            for (;;) {
                std::this_thread::sleep_for(1ms);
                blocking_cancellation_point();
            }
        });
        handle.detach();
        CancelHandle stop = handle.cancel_handle();
        tracking = stop.valid() && !stop.is_finished();
        // A cancel while still queued would skip the callable, and the guard with it.
        while (!started->load()) co_await sleep_for(1ms);
        stop.cancel();
        while (!stop.is_finished()) co_await sleep_for(1ms);
    }(started, unwound, tracking));
    EXPECT_TRUE(tracking);
    EXPECT_TRUE(unwound->load());
}

#endif  // !CORO_PICO
