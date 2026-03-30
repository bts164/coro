#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <coro/runtime/single_threaded_executor.h>
#include <coro/detail/task.h>
#include <coro/detail/task_state.h>

using namespace coro;
using namespace coro::detail;

class MockWaker : public Waker {
public:
    MOCK_METHOD(void, wake, (), (override));
    MOCK_METHOD(std::shared_ptr<Waker>, clone, (), (override));
};

struct ImmediateFuture {
    using OutputType = int;
    int m_value;
    PollResult<int> poll(Context&) { return m_value; }
};

struct NeverFuture {
    using OutputType = int;
    PollResult<int> poll(Context&) { return PollPending; }
};

// Returns Pending once, calls wake(), then returns Ready.
struct SelfWakingFuture {
    using OutputType = int;
    int  m_value;
    bool m_polled_once = false;
    PollResult<int> poll(Context& ctx) {
        if (!m_polled_once) {
            m_polled_once = true;
            ctx.getWaker()->wake();
            return PollPending;
        }
        return m_value;
    }
};

// --- Task tests ---

TEST(TaskTest, WrapsAnyFuture) {
    Task t(ImmediateFuture{42});
    (void)t;
}

TEST(TaskTest, IsMovable) {
    Task t1(ImmediateFuture{1});
    Task t2(std::move(t1));
    (void)t2;
}

TEST(TaskTest, CompletedTaskPollReturnsTrue) {
    auto waker = std::make_shared<MockWaker>();
    Context ctx(waker);
    Task t(ImmediateFuture{1});
    EXPECT_TRUE(t.poll(ctx));
}

TEST(TaskTest, PendingTaskPollReturnsFalse) {
    auto waker = std::make_shared<MockWaker>();
    Context ctx(waker);
    Task t(NeverFuture{});
    EXPECT_FALSE(t.poll(ctx));
}

TEST(TaskTest, PollWritesResultToState) {
    auto waker = std::make_shared<MockWaker>();
    Context ctx(waker);
    auto state = std::make_shared<TaskState<int>>();
    Task t(ImmediateFuture{99}, state);
    t.poll(ctx);
    std::lock_guard lock(state->mutex);
    ASSERT_TRUE(state->result.has_value());
    EXPECT_EQ(*state->result, 99);
}

TEST(TaskTest, PollWritesExceptionToState) {
    auto waker = std::make_shared<MockWaker>();
    Context ctx(waker);
    auto state = std::make_shared<TaskState<int>>();
    struct ThrowingFuture {
        using OutputType = int;
        PollResult<int> poll(Context&) {
            return PollError(std::make_exception_ptr(std::runtime_error("boom")));
        }
    };
    Task t(ThrowingFuture{}, state);
    t.poll(ctx);
    std::lock_guard lock(state->mutex);
    EXPECT_NE(state->exception, nullptr);
}

TEST(TaskTest, CancelledTaskIsSkipped) {
    auto waker = std::make_shared<MockWaker>();
    Context ctx(waker);
    auto state = std::make_shared<TaskState<int>>();
    state->cancelled.store(true);
    Task t(ImmediateFuture{5}, state);
    EXPECT_TRUE(t.poll(ctx));  // treated as done (cancelled)
    std::lock_guard lock(state->mutex);
    EXPECT_FALSE(state->result.has_value());
}

// --- SingleThreadedExecutor tests ---

TEST(SingleThreadedExecutorTest, EmptyReturnsFalse) {
    SingleThreadedExecutor ex;
    EXPECT_FALSE(ex.poll_ready_tasks());
}

TEST(SingleThreadedExecutorTest, ScheduleAndPollTask) {
    SingleThreadedExecutor ex;
    auto state = std::make_shared<TaskState<int>>();
    ex.schedule(std::make_unique<Task>(ImmediateFuture{7}, state));
    EXPECT_TRUE(ex.poll_ready_tasks());
    EXPECT_TRUE(ex.empty());
    std::lock_guard lock(state->mutex);
    ASSERT_TRUE(state->result.has_value());
    EXPECT_EQ(*state->result, 7);
}

TEST(SingleThreadedExecutorTest, PendingTaskMovedToSuspended) {
    SingleThreadedExecutor ex;
    ex.schedule(std::make_unique<Task>(NeverFuture{}));
    ex.poll_ready_tasks();
    // Task returned Pending — ready queue is empty but task lives in suspended.
    EXPECT_TRUE(ex.empty());
}

TEST(SingleThreadedExecutorTest, SelfWakingTaskCompletesInTwoPasses) {
    SingleThreadedExecutor ex;
    auto state = std::make_shared<TaskState<int>>();
    ex.schedule(std::make_unique<Task>(SelfWakingFuture{42}, state));

    ex.poll_ready_tasks();  // Pending; waker fires synchronously → re-enqueued
    {
        std::lock_guard lock(state->mutex);
        EXPECT_FALSE(state->result.has_value());
    }
    EXPECT_FALSE(ex.empty());  // re-enqueued in m_ready

    ex.poll_ready_tasks();  // Ready(42)
    {
        std::lock_guard lock(state->mutex);
        ASSERT_TRUE(state->result.has_value());
        EXPECT_EQ(*state->result, 42);
    }
}

TEST(SingleThreadedExecutorTest, SetResultCallsJoinWaker) {
    auto join_waker = std::make_shared<MockWaker>();
    EXPECT_CALL(*join_waker, wake()).Times(1);

    SingleThreadedExecutor ex;
    auto state = std::make_shared<TaskState<int>>();
    state->join_waker = join_waker;
    ex.schedule(std::make_unique<Task>(ImmediateFuture{1}, state));
    ex.poll_ready_tasks();
}
