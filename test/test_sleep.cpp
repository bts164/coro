#include <gtest/gtest.h>
#include <coro/sync/sleep.h>
#include <coro/sync/timeout.h>
#include <coro/coro.h>
#include <coro/runtime/runtime.h>
#include <chrono>

using namespace coro;
using namespace std::chrono_literals;

// --- Concept check ---
static_assert(Future<SleepFuture>);

// sleep_for completes after the requested duration.
TEST(SleepTest, SleepForCompletesAfterDuration) {
    Runtime rt(1);
    auto start = std::chrono::steady_clock::now();
    rt.block_on([]() -> Coro<void> {
        co_await sleep_for(50ms);
    }());
    auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_GE(elapsed, 50ms);
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
        auto r = co_await timeout(50min, sleep_for(500min));
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
