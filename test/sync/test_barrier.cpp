#include <gtest/gtest.h>
#include "executor_traits.h"
#include <coro/sync/barrier.h>
#include <coro/sync/select.h>
#include <coro/task/join_set.h>
#include <coro/runtime/runtime.h>
#include <coro/coro.h>
#include <array>
#include <atomic>
#include <cstddef>
#include <vector>

using namespace coro;

static_assert(Future<BarrierWaitFuture>);

namespace {

// Counts wake() calls. Used to poll futures by hand, without a runtime.
class CountingWaker : public detail::Waker {
public:
    void wake() override { ++wakes; }
    detail::Rc<detail::Waker> clone() override { return detail::make_rc<CountingWaker>(); }
    int wakes = 0;
};

struct ReadyVoidFuture {
    using OutputType = void;
    PollResult<void> poll(detail::Context&) { return PollReady; }
};

}  // namespace

// ---------------------------------------------------------------------------
// Polled by hand
// ---------------------------------------------------------------------------

TEST(BarrierTest, SizeOneReleasesAtOnceAsLeader) {
    Barrier barrier(1);
    EXPECT_EQ(barrier.size(), 1u);
    auto waker = detail::make_rc<CountingWaker>();
    detail::Context ctx(waker);
    for (int round = 0; round < 3; ++round) {
        auto wait = barrier.wait();
        auto result = wait.poll(ctx);
        ASSERT_TRUE(result.isReady());
        EXPECT_TRUE(result.value().is_leader());
    }
    EXPECT_EQ(waker->wakes, 0);
}

TEST(BarrierTest, LastArrivalLeadsAndWakesTheOthers) {
    Barrier barrier(3);
    auto waker1 = detail::make_rc<CountingWaker>();
    auto waker2 = detail::make_rc<CountingWaker>();
    auto waker3 = detail::make_rc<CountingWaker>();
    detail::Context ctx1(waker1), ctx2(waker2), ctx3(waker3);

    auto wait1 = barrier.wait();
    auto wait2 = barrier.wait();
    auto wait3 = barrier.wait();
    EXPECT_TRUE(wait1.poll(ctx1).isPending());
    EXPECT_TRUE(wait2.poll(ctx2).isPending());

    // The leader completes without suspending.
    auto result3 = wait3.poll(ctx3);
    ASSERT_TRUE(result3.isReady());
    EXPECT_TRUE(result3.value().is_leader());
    EXPECT_EQ(waker1->wakes, 1);
    EXPECT_EQ(waker2->wakes, 1);
    EXPECT_EQ(waker3->wakes, 0);

    auto result1 = wait1.poll(ctx1);
    auto result2 = wait2.poll(ctx2);
    ASSERT_TRUE(result1.isReady());
    ASSERT_TRUE(result2.isReady());
    EXPECT_FALSE(result1.value().is_leader());
    EXPECT_FALSE(result2.value().is_leader());
}

TEST(BarrierTest, PollBeforeReleaseStaysPendingWithLatestWaker) {
    Barrier barrier(2);
    auto old_waker = detail::make_rc<CountingWaker>();
    auto new_waker = detail::make_rc<CountingWaker>();
    detail::Context old_ctx(old_waker), new_ctx(new_waker), leader_ctx(nullptr);

    auto wait1 = barrier.wait();
    EXPECT_TRUE(wait1.poll(old_ctx).isPending());
    // A second poll (a spurious wake) is not a second arrival.
    EXPECT_TRUE(wait1.poll(new_ctx).isPending());

    auto wait2 = barrier.wait();
    EXPECT_TRUE(wait2.poll(leader_ctx).isReady());
    EXPECT_EQ(old_waker->wakes, 0);
    EXPECT_EQ(new_waker->wakes, 1);
    EXPECT_TRUE(wait1.poll(new_ctx).isReady());
}

TEST(BarrierTest, ReusableForLaterRounds) {
    Barrier barrier(2);
    auto waker = detail::make_rc<CountingWaker>();
    detail::Context ctx(waker);
    for (int round = 0; round < 3; ++round) {
        auto first = barrier.wait();
        auto second = barrier.wait();
        EXPECT_TRUE(first.poll(ctx).isPending());
        auto led = second.poll(ctx);
        ASSERT_TRUE(led.isReady());
        EXPECT_TRUE(led.value().is_leader());
        auto followed = first.poll(ctx);
        ASSERT_TRUE(followed.isReady());
        EXPECT_FALSE(followed.value().is_leader());
    }
    EXPECT_EQ(waker->wakes, 3);
}

TEST(BarrierTest, DroppedWaiterStillCountsAsArrived) {
    Barrier barrier(3);
    auto waker = detail::make_rc<CountingWaker>();
    detail::Context ctx(waker);
    {
        auto dropped = barrier.wait();
        EXPECT_TRUE(dropped.poll(ctx).isPending());
    }
    auto wait2 = barrier.wait();
    EXPECT_TRUE(wait2.poll(ctx).isPending());
    // The dropped waiter's arrival counts, so this is the third.
    auto wait3 = barrier.wait();
    auto result3 = wait3.poll(ctx);
    ASSERT_TRUE(result3.isReady());
    EXPECT_TRUE(result3.value().is_leader());
    // Only the waiter still linked is woken.
    EXPECT_EQ(waker->wakes, 1);
    EXPECT_TRUE(wait2.poll(ctx).isReady());
}

TEST(BarrierTest, WaiterDestroyedAfterReleaseWithoutPolling) {
    Barrier barrier(2);
    detail::Context ctx(detail::make_rc<CountingWaker>());
    {
        auto wait1 = barrier.wait();
        EXPECT_TRUE(wait1.poll(ctx).isPending());
        auto wait2 = barrier.wait();
        EXPECT_TRUE(wait2.poll(ctx).isReady());
        // wait1 is destroyed released but never polled again: its node is no longer
        // in the list, and its destructor must leave the next round alone.
    }
    auto next1 = barrier.wait();
    EXPECT_TRUE(next1.poll(ctx).isPending());
    auto next2 = barrier.wait();
    EXPECT_TRUE(next2.poll(ctx).isReady());
    EXPECT_TRUE(next1.poll(ctx).isReady());
}

TEST(BarrierTest, UnpolledFutureIsNotAnArrival) {
    Barrier barrier(2);
    detail::Context ctx(detail::make_rc<CountingWaker>());
    { auto never_polled = barrier.wait(); }
    auto wait1 = barrier.wait();
    EXPECT_TRUE(wait1.poll(ctx).isPending());
    auto wait2 = barrier.wait();
    EXPECT_TRUE(wait2.poll(ctx).isReady());
}

TEST(BarrierTest, ManyWaitersAllWoken) {
    // More waiters than the leader holds without allocating.
    constexpr std::size_t kWaiters = 40;
    Barrier barrier(kWaiters + 1);
    auto waker = detail::make_rc<CountingWaker>();
    detail::Context ctx(waker);
    std::vector<BarrierWaitFuture> waits;
    waits.reserve(kWaiters);  // no reallocation: polled futures must not move
    for (std::size_t i = 0; i < kWaiters; ++i) {
        waits.push_back(barrier.wait());
        EXPECT_TRUE(waits.back().poll(ctx).isPending());
    }
    auto leader = barrier.wait();
    EXPECT_TRUE(leader.poll(ctx).isReady());
    EXPECT_EQ(waker->wakes, static_cast<int>(kWaiters));
    for (auto& wait : waits) EXPECT_TRUE(wait.poll(ctx).isReady());
}

// ---------------------------------------------------------------------------
// On each executor
// ---------------------------------------------------------------------------

namespace {

constexpr std::size_t kRounds = 4;

// One task of RoundsKeepTasksInStep: arrives kRounds times, checking after each
// release that every task arrived in that round.
Coro<void> step_through_rounds(Barrier& barrier,
                               std::array<std::atomic<std::size_t>, kRounds>& arrived,
                               std::atomic<std::size_t>& leaders,
                               std::atomic<bool>& out_of_step) {
    for (std::size_t round = 0; round < kRounds; ++round) {
        arrived[round].fetch_add(1);
        auto result = co_await barrier.wait();
        if (result.is_leader()) leaders.fetch_add(1);
        if (arrived[round].load() != barrier.size()) out_of_step.store(true);
    }
}

}  // namespace

template<typename Traits>
class BarrierExecutorTest : public testing::Test {
protected:
    Traits traits;
};
TYPED_TEST_SUITE(BarrierExecutorTest, AllExecutors);

TYPED_TEST(BarrierExecutorTest, RoundsKeepTasksInStep) {
    constexpr std::size_t kTasks = 5;
    Barrier barrier(kTasks);
    std::array<std::atomic<std::size_t>, kRounds> arrived{};
    std::atomic<std::size_t> leaders = 0;
    std::atomic<bool> out_of_step = false;
    this->traits.rt.block_on([](Barrier& barrier,
                                std::array<std::atomic<std::size_t>, kRounds>& arrived,
                                std::atomic<std::size_t>& leaders,
                                std::atomic<bool>& out_of_step) -> Coro<void> {
        JoinSet<void> js;
        for (std::size_t i = 0; i < kTasks; ++i)
            js.spawn(step_through_rounds(barrier, arrived, leaders, out_of_step));
        co_await js.drain();
    }(barrier, arrived, leaders, out_of_step));
    EXPECT_FALSE(out_of_step);
    EXPECT_EQ(leaders, kRounds);  // one per round
}

TYPED_TEST(BarrierExecutorTest, WaitInLosingSelectBranchKeepsArrival) {
    Barrier barrier(2);
    bool second_led = false;
    this->traits.rt.block_on([](Barrier& barrier, bool& second_led) -> Coro<void> {
        // The wait arrives, then loses to the ready branch and is destroyed.
        co_await select(barrier.wait(), ReadyVoidFuture{});
        auto result = co_await barrier.wait();
        second_led = result.is_leader();
    }(barrier, second_led));
    EXPECT_TRUE(second_led);
}
