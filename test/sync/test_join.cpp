#include <gtest/gtest.h>
#include "executor_traits.h"
#include "tuple_protocol.h"
#include <coro/coro.h>
#include <coro/coro_stream.h>
#include <coro/future.h>
#include <coro/stream.h>
#include <coro/runtime/runtime.h>
#include <coro/sync/join.h>
#include <coro/task/spawn_builder.h>
#include <memory>
#include <stdexcept>
#include <string>
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

// Ready at once with a shared object, so the test can tell whether the result was destroyed.
struct ReadyShared {
    using OutputType = std::shared_ptr<int>;
    std::shared_ptr<int> m_value;
    PollResult<std::shared_ptr<int>> poll(detail::Context&) { return std::move(m_value); }
};

// join()'s result is bound member by member: GCC 15 leaks a tuple-protocol binding held in a
// coroutine frame across a suspension (doc/known_issues.md, KI.2).
static_assert(coro_test::kBindsMemberwise<JoinOutput<int, std::string>>);
static_assert(std::is_same_v<decltype(join(ImmediateInt{1}, ImmediateInt{2}))::OutputType, JoinOutput<int, int>>);

// Binds both results of a join in a stream's frame and yields with them still bound.
CoroStream<int> bind_join_results(std::shared_ptr<int> value) {
    auto [a, b] = co_await join(ReadyShared{value}, ReadyShared{value});
    value.reset();
    co_yield *a + *b;
}

// Takes the stream's first item and drops the stream, destroying its frame at the yield.
Coro<void> take_first(CoroStream<int> stream) {
    (void)co_await next(stream);
}

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
    EXPECT_EQ(r.value().first, 1);
    EXPECT_EQ(r.value().second, 2);
}

// Ten branches, the most coro defines a JoinOutput for, read by index and by binding.
TEST(JoinTest, TenBranchesReadByIndexAndBinding) {
    detail::Context ctx(detail::make_rc<NoOpWaker>());
    auto j = join(ImmediateInt{0}, ImmediateInt{1}, ImmediateInt{2}, ImmediateInt{3}, ImmediateInt{4},
                  ImmediateInt{5}, ImmediateInt{6}, ImmediateInt{7}, ImmediateInt{8}, ImmediateInt{9});
    auto r = j.poll(ctx);
    ASSERT_TRUE(r.isReady());
    auto& out = r.value();
    EXPECT_EQ(coro::get<0>(out), 0);
    EXPECT_EQ(coro::get<9>(out), 9);
    EXPECT_EQ(out.tenth, 9);
    auto [a, b, c, d, e, f, g, h, i, k] = std::move(out);
    EXPECT_EQ(a + b + c + d + e + f + g + h + i + k, 45);
}

// --- On every executor, with a real handle ---

template<typename Traits>
class JoinRuntimeTest : public testing::Test {
protected:
    Traits traits;
};
TYPED_TEST_SUITE(JoinRuntimeTest, AllExecutors);

TYPED_TEST(JoinRuntimeTest, ResultsBoundInAStreamAreDestroyedWithIt) {
    auto value = std::make_shared<int>(1);
    const std::weak_ptr<int> watch = value;
    this->traits.rt.block_on(take_first(bind_join_results(std::move(value))));
    EXPECT_EQ(watch.use_count(), 0);
}

TYPED_TEST(JoinRuntimeTest, HandleToCancelledTaskFailsWithTaskCancelled) {
    EXPECT_THROW(this->traits.rt.block_on(joins_cancelled_handle()), TaskCancelled);
}
