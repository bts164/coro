// SleepFuture / timeout / IntervalTimer on the Pico Runtime, where timers live on
// the CurrentThreadExecutor's own TimerQueue and PicoClock reads time_us_64()
// (stubbed on the host). Built only into test_pico_suite; the desktop equivalents
// are in test_sleep.cpp.

#include <gtest/gtest.h>
#include <coro/coro.h>
#include <coro/runtime/runtime.h>
#include <coro/sync/interval.h>
#include <coro/sync/sleep.h>
#include <coro/sync/timeout.h>

#include <chrono>
#include <vector>

using namespace coro;
using namespace std::chrono_literals;

static_assert(std::is_same_v<Clock, PicoClock>);

TEST(SleepPicoTest, SleepForCompletesAfterDuration) {
    Runtime rt;
    const Instant start = Clock::now();
    rt.block_on([]() -> Coro<void> {
        co_await sleep_for(20ms);
    }());
    EXPECT_GE(Clock::now() - start, 20ms);
}

TEST(SleepPicoTest, SleepUntilCompletesAtDeadline) {
    Runtime rt;
    const Instant deadline = Clock::now() + 15ms;
    rt.block_on([](Instant d) -> Coro<void> {
        co_await sleep_until(d);
    }(deadline));
    EXPECT_GE(Clock::now(), deadline);
}

TEST(SleepPicoTest, PassedDeadlineIsReadyImmediately) {
    Runtime rt;
    rt.block_on([]() -> Coro<void> {
        co_await sleep_until(Clock::now() - 1ms);
    }());
}

TEST(SleepPicoTest, ConcurrentSleepersWakeInDeadlineOrder) {
    Runtime rt;
    std::vector<int> order;
    rt.block_on([](std::vector<int>& out) -> Coro<void> {
        auto a = spawn([](std::vector<int>& o) -> Coro<void> {
            co_await sleep_for(30ms);
            o.push_back(3);
        }(out));
        auto b = spawn([](std::vector<int>& o) -> Coro<void> {
            co_await sleep_for(10ms);
            o.push_back(1);
        }(out));
        auto c = spawn([](std::vector<int>& o) -> Coro<void> {
            co_await sleep_for(20ms);
            o.push_back(2);
        }(out));
        co_await a;
        co_await b;
        co_await c;
    }(order));
    EXPECT_EQ(order, (std::vector<int>{1, 2, 3}));
}

TEST(SleepPicoTest, TimeoutDeadlineWins) {
    Runtime rt;
    const Instant start = Clock::now();
    auto index = rt.block_on([]() -> Coro<int> {
        auto r = co_await timeout(10ms, sleep_for(1s));
        co_return static_cast<int>(r.index());
    }());
    EXPECT_EQ(index, 1);
    EXPECT_LT(Clock::now() - start, 500ms);
}

TEST(SleepPicoTest, IntervalTimerTicksOncePerPeriod) {
    Runtime rt;
    const Instant start = Clock::now();
    rt.block_on([]() -> Coro<void> {
        IntervalTimer timer(5ms);
        for (int i = 0; i < 4; ++i) co_await timer.tick();
    }());
    EXPECT_GE(Clock::now() - start, 20ms);
}
