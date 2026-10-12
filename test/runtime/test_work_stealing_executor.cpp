#include <gtest/gtest.h>
#include <coro/runtime/runtime.h>
#include <coro/runtime/current_thread_executor.h>
#include <coro/runtime/work_sharing_executor.h>
#include <coro/runtime/work_stealing_executor.h>
#include <coro/coro.h>
#include <coro/task/join_handle.h>
#include <coro/task/join_set.h>
#include <coro/task/spawn_blocking.h>
#include <coro/task/yield_now.h>
#include <coro/co_invoke.h>
#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <set>
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

TEST(WorkStealingExecutorTest, ClampsMoreThanMaxWorkers) {
    Runtime rt(1);
    // 65 workers would overflow the 64-bit idle mask; the constructor clamps to MAX_WORKERS.
    WorkStealingExecutor exec(&rt, WorkStealingExecutor::MAX_WORKERS + 1);
    (void)exec;
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

// ---------------------------------------------------------------------------
// Periodic injection-queue check: tasks waiting in the injection queue run
// although no worker's local queue ever empties.
// See doc/design/work_stealing_executor.md, "Periodic injection-queue check".
// ---------------------------------------------------------------------------

namespace {

using namespace std::chrono_literals;

constexpr int  kBusyWorkers = 2;
constexpr int  kSpinners    = 16;
// How long a test waits for something that should take microseconds. A failure
// shows up as this timeout, not as a hang.
constexpr auto kGiveUp      = 10s;

// The worker threads that have polled a spinner.
class WorkerSet {
public:
    void note() {
        if (m_full.load(std::memory_order_acquire)) return;
        std::lock_guard lock(m_mutex);
        m_ids.insert(std::this_thread::get_id());
        if (static_cast<int>(m_ids.size()) >= kBusyWorkers)
            m_full.store(true, std::memory_order_release);
    }
    bool full() const { return m_full.load(std::memory_order_acquire); }

private:
    std::mutex                m_mutex;
    std::set<std::thread::id> m_ids;
    std::atomic<bool>         m_full{false};
};

// Wakes itself on every poll until `*m_stop`, so the local queue of the worker
// that runs it is never empty.
struct SpinUntilStopped {
    using OutputType = void;
    std::atomic<bool>* m_stop;
    WorkerSet*         m_workers;

    PollResult<void> poll(detail::Context& ctx) {
        m_workers->note();
        if (m_stop->load(std::memory_order_acquire)) return PollReady;
        ctx.getWaker()->wake();
        return PollPending;
    }
};

// Starts the spinners and waits until every worker is running some of them. From
// then on no worker reaches the injection queue by running out of local work.
Coro<bool> make_every_worker_busy(std::atomic<bool>& stop, WorkerSet& workers) {
    for (int i = 0; i < kSpinners; ++i)
        coro::spawn(SpinUntilStopped{&stop, &workers}).detach();
    const auto deadline = std::chrono::steady_clock::now() + kGiveUp;
    while (!workers.full() && std::chrono::steady_clock::now() < deadline)
        co_await coro::yield_now();
    co_return workers.full();
}

// Hands its waker to `*m_promise` on the first poll and completes on the second.
struct WakeFromOutsideFuture {
    using OutputType = int;
    int                                      m_value;
    std::promise<detail::Rc<detail::Waker>>* m_promise;
    bool                                     m_first = true;

    PollResult<int> poll(detail::Context& ctx) {
        if (m_first) {
            m_first = false;
            m_promise->set_value(ctx.getWaker()->clone());
            return PollPending;
        }
        return m_value;
    }
};

Coro<void> fulfil_promise(std::shared_ptr<std::promise<void>> ran) {
    ran->set_value();
    co_return;
}

Coro<bool> spawn_from_blocking_thread(std::atomic<bool>& stop, WorkerSet& workers) {
    if (!co_await make_every_worker_busy(stop, workers)) {
        stop.store(true, std::memory_order_release);
        co_return false;
    }
    // The spawn below comes from a blocking-pool thread, so the task goes onto the
    // injection queue. So does this coroutine when the callable's result wakes it;
    // the callable stops the spinners first so that a failure cannot hang here.
    co_return co_await coro::spawn_blocking([&stop] {
        auto ran  = std::make_shared<std::promise<void>>();
        auto done = ran->get_future();
        coro::spawn(fulfil_promise(ran)).detach();
        const bool ok = done.wait_for(kGiveUp) == std::future_status::ready;
        stop.store(true, std::memory_order_release);
        return ok;
    });
}

Coro<void> count_one(std::atomic<int>& counter) {
    counter.fetch_add(1, std::memory_order_relaxed);
    co_return;
}

// Spawns more tasks at once than a local queue holds, so that some spill to the
// injection queue, and returns how many had run by the time it gave up waiting.
Coro<int> overflow_while_busy(std::atomic<bool>& stop, WorkerSet& workers,
                              std::atomic<int>& counter, int n) {
    if (!co_await make_every_worker_busy(stop, workers)) {
        stop.store(true, std::memory_order_release);
        co_return -1;
    }
    for (int i = 0; i < n; ++i)
        coro::spawn(count_one(counter)).detach();
    const auto deadline = std::chrono::steady_clock::now() + kGiveUp;
    while (counter.load(std::memory_order_relaxed) < n &&
           std::chrono::steady_clock::now() < deadline)
        co_await coro::yield_now();
    const int ran = counter.load(std::memory_order_relaxed);
    stop.store(true, std::memory_order_release);
    co_return ran;
}

} // namespace

TEST(WorkStealingExecutorTest, InjectedTaskRunsWhileEveryWorkerIsBusy) {
    Runtime rt(kBusyWorkers);
    std::atomic<bool> stop{false};
    WorkerSet workers;
    ASSERT_TRUE(rt.block_on(make_every_worker_busy(stop, workers)));

    // block_on() from this thread schedules through the injection queue, and the
    // wake below, from a thread outside the executor, goes through it again.
    std::promise<detail::Rc<detail::Waker>> waker_promise;
    auto waker_future = waker_promise.get_future();
    std::atomic<bool> finished{false};
    std::atomic<bool> timed_out{false};
    std::thread outside([&] {
        // On a timeout the spinners are stopped, so that the workers reach the
        // injection queue the old way and the test fails without hanging.
        auto give_up = [&] {
            timed_out.store(true, std::memory_order_release);
            stop.store(true, std::memory_order_release);
        };
        if (waker_future.wait_for(kGiveUp) != std::future_status::ready) give_up();
        waker_future.get()->wake();
        const auto deadline = std::chrono::steady_clock::now() + kGiveUp;
        while (!finished.load(std::memory_order_acquire)) {
            if (std::chrono::steady_clock::now() >= deadline) {
                give_up();
                break;
            }
            std::this_thread::sleep_for(1ms);
        }
    });

    const int value = rt.block_on(WakeFromOutsideFuture{5, &waker_promise});
    finished.store(true, std::memory_order_release);
    outside.join();
    stop.store(true, std::memory_order_release);

    EXPECT_EQ(value, 5);
    EXPECT_FALSE(timed_out.load());
}

TEST(WorkStealingExecutorTest, SpawnFromOutsideRunsWhileEveryWorkerIsBusy) {
    Runtime rt(kBusyWorkers);
    std::atomic<bool> stop{false};
    WorkerSet workers;
    EXPECT_TRUE(rt.block_on(spawn_from_blocking_thread(stop, workers)));
    stop.store(true, std::memory_order_release);
}

TEST(WorkStealingExecutorTest, OverflowedTasksRunWhileEveryWorkerIsBusy) {
    constexpr int kTasks = 1000;   // several times a local queue's capacity
    Runtime rt(kBusyWorkers);
    std::atomic<bool> stop{false};
    WorkerSet workers;
    std::atomic<int> counter{0};
    EXPECT_EQ(rt.block_on(overflow_while_busy(stop, workers, counter, kTasks)), kTasks);
    stop.store(true, std::memory_order_release);
}
