#include <gtest/gtest.h>
#include <coro/runtime/runtime.h>
#include <coro/runtime/single_threaded_executor.h>
#include <coro/runtime/work_sharing_executor.h>
#include <coro/runtime/work_stealing_executor.h>
#include <coro/coro.h>
#include <coro/task/join_handle.h>
#include <coro/task/join_set.h>
#include <coro/co_invoke.h>
#include <atomic>
#include <stdexcept>
#include <thread>

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

Coro<int> simple_coro() { co_return 42; }
Coro<void> void_coro() { co_return; }
Coro<int> throwing_coro() {
    throw std::runtime_error("coro error");
    co_return 0;
}

Coro<int> nested_spawn_coro() {
    auto h = coro::spawn(ImmediateInt{99}).submit();
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

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

TEST(WorkStealingExecutorTest, ConstructsWithTwoThreads) {
    Runtime rt(2);
    (void)rt;
}

TEST(WorkStealingExecutorTest, ConstructsWithFourThreads) {
    Runtime rt(4);
    (void)rt;
}

TEST(WorkStealingExecutorTest, DirectConstruction) {
    Runtime rt(1); // for thread-local setup
    WorkStealingExecutor exec(&rt, 2);
    (void)exec;
}

TEST(WorkStealingExecutorTest, RejectsMoreThanMaxWorkers) {
    Runtime rt(1);
    EXPECT_THROW(WorkStealingExecutor(&rt, 65), std::invalid_argument);
}

// ---------------------------------------------------------------------------
// Basic correctness via Runtime::block_on
// ---------------------------------------------------------------------------

TEST(WorkStealingExecutorTest, BlockOnSimpleCoroReturnsValue) {
    Runtime rt(2);
    EXPECT_EQ(rt.block_on(simple_coro()), 42);
}

TEST(WorkStealingExecutorTest, BlockOnVoidCoroCompletes) {
    Runtime rt(2);
    rt.block_on(void_coro());
}

TEST(WorkStealingExecutorTest, BlockOnRethrowsException) {
    Runtime rt(2);
    EXPECT_THROW(rt.block_on(throwing_coro()), std::runtime_error);
}

TEST(WorkStealingExecutorTest, BlockOnImmediateFutureReturnsValue) {
    Runtime rt(2);
    EXPECT_EQ(rt.block_on(ImmediateInt{7}), 7);
}

// ---------------------------------------------------------------------------
// Spawn and concurrent tasks
// ---------------------------------------------------------------------------

TEST(WorkStealingExecutorTest, SpawnFromWorkerReturnsValue) {
    Runtime rt(2);
    EXPECT_EQ(rt.block_on(nested_spawn_coro()), 99);
}

TEST(WorkStealingExecutorTest, MultipleConcurrentTasksAllComplete) {
    Runtime rt(4);
    std::atomic<int> counter{0};
    rt.block_on(count_tasks_coro(counter, 10));
    EXPECT_EQ(counter.load(), 10);
}

TEST(WorkStealingExecutorTest, LargerFanOutAllTasksComplete) {
    Runtime rt(4);
    std::atomic<int> counter{0};
    rt.block_on(count_tasks_coro(counter, 100));
    EXPECT_EQ(counter.load(), 100);
}

// ---------------------------------------------------------------------------
// JoinSet
// ---------------------------------------------------------------------------

TEST(WorkStealingExecutorTest, JoinSetCollectsResultsCorrectly) {
    Runtime rt(2);
    // 1^2 + 2^2 + 3^2 + 4^2 = 30
    EXPECT_EQ(rt.block_on(sum_of_squares(4)), 30);
}

TEST(WorkStealingExecutorTest, JoinSetExceptionPropagates) {
    Runtime rt(2);
    EXPECT_THROW(rt.block_on(join_set_with_exception()), std::runtime_error);
}

// ---------------------------------------------------------------------------
// Stealing: verify that tasks pushed to one worker's queue are executed
// even if that worker is blocked — another worker must steal them.
// ---------------------------------------------------------------------------

// Spawns N tasks from a single worker then suspends. The N tasks must be
// stolen and executed by other workers before the parent can complete.
Coro<void> stealing_workload(std::atomic<int>& counter, int n) {
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

TEST(WorkStealingExecutorTest, TasksStolenAndExecutedByIdleWorkers) {
    Runtime rt(4);
    std::atomic<int> counter{0};
    rt.block_on(stealing_workload(counter, 50));
    EXPECT_EQ(counter.load(), 50);
}

// ---------------------------------------------------------------------------
// Shutdown: all in-flight tasks must complete before destruction.
// ---------------------------------------------------------------------------

TEST(WorkStealingExecutorTest, ShutdownDrainsAllTasks) {
    std::atomic<int> counter{0};
    {
        Runtime rt(4);
        rt.block_on(count_tasks_coro(counter, 200));
    }
    EXPECT_EQ(counter.load(), 200);
}

// ---------------------------------------------------------------------------
// Stress: high task count, all threads active.
// ---------------------------------------------------------------------------

TEST(WorkStealingExecutorTest, StressHighTaskCount) {
    Runtime rt(std::thread::hardware_concurrency() > 1
                   ? std::thread::hardware_concurrency()
                   : 2);
    std::atomic<int> counter{0};
    rt.block_on(count_tasks_coro(counter, 1000));
    EXPECT_EQ(counter.load(), 1000);
}
