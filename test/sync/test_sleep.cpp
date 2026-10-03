#include <gtest/gtest.h>
#include <coro/sync/sleep.h>
#include <coro/sync/timeout.h>
#include <coro/coro.h>
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

// A sleep dropped before its deadline fires nothing: its slot is emptied and the
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

// A re-poll with a different context (as under select) replaces the stored waker;
// only the latest is woken.
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
