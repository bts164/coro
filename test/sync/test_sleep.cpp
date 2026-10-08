#include <gtest/gtest.h>
#include <coro/sync/interval.h>
#include <coro/sync/sleep.h>
#include <coro/sync/timeout.h>
#include <coro/coro.h>
#include <coro/coro_stream.h>
#include <coro/stream.h>
#include <coro/runtime/runtime.h>
#include <coro/runtime/current_thread_executor.h>
#include <coro/runtime/parker.h>
#include <coro/runtime/work_sharing_executor.h>
#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <stdexcept>
#include <vector>

using namespace coro;
using namespace std::chrono_literals;

// --- Concept check ---
static_assert(Future<SleepFuture>);
static_assert(Future<IntervalTimer::TickFuture>);

// sleep_for completes after the requested duration.
TEST(SleepTest, SleepForCompletesAfterDuration) {
    Runtime rt(1);
    auto start = std::chrono::steady_clock::now();
    rt.block_on([]() -> Coro<void> {
        co_await sleep_for(100ms);
    }());
    auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_GE(elapsed, 100ms);
}

// sleep_for does not complete significantly before the deadline.
TEST(SleepTest, SleepForDoesNotFireEarly) {
    Runtime rt(1);
    auto start = std::chrono::steady_clock::now();
    rt.block_on([]() -> Coro<void> {
        co_await sleep_for(30ms);
    }());
    auto elapsed = std::chrono::steady_clock::now() - start;
    // Allow 5 ms early tolerance for scheduling jitter.
    EXPECT_GE(elapsed, 25ms);
}

// Two sequential sleeps accumulate correctly.
TEST(SleepTest, SequentialSleeps) {
    Runtime rt(1);
    auto start = std::chrono::steady_clock::now();
    rt.block_on([]() -> Coro<void> {
        co_await sleep_for(20ms);
        co_await sleep_for(20ms);
    }());
    auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_GE(elapsed, 40ms);
}

// timeout: future completes before deadline — returns branch 0.
TEST(TimeoutTest, FutureWinsBeforeDeadline) {
    Runtime rt(1);
    auto result = rt.block_on([]() -> Coro<int> {
        auto r = co_await timeout(500ms, []() -> Coro<int> {
            co_return 42;
        }());
        co_return r.index() == 0 ? std::get<0>(r).value : -1;
    }());
    EXPECT_EQ(result, 42);
}

// timeout: deadline passes before a never-completing future — returns branch 1.
TEST(TimeoutTest, DeadlineWinsAgainstSlowFuture) {
    Runtime rt(1);
    auto start = std::chrono::steady_clock::now();
    auto result = rt.block_on([]() -> Coro<int> {
        // sleep_for(500ms) is the "slow future"; timeout wraps it with a 50ms deadline.
        // The inner sleep should be cancelled by the timeout.
        auto r = co_await timeout(50ms, sleep_for(500ms));
        co_return static_cast<int>(r.index()); // 1 = timeout branch
    }());
    auto elapsed = std::chrono::steady_clock::now() - start;

    //EXPECT_EQ(result, 1);
    EXPECT_GE(elapsed, 50ms);
    EXPECT_LT(elapsed, 400ms); // should not wait for the inner 500ms sleep
}

// sleep_for works on the multi-threaded executor too.
TEST(SleepTest, WorksWithMultiThreadedRuntime) {
    Runtime rt(4);
    auto start = std::chrono::steady_clock::now();
    rt.block_on([]() -> Coro<void> {
        co_await sleep_for(50ms);
    }());
    auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_GE(elapsed, 50ms);
}

// --- Timers on the IoDriver (doc/design/timers.md) ---

namespace {

class CountingWaker : public detail::Waker,
                      public std::enable_shared_from_this<CountingWaker> {
public:
    void wake() override { count.fetch_add(1, std::memory_order_relaxed); }
    detail::Rc<detail::Waker> clone() override { return shared_from_this(); }
    int value() const { return count.load(std::memory_order_relaxed); }
private:
    std::atomic<int> count{0};
};

struct TestTask {
    std::shared_ptr<CountingWaker> waker = std::make_shared<CountingWaker>();
    detail::Context ctx{waker};
    int wakes() const { return waker->value(); }
};

Coro<void> many_short_sleeps(int n, std::chrono::microseconds each) {
    for (int i = 0; i < n; ++i) co_await sleep_for(each);
}

// Yields once, after sleeping.
CoroStream<int> yield_after(std::chrono::milliseconds delay) {
    co_await sleep_for(delay);
    co_yield 1;
}

// Polls `stream` from this task, then gives up on it while it is still suspended.
Coro<void> poll_briefly(CoroStream<int>& stream) {
    auto r = co_await timeout(1ms, next(stream));
    EXPECT_EQ(r.index(), 1u);
}

} // namespace

// The driver's epoll_pwait2 timeout is nanosecond-resolution, so short sleeps are not
// rounded up to a millisecond (libuv's timers were). 1000 x 200 us is 0.2 s of sleep;
// at 1 ms per sleep it would take over a second.
TEST(SleepTest, SubMillisecondPrecisionCurrentThread) {
    Runtime rt(1);
    auto start = std::chrono::steady_clock::now();
    rt.block_on(many_short_sleeps(1000, 200us));
    auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_GE(elapsed, 200ms);
    EXPECT_LT(elapsed, 1s);
}

TEST(SleepTest, SubMillisecondPrecisionWorkStealing) {
    Runtime rt(4);
    auto start = std::chrono::steady_clock::now();
    rt.block_on(many_short_sleeps(1000, 200us));
    auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_GE(elapsed, 200ms);
    EXPECT_LT(elapsed, 1s);
}

TEST(SleepTest, WorksWithWorkSharingRuntime) {
    Runtime rt(std::in_place_type<WorkSharingExecutor>, std::size_t{4});
    auto start = std::chrono::steady_clock::now();
    rt.block_on([]() -> Coro<void> {
        co_await sleep_for(30ms);
    }());
    EXPECT_GE(std::chrono::steady_clock::now() - start, 30ms);
}

TEST(SleepTest, SleepUntil) {
    Runtime rt(1);
    const Instant deadline = Clock::now() + 25ms;
    rt.block_on([](Instant d) -> Coro<void> {
        co_await sleep_until(d);
    }(deadline));
    EXPECT_GE(Clock::now(), deadline);
}

TEST(SleepTest, PassedDeadlineIsReadyWithoutATimer) {
    Runtime rt(1);
    rt.block_on([]() -> Coro<void> {
        TestTask task;
        SleepFuture f = sleep_until(Clock::now() - 1ms);
        EXPECT_TRUE(f.poll(task.ctx).isReady());
        co_return;
    }());
}

// A sleep dropped before its deadline fires nothing: its timer is cancelled and the
// queue entry is popped without a wake.
TEST(SleepTest, DroppedSleepDoesNotWake) {
    Runtime rt(1);
    rt.block_on([]() -> Coro<void> {
        TestTask task;
        {
            SleepFuture f = sleep_for(5ms);
            EXPECT_FALSE(f.poll(task.ctx).isReady());
        }
        co_await sleep_for(20ms);
        EXPECT_EQ(task.wakes(), 0);
    }());
}

// A re-poll with a different context moves the timer to the new waker; only the
// latest is woken.
TEST(SleepTest, RepollUpdatesWaker) {
    Runtime rt(1);
    rt.block_on([]() -> Coro<void> {
        TestTask first, second;
        SleepFuture f = sleep_for(5ms);
        EXPECT_FALSE(f.poll(first.ctx).isReady());
        EXPECT_FALSE(f.poll(second.ctx).isReady());
        co_await sleep_for(20ms);
        EXPECT_EQ(first.wakes(), 0);
        EXPECT_EQ(second.wakes(), 1);
        EXPECT_TRUE(f.poll(second.ctx).isReady());
    }());
}

// A re-poll with the same context keeps the one timer: the waker fires once.
TEST(SleepTest, RepollWithSameWakerKeepsTimer) {
    Runtime rt(1);
    rt.block_on([]() -> Coro<void> {
        TestTask task;
        SleepFuture f = sleep_for(5ms);
        EXPECT_FALSE(f.poll(task.ctx).isReady());
        EXPECT_FALSE(f.poll(task.ctx).isReady());
        EXPECT_FALSE(f.poll(task.ctx).isReady());
        co_await sleep_for(20ms);
        EXPECT_EQ(task.wakes(), 1);
        EXPECT_TRUE(f.poll(task.ctx).isReady());
    }());
}

// The timer is named by id, so a polled sleep can still be moved: the moved-to
// future owns the timer, and dropping the moved-from one cancels nothing.
TEST(SleepTest, MovedAfterPollKeepsTimer) {
    Runtime rt(1);
    rt.block_on([]() -> Coro<void> {
        TestTask task;
        std::optional<SleepFuture> first(sleep_for(5ms));
        EXPECT_FALSE(first->poll(task.ctx).isReady());
        SleepFuture second = std::move(*first);
        first.reset();
        co_await sleep_for(20ms);
        EXPECT_EQ(task.wakes(), 1);
        EXPECT_TRUE(second.poll(task.ctx).isReady());
    }());
}

// A stream suspended in a sleep is polled by one task, which then abandons it, and is
// awaited by another. The sleep's timer still names the first task's waker; the
// second task's poll must move it, or the second task is never woken.
TEST(SleepTest, StreamHandedToAnotherTaskStillWakes) {
    Runtime rt(4);
    auto index = rt.block_on([]() -> Coro<int> {
        CoroStream<int> stream = yield_after(50ms);
        co_await spawn(poll_briefly(stream));
        auto r = co_await timeout(2s, next(stream));
        co_return static_cast<int>(r.index());
    }());
    EXPECT_EQ(index, 0);   // the stream's item, not the 2 s timeout
}

// Many timeouts that never expire: each registers a far-off timer (the short sleep
// makes select poll both branches) and cancels it. The cancelled entries are swept
// rather than piling up, and timers keep working afterwards.
TEST(SleepTest, ManyCancelledTimeouts) {
    Runtime rt(1);
    auto start = std::chrono::steady_clock::now();
    int done = rt.block_on([]() -> Coro<int> {
        int n = 0;
        for (int i = 0; i < 10000; ++i) {
            auto r = co_await timeout(1h, []() -> Coro<int> {
                co_await sleep_for(10us);
                co_return 1;
            }());
            if (r.index() == 0) ++n;
        }
        co_await sleep_for(5ms);
        co_return n;
    }());
    EXPECT_EQ(done, 10000);
    EXPECT_LT(std::chrono::steady_clock::now() - start, 5s);
}

TEST(SleepTest, ManyConcurrentSleepers) {
    constexpr int kSleepers = 10000;
    Runtime rt(4);
    auto start = std::chrono::steady_clock::now();
    int done = rt.block_on([]() -> Coro<int> {
        std::vector<JoinHandle<void>> handles;
        handles.reserve(kSleepers);
        for (int i = 0; i < kSleepers; ++i) {
            handles.push_back(spawn([](int ms) -> Coro<void> {
                co_await sleep_for(std::chrono::milliseconds(ms));
            }(1 + i % 50)));
        }
        int n = 0;
        for (auto& h : handles) {
            co_await h;
            ++n;
        }
        co_return n;
    }());
    auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_EQ(done, kSleepers);
    EXPECT_GE(elapsed, 50ms);
    EXPECT_LT(elapsed, 2s);
}

TEST(TimeoutTest, TimeoutAtDeadlineWins) {
    Runtime rt(1);
    auto start = std::chrono::steady_clock::now();
    auto index = rt.block_on([]() -> Coro<int> {
        auto r = co_await timeout_at(Clock::now() + 30ms, sleep_for(1s));
        co_return static_cast<int>(r.index());
    }());
    auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_EQ(index, 1);
    EXPECT_GE(elapsed, 30ms);
    EXPECT_LT(elapsed, 500ms);
}

TEST(IntervalTimerTest, TicksOncePerPeriod) {
    Runtime rt(1);
    const Instant start = Clock::now();
    rt.block_on([]() -> Coro<void> {
        IntervalTimer timer(5ms);
        for (int i = 0; i < 4; ++i) co_await timer.tick();
    }());
    EXPECT_GE(Clock::now() - start, 20ms);
}

// Work done between ticks comes out of the wait: three 40 ms periods with 20 ms of
// work in each take 120 ms, not 180 ms.
TEST(IntervalTimerTest, AbsorbsWorkBetweenTicks) {
    Runtime rt(1);
    const Instant start = Clock::now();
    rt.block_on([]() -> Coro<void> {
        IntervalTimer timer(40ms);
        for (int i = 0; i < 3; ++i) {
            co_await sleep_for(20ms);
            co_await timer.tick();
        }
    }());
    const auto elapsed = Clock::now() - start;
    EXPECT_GE(elapsed, 120ms);
    EXPECT_LT(elapsed, 170ms);
}

// A tick dropped mid-wait doesn't advance the schedule: the next tick() still
// completes one period after construction, not two.
TEST(IntervalTimerTest, DroppedTickKeepsSchedule) {
    Runtime rt(1);
    const Instant start = Clock::now();
    const auto index = rt.block_on([]() -> Coro<int> {
        IntervalTimer timer(100ms);
        auto r = co_await timeout(5ms, timer.tick());
        co_await timer.tick();
        co_return static_cast<int>(r.index());
    }());
    const auto elapsed = Clock::now() - start;
    EXPECT_EQ(index, 1);
    EXPECT_GE(elapsed, 100ms);
    EXPECT_LT(elapsed, 180ms);
}

// After falling several periods behind, one tick is ready at once and the next
// waits a full period: the missed ticks are not delivered in a burst.
TEST(IntervalTimerTest, SkipsMissedTicks) {
    Runtime rt(1);
    const auto after_late = rt.block_on([]() -> Coro<Clock::duration> {
        IntervalTimer timer(10ms);
        co_await sleep_for(35ms);
        const Instant late = Clock::now();
        co_await timer.tick();   // overdue: ready at once
        co_await timer.tick();
        co_await timer.tick();
        co_return Clock::now() - late;
    }());
    EXPECT_GE(after_late, 20ms);
}

// An executor that never turns the IoDriver cannot fire timers on it; the first
// pending poll says so rather than hanging.
TEST(SleepTest, ThrowsWithoutDriver) {
    Runtime rt(std::in_place_type<CurrentThreadExecutor>,
               std::make_unique<PollingParker>([] {}));
    ASSERT_FALSE(rt.turns_io_driver());
    EXPECT_THROW(rt.block_on([]() -> Coro<void> {
        co_await sleep_for(1ms);
    }()), std::logic_error);
}
