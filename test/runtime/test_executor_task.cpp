#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <coro/detail/task.h>
#include <coro/detail/task_state.h>
#include <atomic>
#include <chrono>

using namespace coro;
using namespace coro::detail;
using namespace std::chrono_literals;

class MockWaker : public Waker {
public:
    MOCK_METHOD(void, wake, (), (override));
    MOCK_METHOD(Rc<Waker>, clone, (), (override));
};

struct ImmediateFuture {
    using OutputType = int;
    int m_value;
    PollResult<int> poll(Context&) { return m_value; }
};

struct NeverReadyFuture {
    using OutputType = int;
    PollResult<int> poll(Context&) { return PollPending; }
};

// --- TaskImpl tests ---

TEST(TaskTest, WrapsAnyFuture) {
    TaskImpl<ImmediateFuture> t(ImmediateFuture{42});
    (void)t;
}

TEST(TaskTest, CompletedTaskPollReturnsTrue) {
    auto waker = make_rc<MockWaker>();
    Context ctx(waker);
    TaskImpl<ImmediateFuture> t(ImmediateFuture{1});
    EXPECT_TRUE(t.poll(ctx));
}

TEST(TaskTest, PendingTaskPollReturnsFalse) {
    auto waker = make_rc<MockWaker>();
    Context ctx(waker);
    TaskImpl<NeverReadyFuture> t(NeverReadyFuture{});
    EXPECT_FALSE(t.poll(ctx));
}

TEST(TaskTest, PollWritesResultToState) {
    auto waker = make_rc<MockWaker>();
    Context ctx(waker);
    auto impl = std::make_shared<TaskImpl<ImmediateFuture>>(ImmediateFuture{99});
    std::shared_ptr<TaskState<int>> state = impl;
    impl->poll(ctx);
    std::lock_guard lock(state->mutex);
    ASSERT_TRUE(state->result.has_value());
    EXPECT_EQ(*state->result, 99);
}

TEST(TaskTest, PollWritesExceptionToState) {
    auto waker = make_rc<MockWaker>();
    Context ctx(waker);
    struct ThrowingFuture {
        using OutputType = int;
        PollResult<int> poll(Context&) {
            return PollError(std::make_exception_ptr(std::runtime_error("boom")));
        }
    };
    auto impl = std::make_shared<TaskImpl<ThrowingFuture>>(ThrowingFuture{});
    std::shared_ptr<TaskState<int>> state = impl;
    impl->poll(ctx);
    std::lock_guard lock(state->mutex);
    EXPECT_NE(state->exception, nullptr);
}

TEST(TaskTest, CancelledTaskIsSkipped) {
    auto waker = make_rc<MockWaker>();
    Context ctx(waker);
    auto impl = std::make_shared<TaskImpl<ImmediateFuture>>(ImmediateFuture{5});
    std::shared_ptr<TaskState<int>> state = impl;
    state->cancelled.store(true);
    EXPECT_TRUE(impl->poll(ctx));  // treated as done (cancelled)
    std::lock_guard lock(state->mutex);
    EXPECT_FALSE(state->result.has_value());
}

// Executor-level ready-queue and parking tests live in test_current_thread_executor.cpp.

// --- SchedulingState tests ---

TEST(SchedulingStateTest, InitialStateIsIdle) {
    TaskImpl<ImmediateFuture> t(ImmediateFuture{1});
    EXPECT_EQ(t.scheduling_state.load(), SchedulingState::Idle);
}
