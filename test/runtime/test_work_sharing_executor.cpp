#include <gtest/gtest.h>
#include <coro/runtime/runtime.h>
#include <coro/runtime/work_sharing_executor.h>
#include <utility>
#include <coro/coro.h>
#include <coro/task/join_handle.h>
#include <coro/task/join_set.h>
#include <coro/co_invoke.h>
#include <atomic>
#include <stdexcept>

using namespace coro;

// ---------------------------------------------------------------------------
// Helper futures and coroutines
// ---------------------------------------------------------------------------

struct ImmediateInt {
    using OutputType = int;
    int m_value;
    PollResult<int> poll(detail::Context&) { return m_value; }
};

struct ImmediateVoid {
    using OutputType = void;
    PollResult<void> poll(detail::Context&) { return PollReady; }
};

struct ThrowingFuture {
    using OutputType = int;
    PollResult<int> poll(detail::Context&) {
        return PollError(std::make_exception_ptr(std::runtime_error("boom")));
    }
};

// Suspends once, stores the waker, then is woken externally.
struct SuspendOnceFuture {
    using OutputType = int;
    int                         m_value;
    std::shared_ptr<detail::Waker> m_waker;
    bool                        m_polled_once = false;

    PollResult<int> poll(detail::Context& ctx) {
        if (!m_polled_once) {
            m_polled_once = true;
            m_waker = ctx.getWaker()->clone();
            return PollPending;
        }
        return m_value;
    }
};

Coro<int> simple_coro() { co_return 42; }

Coro<void> void_coro() { co_return; }

Coro<int> throwing_coro() {
    throw std::runtime_error("coro error");
    co_return 0;
}

Coro<int> nested_spawn_coro() {
    auto h = coro::spawn(ImmediateInt{99});
    co_return co_await std::move(h);
}

Coro<void> count_tasks_coro(std::atomic<int>& counter, int n) {
    co_await coro::co_invoke([&]() -> Coro<void> {
        JoinSet<void> js;
        for (int i = 0; i < n; ++i)
            js.spawn(coro::co_invoke([&]() -> Coro<void> {
                counter.fetch_add(1, std::memory_order_relaxed);
                co_return;
            }));
        co_await js.drain();
    });
}

// ---------------------------------------------------------------------------
// Tests: Runtime construction with multiple threads
// ---------------------------------------------------------------------------

TEST(WorkSharingExecutorTest, RuntimeConstructsWithMultipleThreads) {
    Runtime rt(std::in_place_type<WorkSharingExecutor>, 2);
    (void)rt;
}

TEST(WorkSharingExecutorTest, RuntimeConstructsWithFourThreads) {
    Runtime rt(std::in_place_type<WorkSharingExecutor>, 4);
    (void)rt;
}

// ---------------------------------------------------------------------------
// Tests: block_on with multi-threaded runtime
// ---------------------------------------------------------------------------

TEST(WorkSharingExecutorTest, BlockOnSimpleCoroReturnsValue) {
    Runtime rt(std::in_place_type<WorkSharingExecutor>, 2);
    EXPECT_EQ(rt.block_on(simple_coro()), 42);
}

TEST(WorkSharingExecutorTest, BlockOnVoidCoroCompletes) {
    Runtime rt(std::in_place_type<WorkSharingExecutor>, 2);
    rt.block_on(void_coro());
}

TEST(WorkSharingExecutorTest, BlockOnRethrowsException) {
    Runtime rt(std::in_place_type<WorkSharingExecutor>, 2);
    EXPECT_THROW(rt.block_on(throwing_coro()), std::runtime_error);
}

TEST(WorkSharingExecutorTest, BlockOnImmediateFutureReturnsValue) {
    Runtime rt(std::in_place_type<WorkSharingExecutor>, 2);
    EXPECT_EQ(rt.block_on(ImmediateInt{7}), 7);
}

// ---------------------------------------------------------------------------
// Tests: spawn from within a worker thread
// ---------------------------------------------------------------------------

TEST(WorkSharingExecutorTest, SpawnFromWorkerThreadReturnsValue) {
    Runtime rt(std::in_place_type<WorkSharingExecutor>, 2);
    EXPECT_EQ(rt.block_on(nested_spawn_coro()), 99);
}

// ---------------------------------------------------------------------------
// Tests: concurrent task execution
// ---------------------------------------------------------------------------

TEST(WorkSharingExecutorTest, MultipleConcurrentTasksAllComplete) {
    Runtime rt(std::in_place_type<WorkSharingExecutor>, 4);
    std::atomic<int> counter{0};
    rt.block_on(count_tasks_coro(counter, 10));
    EXPECT_EQ(counter.load(), 10);
}

TEST(WorkSharingExecutorTest, LargerFanOutAllTasksComplete) {
    Runtime rt(std::in_place_type<WorkSharingExecutor>, 4);
    std::atomic<int> counter{0};
    rt.block_on(count_tasks_coro(counter, 100));
    EXPECT_EQ(counter.load(), 100);
}

// ---------------------------------------------------------------------------
// Tests: JoinSet on multi-threaded runtime
// ---------------------------------------------------------------------------

Coro<int> compute(int x) { co_return x * x; }

Coro<int> sum_of_squares(int n) {
    co_return co_await coro::co_invoke([&]() -> Coro<int> {
        JoinSet<int> js;
        for (int i = 1; i <= n; ++i)
            js.spawn(compute(i));

        int total = 0;
        while (auto result = co_await coro::next(js))
            total += *result;
        co_return total;
    });
}

TEST(WorkSharingExecutorTest, JoinSetCollectsResultsOnMultiThreadedRuntime) {
    Runtime rt(std::in_place_type<WorkSharingExecutor>, 2);
    // 1^2 + 2^2 + 3^2 + 4^2 = 30
    EXPECT_EQ(rt.block_on(sum_of_squares(4)), 30);
}

// ---------------------------------------------------------------------------
// Tests: exception propagation through JoinSet
// ---------------------------------------------------------------------------

Coro<int> throwing_child() {
    throw std::runtime_error("child error");
    co_return 0;
}

Coro<void> join_set_with_exception() {
    co_await coro::co_invoke([]() -> Coro<void> {
        JoinSet<int> js;
        js.spawn(throwing_child());
        while (auto result = co_await coro::next(js))
            (void)result;
    });
}

TEST(WorkSharingExecutorTest, JoinSetExceptionPropagatesOnMultiThreadedRuntime) {
    Runtime rt(std::in_place_type<WorkSharingExecutor>, 2);
    EXPECT_THROW(rt.block_on(join_set_with_exception()), std::runtime_error);
}

// ---------------------------------------------------------------------------
// Tests: WorkSharingExecutor direct construction
// ---------------------------------------------------------------------------

TEST(WorkSharingExecutorTest, DirectConstructionAndSchedule) {
    Runtime rt(1);  // use rt for thread-local setup
    WorkSharingExecutor exec(&rt, 2);
    // schedule an immediate task; executor should not crash on destruction
    auto impl = std::make_shared<detail::TaskImpl<ImmediateVoid>>(ImmediateVoid{});
    exec.schedule(std::shared_ptr<detail::TaskBase>(std::move(impl)));
}
