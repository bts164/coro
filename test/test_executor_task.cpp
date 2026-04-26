#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <coro/runtime/single_threaded_executor.h>
#include <coro/detail/task.h>
#include <coro/detail/task_state.h>
#include <atomic>
#include <chrono>
#include <future>
#include <thread>

using namespace coro;
using namespace coro::detail;
using namespace std::chrono_literals;

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

// --- TaskImpl tests ---

TEST(TaskTest, WrapsAnyFuture) {
    TaskImpl<ImmediateFuture> t(ImmediateFuture{42});
    (void)t;
}

TEST(TaskTest, CompletedTaskPollReturnsTrue) {
    auto waker = std::make_shared<MockWaker>();
    Context ctx(waker);
    TaskImpl<ImmediateFuture> t(ImmediateFuture{1});
    EXPECT_TRUE(t.poll(ctx));
}

TEST(TaskTest, PendingTaskPollReturnsFalse) {
    auto waker = std::make_shared<MockWaker>();
    Context ctx(waker);
    TaskImpl<NeverFuture> t(NeverFuture{});
    EXPECT_FALSE(t.poll(ctx));
}

TEST(TaskTest, PollWritesResultToState) {
    auto waker = std::make_shared<MockWaker>();
    Context ctx(waker);
    auto impl = std::make_shared<TaskImpl<ImmediateFuture>>(ImmediateFuture{99});
    std::shared_ptr<TaskState<int>> state = impl;
    impl->poll(ctx);
    std::lock_guard lock(state->mutex);
    ASSERT_TRUE(state->result.has_value());
    EXPECT_EQ(*state->result, 99);
}

TEST(TaskTest, PollWritesExceptionToState) {
    auto waker = std::make_shared<MockWaker>();
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
    auto waker = std::make_shared<MockWaker>();
    Context ctx(waker);
    auto impl = std::make_shared<TaskImpl<ImmediateFuture>>(ImmediateFuture{5});
    std::shared_ptr<TaskState<int>> state = impl;
    state->cancelled.store(true);
    EXPECT_TRUE(impl->poll(ctx));  // treated as done (cancelled)
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
    auto impl = std::make_shared<TaskImpl<ImmediateFuture>>(ImmediateFuture{7});
    std::shared_ptr<TaskState<int>> state = impl;
    ex.schedule(std::shared_ptr<TaskBase>(std::move(impl)));
    EXPECT_TRUE(ex.poll_ready_tasks());
    EXPECT_TRUE(ex.empty());
    std::lock_guard lock(state->mutex);
    ASSERT_TRUE(state->result.has_value());
    EXPECT_EQ(*state->result, 7);
}

TEST(SingleThreadedExecutorTest, PendingTaskBecomesIdle) {
    SingleThreadedExecutor ex;
    auto impl = std::make_shared<TaskImpl<NeverFuture>>(NeverFuture{});
    ex.schedule(std::shared_ptr<TaskBase>(std::move(impl)));
    ex.poll_ready_tasks();
    // Task returned Pending without storing a waker — transitions to Idle and
    // then drops immediately (no waker holds the ref). Ready queue is empty.
    EXPECT_TRUE(ex.empty());
}

TEST(SingleThreadedExecutorTest, SelfWakingTaskCompletesInTwoPasses) {
    SingleThreadedExecutor ex;
    auto impl = std::make_shared<TaskImpl<SelfWakingFuture>>(SelfWakingFuture{42});
    std::shared_ptr<TaskState<int>> state = impl;
    ex.schedule(std::shared_ptr<TaskBase>(std::move(impl)));

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
    auto impl = std::make_shared<TaskImpl<ImmediateFuture>>(ImmediateFuture{1});
    std::shared_ptr<TaskState<int>> state = impl;
    state->join_waker = join_waker;
    ex.schedule(std::shared_ptr<TaskBase>(std::move(impl)));
    ex.poll_ready_tasks();
}

TEST(SingleThreadedExecutorTest, WaitForComplete) {
    auto join_waker = std::make_shared<MockWaker>();
    EXPECT_CALL(*join_waker, wake()).Times(1);

    SingleThreadedExecutor ex;
    auto impl = std::make_shared<TaskImpl<ImmediateFuture>>(ImmediateFuture{1});
    std::shared_ptr<TaskState<int>> state = impl;
    state->join_waker = join_waker;
    ex.schedule(std::shared_ptr<TaskBase>(std::move(impl)));
    ex.poll_ready_tasks();
}

// A future that suspends on the first poll, hands the waker to a std::promise
// for safe cross-thread transfer, then returns the value on the second poll.
// std::promise::set_value / future::get provide the happens-before needed to
// avoid a data race between the future setting the waker and the waker thread
// reading it.
struct PromiseWakeFuture {
    using OutputType = int;
    int                                         m_value;
    std::promise<std::shared_ptr<detail::Waker>>* m_promise;
    bool                                         m_first = true;

    PollResult<int> poll(Context& ctx) {
        if (m_first) {
            m_first = false;
            m_promise->set_value(ctx.getWaker()->clone());
            return PollPending;
        }
        return m_value;
    }
};

TEST(SingleThreadedExecutorTest, ExternalThreadWakeup) {
    // Verifies that a task suspended waiting for an external wake is correctly
    // resumed when wake() is called from another thread, and that
    // wait_for_completion() does not return prematurely.
    std::promise<std::shared_ptr<detail::Waker>> waker_promise;
    auto waker_future = waker_promise.get_future();

    SingleThreadedExecutor ex;
    auto impl = std::make_shared<TaskImpl<PromiseWakeFuture>>(
        PromiseWakeFuture{99, &waker_promise});
    std::shared_ptr<TaskState<int>> state = impl;
    ex.schedule(std::shared_ptr<TaskBase>(std::move(impl)));

    // The waker thread blocks on future::get() until the task has been polled
    // and set the promise — no sleep-based races.
    std::thread waker_thread([wf = std::move(waker_future)]() mutable {
        wf.get()->wake();
    });

    ex.wait_for_completion(*state);
    waker_thread.join();

    std::lock_guard lock(state->mutex);
    ASSERT_TRUE(state->result.has_value());
    EXPECT_EQ(*state->result, 99);
}

TEST(SingleThreadedExecutorTest, WaitForCompletionDoesNotReturnEarlyWithPendingTask) {
    // Regression test for the premature-return bug: the old implementation
    // exited wait_for_completion() when the ready queue was empty, before the
    // task had a chance to be woken externally. With the injection queue +
    // condvar fix it must block until the task completes.
    std::promise<std::shared_ptr<detail::Waker>> waker_promise;
    auto waker_future = waker_promise.get_future();

    SingleThreadedExecutor ex;
    auto impl = std::make_shared<TaskImpl<PromiseWakeFuture>>(
        PromiseWakeFuture{7, &waker_promise});
    std::shared_ptr<TaskState<int>> state = impl;
    ex.schedule(std::shared_ptr<TaskBase>(std::move(impl)));

    std::thread waker_thread([wf = std::move(waker_future)]() mutable {
        wf.get()->wake();
    });

    ex.wait_for_completion(*state);
    waker_thread.join();

    std::lock_guard lock(state->mutex);
    ASSERT_TRUE(state->result.has_value());
    EXPECT_EQ(*state->result, 7);
}

// --- SchedulingState tests ---

TEST(SchedulingStateTest, InitialStateIsIdle) {
    TaskImpl<ImmediateFuture> t(ImmediateFuture{1});
    EXPECT_EQ(t.scheduling_state.load(), SchedulingState::Idle);
}

TEST(SchedulingStateTest, ScheduleSetsNotified) {
    SingleThreadedExecutor ex;
    // Keep a shared_ptr alive so we can inspect scheduling_state after schedule().
    auto impl = std::make_shared<TaskImpl<NeverFuture>>(NeverFuture{});
    TaskBase* raw = impl.get();
    ex.schedule(std::shared_ptr<TaskBase>(impl));  // shares ownership; impl still valid
    EXPECT_EQ(raw->scheduling_state.load(), SchedulingState::Notified);
}
