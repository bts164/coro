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

class ArgMarker
{
    ArgMarker() = default;
public:
    static inline std::atomic_size_t depth_counters[7];
    static inline std::atomic_size_t clone_depth_counters[7];
    static void reset_counters() {
        for (auto& c : depth_counters)
            c.store(0, std::memory_order_release);
        for (auto& c : clone_depth_counters)
            c.store(0, std::memory_order_release);
    }

    ArgMarker(std::atomic_size_t& id, size_t depth) :
        m_id(&id),
        m_depth(depth)
    {
        m_id->store(0, std::memory_order_release);
    }
    ArgMarker clone() const
    {
        ArgMarker a;
        a.m_id = m_id;
        a.m_clone_depth = m_depth;
        return a;
    }
    ArgMarker(const ArgMarker&) = delete;
    ArgMarker(ArgMarker&&other) :
        m_id(std::exchange(other.m_id, nullptr)),
        m_depth(std::exchange(other.m_depth, std::nullopt)),
        m_clone_depth(std::exchange(other.m_clone_depth, std::nullopt))
    {}
    ArgMarker& operator=(const ArgMarker&) = delete;
    ArgMarker& operator=(ArgMarker&&) = delete;
    ~ArgMarker()
    {
        if (m_id) {
            m_id->fetch_add(1);
        }
        if (m_depth) {
            depth_counters[m_depth.value()].fetch_add(1);
            //std::cout << "Set arg to true\n" << std::flush;
        }
        if (m_clone_depth) {
            clone_depth_counters[m_clone_depth.value()].fetch_add(1);
            //std::cout << "Set arg to true\n" << std::flush;
        }
    }
private:
    std::atomic_size_t *m_id = nullptr;
    std::optional<std::size_t> m_depth;
    std::optional<std::size_t> m_clone_depth;
};

#include <co_assert.h>
#include <coro/runtime/work_sharing_executor.h>
#include <coro/sync/join.h>

template<std::size_t... Is>
Coro<size_t> skynet(size_t my_num, size_t remaining, size_t depth, ArgMarker arg_lifetime_marker, std::index_sequence<Is...> seq) {
    size_t sum;
    {
        auto arg = arg_lifetime_marker.clone();
        if (remaining == 1) co_return my_num;

        static constexpr size_t N = sizeof...(Is);
        std::atomic_size_t arg_markers[N];
        JoinHandle<size_t> handles[N] = {
            coro::spawn(
                skynet(my_num + Is*(remaining/N), remaining/N, depth - 1, ArgMarker(arg_markers[Is], depth - 1), std::make_index_sequence<N>{})
            ).submit()
            ...
        };
        auto results = co_await coro::join(std::move(handles[Is])...);
        for (size_t i = 0; i < N; ++i) {
            size_t n = arg_markers[i].load(std::memory_order_acquire);
            CO_ASSERT_EQ(n, 2)
                << "Argument lifetimes not as expected: " << n << " != 2 (" << my_num << ", " << remaining << ", " << i << ") not set after completion";
        }
        sum = ((std::get<Is>(results) + ...));
    }
    co_return sum;
}
TEST(WorkSharingExecutorTest, DISABLED_SkynetSynchronizeSingle) {
    Runtime rt(std::in_place_type<SingleThreadedExecutor>);
    std::atomic_size_t arg_marker{0};
    rt.block_on(skynet(0, 1000000, 6, ArgMarker(arg_marker, 6), std::make_index_sequence<10>{}));
    ASSERT_TRUE(arg_marker.load(std::memory_order_acquire)) << "Expected argument marker not set";
}

TEST(WorkSharingExecutorTest, DISABLED_SkynetSynchronizeShare) {
    Runtime rt(std::in_place_type<WorkSharingExecutor>);
    std::atomic_size_t arg_marker{0};
    rt.block_on(skynet(0, 1000000, 6, ArgMarker(arg_marker, 6), std::make_index_sequence<10>{}));
    ASSERT_TRUE(arg_marker.load(std::memory_order_acquire)) << "Expected argument marker not set";
}

TEST(WorkSharingExecutorTest, DISABLED_SkynetSynchronizeSteal) {
    Runtime rt(std::in_place_type<WorkStealingExecutor>);
    std::atomic_size_t arg_marker{0};
    rt.block_on(skynet(0, 1000000, 6, ArgMarker(arg_marker, 6), std::make_index_sequence<10>{}));
    ASSERT_TRUE(arg_marker.load(std::memory_order_acquire)) << "Expected argument marker not set";
}