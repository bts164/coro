#include <gtest/gtest.h>
#include <coro/runtime/runtime.h>
#include <coro/runtime/current_thread_executor.h>
#include <coro/runtime/work_sharing_executor.h>
#include <coro/runtime/work_stealing_executor.h>
#include <coro/coro.h>
#include <coro/detail/wake_batch.h>
#include <coro/sync/event.h>
#include <coro/task/join_handle.h>
#include <coro/task/join_set.h>
#include <coro/task/spawn_blocking.h>
#include <coro/task/yield_now.h>
#include <coro/co_invoke.h>
#include <coro/stream.h>
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
            total += result->value();
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
            result->value();  // rethrows the child's exception
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

// ---------------------------------------------------------------------------
// A burst of spawns reaches every worker, although the spawns themselves wake
// only one. See doc/design/work_stealing_executor.md, "Passing the wake on".
// ---------------------------------------------------------------------------

namespace {

constexpr int kChainWorkers = 4;
constexpr int kChainTasks   = 8;

// Holds its worker, with no co_await, until kChainWorkers of these have started.
// None of them finishes before that, so the first kChainWorkers to start are
// running at the same moment, each on a worker of its own.
Coro<bool> hold_worker(std::atomic<int>& started) {
    started.fetch_add(1, std::memory_order_acq_rel);
    const auto deadline = std::chrono::steady_clock::now() + kGiveUp;
    while (started.load(std::memory_order_acquire) < kChainWorkers) {
        if (std::chrono::steady_clock::now() >= deadline) co_return false;
        std::this_thread::yield();
    }
    co_return true;
}

Coro<bool> spawn_burst(std::atomic<int>& started) {
    JoinSet<bool> js;
    for (int i = 0; i < kChainTasks; ++i)
        js.spawn(hold_worker(started));
    bool all = true;
    while (auto held = co_await next(js))
        all = held->value() && all;
    co_return all;
}

} // namespace

// The spawns all come from one worker within a few microseconds, sooner than a
// parked worker can wake, so every one of them picks the same worker to wake. That
// worker steals half the tasks and runs one. Nothing is enqueued after that: the
// tasks never suspend. The other two workers start only if a worker that finds work
// by stealing wakes the next one.
TEST(WorkStealingExecutorTest, BurstOfSpawnsReachesEveryWorker) {
    Runtime rt(kChainWorkers);
    std::atomic<int> started{0};
    EXPECT_TRUE(rt.block_on(spawn_burst(started)));
}

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

// ---------------------------------------------------------------------------
// WakeBatch. See doc/design/work_stealing_executor.md, "Batched wakes (`WakeBatch`)".
// ---------------------------------------------------------------------------

namespace {

// Spawns kChainWorkers tasks inside one batch; each holds its worker until all
// have started, so they must run on kChainWorkers workers at once.
Coro<bool> spawn_batch_of_holders(std::atomic<int>& started) {
    JoinSet<bool> js;
    {
        detail::WakeBatch batch;
        for (int i = 0; i < kChainWorkers; ++i)
            js.spawn(hold_worker(started));
    }
    bool all = true;
    while (auto held = co_await next(js))
        all = held->value() && all;
    co_return all;
}

Coro<int> spawn_in_batch(std::atomic<int>& counter, int n) {
    JoinSet<void> js;
    {
        detail::WakeBatch batch;
        for (int i = 0; i < n; ++i)
            js.spawn(count_one(counter));
    }
    co_await js.drain();
    co_return counter.load();
}

Coro<bool> nested_batches(std::atomic<int>& counter) {
    bool open_states_ok = !detail::WakeBatch::is_open();
    JoinSet<void> js;
    {
        detail::WakeBatch outer;
        js.spawn(count_one(counter));
        {
            detail::WakeBatch inner;
            js.spawn(count_one(counter));
            open_states_ok = open_states_ok && detail::WakeBatch::is_open();
        }
        // The outer batch is open again.
        open_states_ok = open_states_ok && detail::WakeBatch::is_open();
        js.spawn(count_one(counter));
    }
    open_states_ok = open_states_ok && !detail::WakeBatch::is_open();
    co_await js.drain();
    co_return open_states_ok && counter.load() == 3;
}

Coro<int> batch_closed_by_exception(std::atomic<int>& counter) {
    JoinSet<void> js;
    try {
        detail::WakeBatch batch;
        js.spawn(count_one(counter));
        js.spawn(count_one(counter));
        throw std::runtime_error("unwinds through the batch");
    } catch (const std::runtime_error&) {}
    co_await js.drain();
    co_return counter.load();
}

} // namespace

TEST(WorkStealingExecutorTest, BatchedSpawnsStartOnEveryWorker) {
    Runtime rt(kChainWorkers);
    std::atomic<int> started{0};
    EXPECT_TRUE(rt.block_on(spawn_batch_of_holders(started)));
}

TEST(WorkStealingExecutorTest, BatchLargerThanInlineCapacityRunsEveryTask) {
    constexpr int kTasks = 200;  // more than WakeBatch holds without allocating
    Runtime rt(4);
    std::atomic<int> counter{0};
    EXPECT_EQ(rt.block_on(spawn_in_batch(counter, kTasks)), kTasks);
}

TEST(WorkStealingExecutorTest, NestedBatchesRestoreTheOuterOne) {
    Runtime rt(2);
    std::atomic<int> counter{0};
    EXPECT_TRUE(rt.block_on(nested_batches(counter)));
}

TEST(WorkStealingExecutorTest, BatchClosedByExceptionDeliversItsTasks) {
    Runtime rt(2);
    std::atomic<int> counter{0};
    EXPECT_EQ(rt.block_on(batch_closed_by_exception(counter)), 2);
}

TEST(WorkStealingExecutorTest, BatchOnThreadOutsideExecutorDeliversWake) {
    Runtime rt(2);
    std::promise<detail::Rc<detail::Waker>> waker_promise;
    auto waker_future = waker_promise.get_future();
    std::thread outside([&] {
        if (waker_future.wait_for(kGiveUp) != std::future_status::ready) return;
        auto waker = waker_future.get();
        detail::WakeBatch batch;
        waker->wake();
    });
    const int value = rt.block_on(WakeFromOutsideFuture{9, &waker_promise});
    outside.join();
    EXPECT_EQ(value, 9);
}

TEST(WorkStealingExecutorTest, BatchIsNoOpOnOtherExecutors) {
    std::atomic<int> counter{0};
    {
        Runtime rt(1);
        EXPECT_EQ(rt.block_on(spawn_in_batch(counter, 100)), 100);
    }
    counter = 0;
    {
        Runtime rt(std::in_place_type<WorkSharingExecutor>, std::size_t{2});
        EXPECT_EQ(rt.block_on(spawn_in_batch(counter, 100)), 100);
    }
}

// ---------------------------------------------------------------------------
// LIFO slot. See doc/design/work_stealing_executor.md, "LIFO slot".
// ---------------------------------------------------------------------------

namespace {

// Holds its worker, with no co_await, until `stop` (or kGiveUp).
Coro<void> hold_until_stopped(std::atomic<bool>& started, std::atomic<bool>& stop) {
    started.store(true, std::memory_order_release);
    const auto deadline = std::chrono::steady_clock::now() + kGiveUp;
    while (!stop.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    co_return;
}

// The partner of ping_pong(): answers each ping until `done`.
Coro<void> pong(Event& ping, Event& pong_ev, std::atomic<bool>& done) {
    for (;;) {
        co_await ping.wait();
        ping.clear();
        if (done.load(std::memory_order_acquire)) co_return;
        pong_ev.set();
    }
}

// Exchanges wakes with a partner task until `until()` or kGiveUp, and returns the
// number of round trips. Each wake comes from a worker, so it goes to the LIFO slot.
template<typename Until>
Coro<int> ping_pong(Until until) {
    Event ping, pong_ev;
    std::atomic<bool> done{false};
    auto partner = coro::spawn(pong(ping, pong_ev, done));
    int rounds = 0;
    const auto deadline = std::chrono::steady_clock::now() + kGiveUp;
    while (!until(rounds) && std::chrono::steady_clock::now() < deadline) {
        ping.set();
        co_await pong_ev.wait();
        pong_ev.clear();
        ++rounds;
    }
    done.store(true, std::memory_order_release);
    ping.set();
    co_await std::move(partner);
    co_return rounds;
}

struct RoundsReached {
    int n;
    bool operator()(int rounds) const { return rounds >= n; }
};

struct CounterReached {
    std::atomic<int>* counter;
    int               n;
    bool operator()(int) const { return counter->load(std::memory_order_relaxed) >= n; }
};

// Holds every worker but one, then ping-pongs on that one with tasks queued behind
// the pair. The queued tasks run only if the slot gives way after kMaxLifoPolls.
Coro<int> ping_pong_beside_queued_tasks(std::atomic<int>& counter, int n) {
    std::atomic<bool> holder_started{false}, stop{false};
    auto holder = coro::spawn(hold_until_stopped(holder_started, stop));
    const auto deadline = std::chrono::steady_clock::now() + kGiveUp;
    while (!holder_started.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline)
        co_await coro::yield_now();
    // From here this task and everything it spawns run on the one free worker.
    for (int i = 0; i < n; ++i)
        coro::spawn(count_one(counter)).detach();
    co_await ping_pong(CounterReached{&counter, n});
    stop.store(true, std::memory_order_release);
    co_await std::move(holder);
    co_return counter.load();
}

} // namespace

TEST(WorkStealingExecutorTest, PingPongThroughLifoSlotCompletes) {
    constexpr int kRounds = 10000;
    Runtime rt(4);
    EXPECT_EQ(rt.block_on(ping_pong(RoundsReached{kRounds})), kRounds);
}

TEST(WorkStealingExecutorTest, LifoPingPongDoesNotStarveQueuedTasks) {
    constexpr int kTasks = 20;
    Runtime rt(2);
    std::atomic<int> counter{0};
    EXPECT_EQ(rt.block_on(ping_pong_beside_queued_tasks(counter, kTasks)), kTasks);
}
