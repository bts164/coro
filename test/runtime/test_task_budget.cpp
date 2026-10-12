// Tests for the task budget and yield_now(). See doc/design/task_budget.md.
//
// A task that awaits only ready futures used to keep its thread until one was pending.
// The budget makes it yield after a fixed number of co_awaits, so the tests that say
// "loop" below hang (here: time out) without it on a single-thread runtime.
//
// Also built for Pico (stub suite and board), where AllExecutors is the
// CurrentThreadExecutor alone. The tests under #ifndef CORO_PICO need file
// descriptors, signals or blocking_wait(); the fiber test needs fibers, which the
// stub suite does not have.

#include <gtest/gtest.h>
#include "executor_traits.h"
#include <coro/coro.h>
#include <coro/future.h>
#include <coro/stream.h>
#include <coro/detail/context.h>
#include <coro/detail/rc.h>
#include <coro/detail/waker.h>
#include <coro/runtime/runtime.h>
#include <coro/sync/join.h>
#include <coro/sync/mpsc.h>
#include <coro/sync/select.h>
#include <coro/sync/sleep.h>
#include <coro/task/yield_now.h>

#ifndef CORO_PICO
#include "io_test_util.h"
#include <coro/io/signal.h>
#include <csignal>
#endif

// Desktop and board, not the stub suite.
#if !defined(CORO_PICO) || (defined(PICO_ON_DEVICE) && PICO_ON_DEVICE)
#define CORO_TEST_HAS_FIBERS 1
#include <coro/task/fiber.h>
#endif

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numeric>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

using namespace coro;
using namespace std::chrono_literals;

namespace {

static_assert(Future<YieldNowFuture>);

// Small, so that a loop reaches a yield after a handful of awaits.
constexpr std::uint32_t kBudget = 4;

// How long a loop that should have been interrupted keeps going before the test
// gives up and fails instead of hanging the suite.
constexpr auto kGiveUp = 5s;

#ifdef CORO_TEST_HAS_FIBERS
#ifdef CORO_PICO
// Out of a heap of under 200 KB; the same size test_fiber.cpp uses.
constexpr std::size_t kFiberStackSize = 8 * 1024;
#else
constexpr std::size_t kFiberStackSize = 64 * 1024;
#endif
#endif

// Always ready. Counts its polls when given a counter.
struct ReadyFuture {
    using OutputType = void;
    std::atomic<std::size_t>* m_polls = nullptr;

    PollResult<void> poll(detail::Context&) {
        if (m_polls) m_polls->fetch_add(1, std::memory_order_relaxed);
        return PollReady;
    }
};

// Wraps the root future of a task. At the start of each task poll it records how many
// times ReadyFuture had been polled by then, so a test can see both how many times the
// task was polled and how far it got in each poll.
template<Future F>
struct PollLog {
    using OutputType = typename F::OutputType;
    F                               m_inner;
    const std::atomic<std::size_t>* m_polls;
    std::vector<std::size_t>*       m_starts;

    PollResult<OutputType> poll(detail::Context& ctx) {
        m_starts->push_back(m_polls->load(std::memory_order_relaxed));
        return m_inner.poll(ctx);
    }
};

template<Future F>
PollLog<F> log_polls(F future, const std::atomic<std::size_t>& polls,
                     std::vector<std::size_t>& starts) {
    return PollLog<F>{std::move(future), &polls, &starts};
}

Coro<void> await_ready_n(std::size_t n, std::atomic<std::size_t>& polls) {
    for (std::size_t i = 0; i < n; ++i) co_await ReadyFuture{&polls};
}

// Loops on an always-ready future until `stop` is set. Returns false if it gave up.
Coro<bool> spin_until(const std::atomic<bool>& stop) {
    const auto deadline = std::chrono::steady_clock::now() + kGiveUp;
    while (!stop.load()) {
        if (std::chrono::steady_clock::now() > deadline) co_return false;
        co_await ReadyFuture{};
    }
    co_return true;
}

// The same with yield_now(): the task is never suspended on anything but itself.
Coro<bool> yield_until(const std::atomic<bool>& stop) {
    const auto deadline = std::chrono::steady_clock::now() + kGiveUp;
    while (!stop.load()) {
        if (std::chrono::steady_clock::now() > deadline) co_return false;
        co_await yield_now();
    }
    co_return true;
}

Coro<void> set_flag(std::atomic<bool>& flag) {
    flag.store(true);
    co_return;
}

Coro<void> sleep_then_set(std::chrono::milliseconds delay, std::atomic<bool>& flag) {
    co_await sleep_for(delay);
    flag.store(true);
}

#ifndef CORO_PICO
Coro<void> read_then_set(int fd, std::atomic<bool>& flag) {
    co_await io_test::ReadByteFuture{fd, {}};
    flag.store(true);
}

Coro<void> signal_then_set(SignalFuture sig, std::atomic<bool>& flag) {
    co_await std::move(sig);
    flag.store(true);
}
#endif  // CORO_PICO

// Counts wakes in *m_wakes, which its clones share.
class CountingWaker final : public detail::Waker {
public:
    explicit CountingWaker(int* wakes) : m_wakes(wakes) {}
    void wake() override { ++*m_wakes; }
    detail::Rc<detail::Waker> clone() override {
        return detail::make_rc<CountingWaker>(m_wakes);
    }

private:
    int* m_wakes;
};

// Records what happened to a ProbeFuture.
struct Probe {
    int  polls     = 0;
    bool destroyed = false;
};

struct ProbeFuture {
    using OutputType = void;

    explicit ProbeFuture(Probe* probe) : m_probe(probe) {}
    ProbeFuture(ProbeFuture&& other) noexcept : m_probe(std::exchange(other.m_probe, nullptr)) {}
    ProbeFuture& operator=(ProbeFuture&&) = delete;
    ~ProbeFuture() { if (m_probe) m_probe->destroyed = true; }

    PollResult<void> poll(detail::Context&) {
        ++m_probe->polls;
        return PollReady;
    }

    Probe* m_probe;
};

Coro<void> await_probe(Probe* probe) {
    co_await ProbeFuture(probe);
}

// With a budget of 1, the first await spends it and the second is where the task yields.
Coro<void> ready_then_probe(Probe* probe) {
    co_await ReadyFuture{};
    co_await ProbeFuture(probe);
}

Coro<void> ready_then_probe_coro(Probe* probe) {
    co_await ReadyFuture{};
    co_await await_probe(probe);
}

// Never ready and never wakes: a branch that is waiting on something.
struct WaitingFuture {
    using OutputType = void;
    PollResult<void> poll(detail::Context&) { return PollPending; }
};

Coro<void> await_waiting() {
    co_await WaitingFuture{};
}

// Has no co_await: counts that its body ran.
Coro<void> count_start(int* started) {
    ++*started;
    co_return;
}

} // namespace

// ---------------------------------------------------------------------------
// Context
// ---------------------------------------------------------------------------

TEST(TaskBudgetContext, DefaultIsUnlimited) {
    int wakes = 0;
    detail::Context ctx(detail::make_rc<CountingWaker>(&wakes));
    for (int i = 0; i < 1000; ++i) EXPECT_TRUE(ctx.try_consume_budget());
    EXPECT_FALSE(ctx.yielded());
}

TEST(TaskBudgetContext, ConsumesExactlyTheBudget) {
    int wakes = 0;
    detail::Context ctx(detail::make_rc<CountingWaker>(&wakes), 3);
    EXPECT_TRUE(ctx.try_consume_budget());
    EXPECT_TRUE(ctx.try_consume_budget());
    EXPECT_TRUE(ctx.try_consume_budget());
    EXPECT_FALSE(ctx.try_consume_budget());
    EXPECT_FALSE(ctx.try_consume_budget());
}

TEST(TaskBudgetContext, YieldWakesAndMarksThePoll) {
    int  wakes = 0;
    auto waker = detail::make_rc<CountingWaker>(&wakes);
    detail::Context ctx(waker, 1);
    EXPECT_FALSE(ctx.yielded());
    ctx.yield_to_executor();
    EXPECT_TRUE(ctx.yielded());
    EXPECT_EQ(wakes, 1);
}

TEST(TaskBudgetContext, YieldIfBudgetSpent) {
    int wakes = 0;
    detail::Context ctx(detail::make_rc<CountingWaker>(&wakes), 1);
    // Looks, takes nothing.
    EXPECT_FALSE(ctx.yield_if_budget_spent());
    EXPECT_FALSE(ctx.yield_if_budget_spent());
    EXPECT_FALSE(ctx.yielded());
    EXPECT_TRUE(ctx.try_consume_budget());
    // Spent: yields, and only once however often it is asked.
    EXPECT_TRUE(ctx.yield_if_budget_spent());
    EXPECT_TRUE(ctx.yield_if_budget_spent());
    EXPECT_TRUE(ctx.yielded());
    EXPECT_EQ(wakes, 1);
}

// ---------------------------------------------------------------------------
// Driven by hand: what a coroutine does when the budget runs out
// ---------------------------------------------------------------------------

TEST(TaskBudgetCoro, FutureIsNotPolledWhenBudgetIsSpent) {
    int   wakes = 0;
    auto  waker = detail::make_rc<CountingWaker>(&wakes);
    Probe probe;
    auto  coro = ready_then_probe(&probe);

    detail::Context first(waker, 1);
    EXPECT_TRUE(coro.poll(first).isPending());
    EXPECT_TRUE(first.yielded());
    EXPECT_EQ(wakes, 1);
    EXPECT_EQ(probe.polls, 0);

    // The next poll's re-poll before resume is the future's first poll.
    detail::Context second(waker, 1);
    EXPECT_TRUE(coro.poll(second).isReady());
    EXPECT_FALSE(second.yielded());
    EXPECT_EQ(probe.polls, 1);
}

// That first poll takes a unit like any other: with two deferred awaits under a
// join() and a budget of 1, the next poll makes one of them and defers the other again.
TEST(TaskBudgetCoro, DeferredFirstPollIsCharged) {
    int   wakes = 0;
    auto  waker = detail::make_rc<CountingWaker>(&wakes);
    Probe probe;
    std::atomic<std::size_t> polls{0};
    auto coro = [](std::atomic<std::size_t>& polls, Probe* probe) -> Coro<void> {
        co_await join(await_ready_n(3, polls), await_probe(probe));
    }(polls, &probe);

    // The co_await of the join() itself takes the only unit, so both branches are
    // deferred at their first await.
    detail::Context first(waker, 1);
    EXPECT_TRUE(coro.poll(first).isPending());
    EXPECT_TRUE(first.yielded());
    EXPECT_EQ(polls.load(), 0u);
    EXPECT_EQ(probe.polls, 0);

    // One unit, two deferred awaits: exactly one of them is polled.
    detail::Context second(waker, 1);
    EXPECT_TRUE(coro.poll(second).isPending());
    EXPECT_TRUE(second.yielded());
    EXPECT_EQ(polls.load() + static_cast<std::size_t>(probe.polls), 1u);

    // Without a limit everything left runs.
    detail::Context third(waker);
    EXPECT_TRUE(coro.poll(third).isReady());
    EXPECT_EQ(polls.load(), 3u);
    EXPECT_EQ(probe.polls, 1);
}

// The co_await of the join() takes one unit and the first branch's first await the
// other. The second branch has not started, and must not be started just to find at
// its first co_await that it cannot go on.
TEST(TaskBudgetCoro, JoinDoesNotStartBranchWhenBudgetIsSpent) {
    int  wakes   = 0;
    int  started = 0;
    auto waker   = detail::make_rc<CountingWaker>(&wakes);
    std::atomic<std::size_t> polls{0};
    auto coro = [](std::atomic<std::size_t>& polls, int* started) -> Coro<void> {
        co_await join(await_ready_n(3, polls), count_start(started));
    }(polls, &started);

    detail::Context first(waker, 2);
    EXPECT_TRUE(coro.poll(first).isPending());
    EXPECT_TRUE(first.yielded());
    EXPECT_EQ(polls.load(), 1u);
    EXPECT_EQ(started, 0);

    detail::Context second(waker);
    EXPECT_TRUE(coro.poll(second).isReady());
    EXPECT_EQ(polls.load(), 3u);
    EXPECT_EQ(started, 1);
}

TEST(TaskBudgetCoro, SelectDoesNotStartBranchWhenBudgetIsSpent) {
    int  wakes   = 0;
    int  started = 0;
    auto waker   = detail::make_rc<CountingWaker>(&wakes);
    std::atomic<std::size_t> polls{0};
    auto coro = [](std::atomic<std::size_t>& polls, int* started) -> Coro<std::size_t> {
        auto won = co_await select(await_ready_n(3, polls), count_start(started));
        co_return won.index();
    }(polls, &started);

    detail::Context first(waker, 2);
    EXPECT_TRUE(coro.poll(first).isPending());
    EXPECT_TRUE(first.yielded());
    EXPECT_EQ(polls.load(), 1u);
    EXPECT_EQ(started, 0);

    // select() starts one branch later on each poll, so the second branch goes
    // first now and wins. The first branch is cancelled and drained in the same poll.
    detail::Context second(waker);
    auto result = coro.poll(second);
    ASSERT_TRUE(result.isReady());
    EXPECT_EQ(std::move(result).value(), 1u);
    EXPECT_EQ(started, 1);
}

// The first branch takes the last unit and then waits on something, so nothing has
// yielded when join() comes to the second branch. Skipping it must make the yield:
// this poll may have been for that branch, and nothing else would bring the task back.
TEST(TaskBudgetCoro, SkippedBranchMakesTheTaskYield) {
    int  wakes   = 0;
    int  started = 0;
    auto waker   = detail::make_rc<CountingWaker>(&wakes);
    auto coro = [](int* started) -> Coro<void> {
        co_await join(await_waiting(), count_start(started));
    }(&started);

    detail::Context first(waker, 2);
    EXPECT_TRUE(coro.poll(first).isPending());
    EXPECT_EQ(started, 0);
    EXPECT_TRUE(first.yielded());
    EXPECT_EQ(wakes, 1);

    // With budget left for it the second branch runs, and nothing yields: the task
    // now waits for the first branch alone.
    detail::Context second(waker, 2);
    EXPECT_TRUE(coro.poll(second).isPending());
    EXPECT_EQ(started, 1);
    EXPECT_FALSE(second.yielded());
    EXPECT_EQ(wakes, 1);
}

TEST(TaskBudgetCoro, DropWhileSuspendedByBudget) {
    int   wakes = 0;
    auto  waker = detail::make_rc<CountingWaker>(&wakes);
    Probe probe;
    {
        auto coro = ready_then_probe(&probe);
        detail::Context ctx(waker, 1);
        EXPECT_TRUE(coro.poll(ctx).isPending());
        EXPECT_FALSE(probe.destroyed);
    }
    EXPECT_TRUE(probe.destroyed);
    EXPECT_EQ(probe.polls, 0);
}

TEST(TaskBudgetCoro, CancelWhileSuspendedByBudget) {
    int   wakes = 0;
    auto  waker = detail::make_rc<CountingWaker>(&wakes);
    Probe probe;
    auto  coro = ready_then_probe(&probe);

    detail::Context first(waker, 1);
    EXPECT_TRUE(coro.poll(first).isPending());

    coro.cancel();
    detail::Context second(waker, 1);
    EXPECT_TRUE(coro.poll(second).isDropped());
    EXPECT_TRUE(probe.destroyed);
    EXPECT_EQ(probe.polls, 0);
}

// The awaited future is a Coro, which is cancelled and drained, not just destroyed.
// It has not started, so its body never runs.
TEST(TaskBudgetCoro, CancelWhileSuspendedByBudgetOnCoro) {
    int   wakes = 0;
    auto  waker = detail::make_rc<CountingWaker>(&wakes);
    Probe probe;
    auto  coro = ready_then_probe_coro(&probe);

    detail::Context first(waker, 1);
    EXPECT_TRUE(coro.poll(first).isPending());

    coro.cancel();
    detail::Context second(waker, 1);
    EXPECT_TRUE(coro.poll(second).isDropped());
    EXPECT_EQ(probe.polls, 0);
}

#ifndef CORO_PICO  // no blocking_wait() on Pico

TEST(TaskBudgetCoro, BlockingWaitHasNoBudget) {
    std::atomic<std::size_t> polls{0};
    std::vector<std::size_t> starts;
    blocking_wait(log_polls(await_ready_n(1000, polls), polls, starts));
    EXPECT_EQ(polls.load(), 1000u);
    EXPECT_EQ(starts, (std::vector<std::size_t>{0}));
}

TEST(TaskBudgetCoro, YieldNowInBlockingWait) {
    std::atomic<std::size_t> polls{0};
    std::vector<std::size_t> starts;
    blocking_wait(log_polls(yield_now(), polls, starts));
    EXPECT_EQ(starts.size(), 2u);
}

#endif  // CORO_PICO

// ---------------------------------------------------------------------------
// On every executor
// ---------------------------------------------------------------------------

template<typename Traits>
class TaskBudgetTest : public testing::Test {
protected:
    TaskBudgetTest() { traits.rt.set_task_budget(kBudget); }
    Traits traits;
};
TYPED_TEST_SUITE(TaskBudgetTest, AllExecutors);

// Poll 1 passes kBudget awaits and yields at the next one. Poll 2 completes that
// await, which is charged like any other, and passes kBudget - 1 more.
TYPED_TEST(TaskBudgetTest, YieldsAfterBudgetAwaits) {
    std::atomic<std::size_t> polls{0};
    std::vector<std::size_t> starts;
    this->traits.rt.block_on(log_polls(await_ready_n(10, polls), polls, starts));
    EXPECT_EQ(polls.load(), 10u);
    EXPECT_EQ(starts, (std::vector<std::size_t>{0, 4, 8}));
}

TYPED_TEST(TaskBudgetTest, ZeroBudgetMeansUnlimited) {
    this->traits.rt.set_task_budget(0);
    std::atomic<std::size_t> polls{0};
    std::vector<std::size_t> starts;
    this->traits.rt.block_on(log_polls(await_ready_n(1000, polls), polls, starts));
    EXPECT_EQ(polls.load(), 1000u);
    EXPECT_EQ(starts, (std::vector<std::size_t>{0}));
}

// The outer co_await of each inner coroutine takes one unit and its three awaits take
// three more, so the second inner coroutine is where the task yields.
TYPED_TEST(TaskBudgetTest, NestedCoroutinesShareOneBudget) {
    std::atomic<std::size_t> polls{0};
    std::vector<std::size_t> starts;
    this->traits.rt.block_on(log_polls(
        [](std::atomic<std::size_t>& polls) -> Coro<void> {
            co_await await_ready_n(3, polls);
            co_await await_ready_n(3, polls);
        }(polls), polls, starts));
    EXPECT_EQ(polls.load(), 6u);
    EXPECT_EQ(starts, (std::vector<std::size_t>{0, 3}));
}

// A tree of join()s over coroutines that never wait: 3 children per node, 4 levels,
// 40 inner nodes. Each task poll may start at most kBudget awaits, however many
// coroutines the earlier polls left suspended. Before deferred awaits were charged,
// every suspended coroutine started its children for free on the next poll and the
// whole tree was live after a few polls.
Coro<std::size_t> ready_tree(std::size_t depth, std::atomic<std::size_t>& awaits) {
    if (depth == 0) co_return 1;
    struct Counted {
        using Join = JoinFuture<Coro<std::size_t>, Coro<std::size_t>, Coro<std::size_t>>;
        using OutputType = Join::OutputType;
        Join                      m_join;
        std::atomic<std::size_t>* m_awaits;
        bool                      m_counted = false;
        PollResult<OutputType> poll(detail::Context& ctx) {
            // Counted at its first poll, which is what the budget charges for.
            if (!std::exchange(m_counted, true))
                m_awaits->fetch_add(1, std::memory_order_relaxed);
            return m_join.poll(ctx);
        }
    };
    auto [a, b, c] = co_await Counted{
        join(ready_tree(depth - 1, awaits), ready_tree(depth - 1, awaits),
             ready_tree(depth - 1, awaits)),
        &awaits};
    co_return a + b + c;
}

TYPED_TEST(TaskBudgetTest, NestedJoinStartsAtMostBudgetAwaitsPerPoll) {
    std::atomic<std::size_t> awaits{0};
    std::vector<std::size_t> starts;
    const std::size_t leaves =
        this->traits.rt.block_on(log_polls(ready_tree(4, awaits), awaits, starts));
    EXPECT_EQ(leaves, 81u);
    EXPECT_EQ(awaits.load(), 40u);
    starts.push_back(awaits.load());
    for (std::size_t i = 1; i < starts.size(); ++i)
        EXPECT_LE(starts[i] - starts[i - 1], kBudget) << "task poll " << i;
}

TYPED_TEST(TaskBudgetTest, JoinedCoroutinesYieldTogether) {
    std::atomic<std::size_t> polls{0};
    std::vector<std::size_t> starts;
    this->traits.rt.block_on(log_polls(
        [](std::atomic<std::size_t>& polls) -> Coro<void> {
            co_await join(await_ready_n(6, polls), await_ready_n(6, polls));
        }(polls), polls, starts));
    EXPECT_EQ(polls.load(), 12u);
    EXPECT_GE(starts.size(), 2u);
}

// One branch spends the budget in every poll; the other is resumed from a pending
// await in those same polls and must still be re-polled. Its next sleep_for() may
// then be deferred, and gets its first poll in a turn where join() polls it first.
TYPED_TEST(TaskBudgetTest, PendingAwaitResumesAfterBudgetSpent) {
    std::atomic<bool> stop{false};
    const bool seen = this->traits.rt.block_on(
        [](std::atomic<bool>& stop) -> Coro<bool> {
            auto [seen, slept] = co_await join(
                spin_until(stop),
                [](std::atomic<bool>& stop) -> Coro<int> {
                    for (int i = 0; i < 3; ++i) co_await sleep_for(1ms);
                    stop.store(true);
                    co_return 3;
                }(stop));
            co_return seen && slept == 3;
        }(stop));
    EXPECT_TRUE(seen);
}

TYPED_TEST(TaskBudgetTest, AlwaysReadyLoopLetsPeerTaskRun) {
    std::atomic<bool> stop{false};
    const bool seen = this->traits.rt.block_on(
        [](std::atomic<bool>& stop) -> Coro<bool> {
            auto peer = coro::spawn(set_flag(stop));
            const bool seen = co_await spin_until(stop);
            if (seen) co_await std::move(peer);
            co_return seen;
        }(stop));
    EXPECT_TRUE(seen);
}

TYPED_TEST(TaskBudgetTest, AlwaysReadyLoopLetsTimerFire) {
    std::atomic<bool> stop{false};
    const bool seen = this->traits.rt.block_on(
        [](std::atomic<bool>& stop) -> Coro<bool> {
            auto peer = coro::spawn(sleep_then_set(2ms, stop));
            const bool seen = co_await spin_until(stop);
            if (seen) co_await std::move(peer);
            co_return seen;
        }(stop));
    EXPECT_TRUE(seen);
}

#ifndef CORO_PICO  // file descriptors and signals

TYPED_TEST(TaskBudgetTest, AlwaysReadyLoopLetsIoEventThrough) {
    io_test::SocketPair sp;
    std::atomic<bool>   stop{false};
    const bool seen = this->traits.rt.block_on(
        [](int read_fd, int write_fd, std::atomic<bool>& stop) -> Coro<bool> {
            auto peer = coro::spawn(read_then_set(read_fd, stop));
            // Long enough for the peer to be polled and find nothing to read yet.
            for (int i = 0; i < 64; ++i) co_await ReadyFuture{};
            io_test::write_byte(write_fd);
            const bool seen = co_await spin_until(stop);
            if (seen) co_await std::move(peer);
            co_return seen;
        }(sp.a, sp.b, stop));
    EXPECT_TRUE(seen);
}

TYPED_TEST(TaskBudgetTest, AlwaysReadyLoopLetsSignalThrough) {
    std::atomic<bool> stop{false};
    const bool seen = this->traits.rt.block_on(
        [](std::atomic<bool>& stop) -> Coro<bool> {
            // Watching starts here, so the raise below cannot be missed.
            auto peer = coro::spawn(signal_then_set(coro::signal(SIGUSR1), stop));
            for (int i = 0; i < 64; ++i) co_await ReadyFuture{};
            ::raise(SIGUSR1);
            const bool seen = co_await spin_until(stop);
            if (seen) co_await std::move(peer);
            co_return seen;
        }(stop));
    EXPECT_TRUE(seen);
}

#endif  // CORO_PICO

// The receiving loop yields many times; nothing is lost or seen twice across a yield.
TYPED_TEST(TaskBudgetTest, ValueDeliveredOnceAcrossYield) {
    constexpr int    kCount = 200;
    std::vector<int> received;
    this->traits.rt.block_on(
        [](std::vector<int>& received) -> Coro<void> {
            auto [tx, rx] = mpsc_channel<int>(64);
            auto producer = coro::spawn([](MpscSender<int> tx) -> Coro<void> {
                for (int i = 0; i < kCount; ++i) co_await tx.send(i);
            }(std::move(tx)));
            while (auto v = co_await next(rx)) received.push_back(*v);
            co_await std::move(producer);
        }(received));
    std::vector<int> expected(kCount);
    std::iota(expected.begin(), expected.end(), 0);
    EXPECT_EQ(received, expected);
}

TYPED_TEST(TaskBudgetTest, YieldNowLoopLetsPeerTaskRun) {
    std::atomic<bool> stop{false};
    const bool seen = this->traits.rt.block_on(
        [](std::atomic<bool>& stop) -> Coro<bool> {
            auto peer = coro::spawn(set_flag(stop));
            const bool seen = co_await yield_until(stop);
            if (seen) co_await std::move(peer);
            co_return seen;
        }(stop));
    EXPECT_TRUE(seen);
}

// The only runnable task keeps yielding; the timer it waits for needs the driver.
TYPED_TEST(TaskBudgetTest, YieldNowTurnsDriver) {
    std::atomic<bool> stop{false};
    const bool seen = this->traits.rt.block_on(
        [](std::atomic<bool>& stop) -> Coro<bool> {
            auto peer = coro::spawn(sleep_then_set(2ms, stop));
            const bool seen = co_await yield_until(stop);
            if (seen) co_await std::move(peer);
            co_return seen;
        }(stop));
    EXPECT_TRUE(seen);
}

// ---------------------------------------------------------------------------
// Single-thread runtime only
// ---------------------------------------------------------------------------

// Each yield puts the task behind the other one, so they alternate strictly.
TEST(TaskBudgetSingleThread, YieldNowAlternatesTasks) {
    SingleThreadRuntime rt;
    std::string         order;
    rt.block_on([](std::string& order) -> Coro<void> {
        auto worker = [](std::string& order, char id) -> Coro<void> {
            for (int i = 0; i < 3; ++i) {
                order.push_back(id);
                co_await yield_now();
            }
        };
        auto a = coro::spawn(worker(order, 'A'));
        auto b = coro::spawn(worker(order, 'B'));
        co_await std::move(a);
        co_await std::move(b);
    }(order));
    EXPECT_EQ(order, "ABABAB");
}

#ifdef CORO_TEST_HAS_FIBERS

TEST(TaskBudgetSingleThread, FiberAwaitYieldsWhenBudgetSpent) {
    SingleThreadRuntime rt;
    rt.set_task_budget(kBudget);
    std::atomic<bool> stop{false};
    std::atomic<bool> seen{false};
    rt.block_on([](std::atomic<bool>* stop, std::atomic<bool>* seen) -> Coro<void> {
        auto fiber = coro::spawn_fiber<void>([stop, seen] {
            const auto deadline = std::chrono::steady_clock::now() + kGiveUp;
            while (!stop->load() && std::chrono::steady_clock::now() < deadline)
                coro::fiber_await(ReadyFuture{});
            seen->store(stop->load());
        }, kFiberStackSize);
        auto peer = coro::spawn(set_flag(*stop));
        co_await std::move(fiber);
        co_await std::move(peer);
    }(&stop, &seen));
    EXPECT_TRUE(seen.load());
}

#endif  // CORO_TEST_HAS_FIBERS
