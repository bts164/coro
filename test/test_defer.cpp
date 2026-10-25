#include <gtest/gtest.h>
#include "executor_traits.h"
#include "co_assert.h"
#include <coro/coro.h>
#include <coro/defer.h>
#include <coro/future.h>
#include <coro/runtime/runtime.h>
#include <coro/stream.h>
#include <coro/task/cancel_handle.h>
#include <coro/task/join_set.h>
#include <coro/task/yield_now.h>
#include <atomic>
#include <stdexcept>
#include <string>
#include <type_traits>

using namespace coro;

namespace {

using PlainDefer = Defer<void (*)()>;
static_assert(!std::is_copy_constructible_v<PlainDefer>);
static_assert(!std::is_move_constructible_v<PlainDefer>);
static_assert(!std::is_copy_assignable_v<PlainDefer>);

// Sets `started` once its Defer exists, so a cancel after that is a cancel of a
// suspended frame, not of a task that never ran.
Coro<void> deferred_then_wait(std::atomic<bool>& started, std::atomic<bool>& ran) {
    Defer mark([&ran] { ran = true; });
    started = true;
    co_await never<void>();
}

}  // namespace

TEST(DeferTest, RunsWhenScopeEnds) {
    int calls = 0;
    {
        Defer count([&calls] { ++calls; });
        EXPECT_EQ(calls, 0);
    }
    EXPECT_EQ(calls, 1);
}

TEST(DeferTest, RunsWhenExceptionLeavesScope) {
    int calls = 0;
    EXPECT_THROW(
        {
            Defer count([&calls] { ++calls; });
            throw std::runtime_error("leaving");
        },
        std::runtime_error);
    EXPECT_EQ(calls, 1);
}

TEST(DeferTest, RunsInReverseOrderOfDeclaration) {
    std::string order;
    {
        Defer first([&order] { order += '1'; });
        Defer second([&order] { order += '2'; });
    }
    EXPECT_EQ(order, "21");
}

TEST(DeferTest, EarlyCallRunsOnlyOnce) {
    int calls = 0;
    {
        Defer count([&calls] { ++calls; });
        count();
        EXPECT_EQ(calls, 1);
    }
    EXPECT_EQ(calls, 1);
}

TEST(DeferTest, SecondEarlyCallThrows) {
    int calls = 0;
    {
        Defer count([&calls] { ++calls; });
        count();
        EXPECT_THROW(count(), std::logic_error);
    }
    EXPECT_EQ(calls, 1);
}

TEST(DeferTest, ThrowingEarlyCallReachesCallerAndIsNotRetried) {
    int calls = 0;
    {
        Defer fail([&calls] {
            ++calls;
            throw std::runtime_error("cleanup failed");
        });
        EXPECT_THROW(fail(), std::runtime_error);
    }
    EXPECT_EQ(calls, 1);
}

TEST(DeferTest, DismissedIsNeverCalled) {
    int calls = 0;
    {
        Defer count([&calls] { ++calls; });
        count.dismiss();
    }
    EXPECT_EQ(calls, 0);
}

TEST(DeferTest, CallAfterDismissThrows) {
    int calls = 0;
    {
        Defer count([&calls] { ++calls; });
        count.dismiss();
        EXPECT_THROW(count(), std::logic_error);
    }
    EXPECT_EQ(calls, 0);
}

TEST(DeferTest, DismissAfterCallThrows) {
    int calls = 0;
    {
        Defer count([&calls] { ++calls; });
        count();
        EXPECT_THROW(count.dismiss(), std::logic_error);
    }
    EXPECT_EQ(calls, 1);
}

template<typename Traits>
class DeferRuntimeTest : public testing::Test {
protected:
    Traits traits;
};
TYPED_TEST_SUITE(DeferRuntimeTest, AllExecutors);

TYPED_TEST(DeferRuntimeTest, RunsWhenSuspendedCoroutineIsCancelled) {
    bool cancelled = false;
    std::atomic<bool> started = false, ran = false;
    this->traits.rt.block_on([](bool& cancelled, std::atomic<bool>& started,
                                std::atomic<bool>& ran) -> Coro<void> {
        JoinSet<void> js;
        CancelHandle handle = js.spawn(deferred_then_wait(started, ran));
        while (!started) co_await yield_now();
        handle.cancel();
        auto item = co_await next(js);
        CO_ASSERT(item.has_value());
        cancelled = item->is_cancelled();
    }(cancelled, started, ran));
    EXPECT_TRUE(cancelled);
    EXPECT_TRUE(ran);
}
