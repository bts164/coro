#include <gtest/gtest.h>
#include "executor_traits.h"
#include <coro/coro.h>
#include <coro/future.h>
#include <coro/runtime/runtime.h>
#include <coro/sync/join.h>
#include <coro/task/spawn_builder.h>
#include <stdexcept>
#include <tuple>

using namespace coro;

// join() is otherwise exercised only through other tests. These cover a branch that
// reports PollDropped without join() having cancelled it, as a handle to a task cancelled
// elsewhere does. Before the fix join() marked such a branch done with no result and
// waited forever for it, so a regression hangs rather than fails.

namespace {

struct NoOpWaker : detail::Waker {
    void wake() override {}
    detail::Rc<detail::Waker> clone() override { return detail::make_rc<NoOpWaker>(); }
};

struct ImmediateInt {
    using OutputType = int;
    int m_value;
    PollResult<int> poll(detail::Context&) { return m_value; }
};

struct ThrowingFuture {
    using OutputType = int;
    PollResult<int> poll(detail::Context&) {
        return PollError(std::make_exception_ptr(std::runtime_error("bang")));
    }
};

template<typename T>
struct DroppedFuture {
    using OutputType = T;
    PollResult<T> poll(detail::Context&) { return PollDropped; }
};

// Pending until cancelled; then reports PollDropped on its next poll, as a drained
// Cancellable future does.
struct CancellableNever {
    using OutputType = int;
    bool* cancelled;
    void cancel() { *cancelled = true; }
    PollResult<int> poll(detail::Context&) {
        if (*cancelled) return PollDropped;
        return PollPending;
    }
};
static_assert(Cancellable<CancellableNever>);

Coro<void> joins_cancelled_handle() {
    JoinHandle<int> h = spawn(never<int>());
    h.cancel();
    (void)co_await join(std::move(h), ImmediateInt{1});
}

}  // namespace

// --- Polled directly ---

TEST(JoinTest, DroppedBranchFailsWithTaskCancelled) {
    detail::Context ctx(detail::make_rc<NoOpWaker>());
    auto j = join(ImmediateInt{1}, DroppedFuture<int>{});
    auto r = j.poll(ctx);
    ASSERT_TRUE(r.isError());
    EXPECT_THROW(r.rethrowIfError(), TaskCancelled);
}

TEST(JoinTest, VoidDroppedBranchFailsWithTaskCancelled) {
    detail::Context ctx(detail::make_rc<NoOpWaker>());
    auto j = join(DroppedFuture<void>{}, ImmediateInt{1});
    auto r = j.poll(ctx);
    ASSERT_TRUE(r.isError());
    EXPECT_THROW(r.rethrowIfError(), TaskCancelled);
}

// Fails as any other faulting branch does: the rest are cancelled and drained first.
TEST(JoinTest, DroppedBranchCancelsAndDrainsTheOthers) {
    detail::Context ctx(detail::make_rc<NoOpWaker>());
    bool cancelled = false;
    auto j = join(CancellableNever{&cancelled}, DroppedFuture<int>{});
    auto r = j.poll(ctx);
    EXPECT_TRUE(cancelled);
    ASSERT_TRUE(r.isError());
    EXPECT_THROW(r.rethrowIfError(), TaskCancelled);
}

// A branch that join() cancelled itself reports PollDropped while draining; that is
// not mistaken for a branch dropped elsewhere: the first error is the one delivered.
TEST(JoinTest, DrainedBranchDoesNotReplaceTheFirstError) {
    detail::Context ctx(detail::make_rc<NoOpWaker>());
    bool cancelled = false;
    auto j = join(CancellableNever{&cancelled}, ThrowingFuture{});
    auto r = j.poll(ctx);
    EXPECT_TRUE(cancelled);
    ASSERT_TRUE(r.isError());
    EXPECT_THROW(r.rethrowIfError(), std::runtime_error);
}

TEST(JoinTest, AllBranchesSucceed) {
    detail::Context ctx(detail::make_rc<NoOpWaker>());
    auto j = join(ImmediateInt{1}, ImmediateInt{2});
    auto r = j.poll(ctx);
    ASSERT_TRUE(r.isReady());
    EXPECT_EQ(std::get<0>(r.value()), 1);
    EXPECT_EQ(std::get<1>(r.value()), 2);
}

// --- On every executor, with a real handle ---

template<typename Traits>
class JoinRuntimeTest : public testing::Test {
protected:
    Traits traits;
};
TYPED_TEST_SUITE(JoinRuntimeTest, AllExecutors);

TYPED_TEST(JoinRuntimeTest, HandleToCancelledTaskFailsWithTaskCancelled) {
    EXPECT_THROW(this->traits.rt.block_on(joins_cancelled_handle()), TaskCancelled);
}
