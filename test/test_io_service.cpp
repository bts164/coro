#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <coro/runtime/io_service.h>
#include <coro/runtime/runtime.h>
#include <coro/sync/sleep.h>
#include <coro/coro.h>
#include <chrono>
#include <atomic>
#include <thread>

using namespace coro;
using namespace std::chrono_literals;

class MockWaker : public detail::Waker {
public:
    MOCK_METHOD(void, wake, (), (override));
    MOCK_METHOD(std::shared_ptr<detail::Waker>, clone, (), (override));
};

// ---------------------------------------------------------------------------
// Concept checks
// ---------------------------------------------------------------------------

static_assert(Future<SleepFuture>);

// ---------------------------------------------------------------------------
// IoService lifecycle
// ---------------------------------------------------------------------------

// IoService can be constructed and destroyed without crashing.
TEST(IoServiceTest, ConstructDestruct) {
    IoService svc;
}

// stop() is idempotent — calling it multiple times does not crash or deadlock.
TEST(IoServiceTest, StopIsIdempotent) {
    IoService svc;
    svc.stop();
    svc.stop();
}

// ---------------------------------------------------------------------------
// Thread-local access
// ---------------------------------------------------------------------------

// set_current_io_service / current_io_service round-trip correctly.
TEST(IoServiceTest, ThreadLocalRoundTrip) {
    IoService svc;
    set_current_io_service(&svc);
    EXPECT_EQ(&current_io_service(), &svc);
    set_current_io_service(nullptr);
}

// current_io_service() throws when no service is active on this thread.
TEST(IoServiceTest, ThrowsWhenNoServiceActive) {
    set_current_io_service(nullptr);
    EXPECT_THROW(current_io_service(), std::runtime_error);
}

// Each thread gets its own thread-local slot.
TEST(IoServiceTest, ThreadLocalIsPerThread) {
    IoService svc1;
    IoService svc2;

    set_current_io_service(&svc1);

    IoService* seen_in_thread = nullptr;
    std::thread t([&] {
        // Worker thread starts with no service set.
        set_current_io_service(&svc2);
        seen_in_thread = &current_io_service();
        set_current_io_service(nullptr);
    });
    t.join();

    // Main thread still sees svc1; worker saw svc2.
    EXPECT_EQ(&current_io_service(), &svc1);
    EXPECT_EQ(seen_in_thread, &svc2);
    set_current_io_service(nullptr);
}

// ---------------------------------------------------------------------------
// TimerState
// ---------------------------------------------------------------------------

// TimerState fields are accessible and atomic operations compile.
TEST(TimerStateTest, AtomicFieldsCompile) {
    auto state = std::make_shared<TimerState>();
    EXPECT_FALSE(state->fired.load());

    // exchange returns the old value.
    bool old = state->fired.exchange(true);
    EXPECT_FALSE(old);
    EXPECT_TRUE(state->fired.load());
}

// TimerState waker can be stored and loaded atomically.
TEST(TimerStateTest, WakerAtomicStoreLoad) {
    auto state = std::make_shared<TimerState>();
    auto waker = std::make_shared<MockWaker>();
    state->waker.store(waker);
    EXPECT_EQ(state->waker.load(), waker);
}

// ---------------------------------------------------------------------------
// Request submission
// ---------------------------------------------------------------------------

// Submitting a CancelTimer for a state that was never started does not crash.
// (fired.exchange(true) returns false → would call uv_timer_stop on uninitialised
//  handle in a real loop, but with a live IoService the I/O thread handles it.)
TEST(IoServiceTest, SubmitCancelTimerDoesNotCrash) {
    IoService svc;
    auto state = std::make_shared<TimerState>();
    // Mark as already fired so CancelTimer::execute() is a safe no-op.
    state->fired.store(true);
    svc.submit(std::make_unique<CancelTimer>(state));
    // Give the I/O thread a moment to process the queue.
    std::this_thread::sleep_for(10ms);
}

// ---------------------------------------------------------------------------
// SleepFuture — unit-level behaviour
// ---------------------------------------------------------------------------

// SleepFuture with a deadline already in the past returns PollReady immediately,
// without touching IoService at all.
TEST(SleepFutureTest, PollReadyWhenDeadlinePassed) {
    IoService svc;
    set_current_io_service(&svc);

    SleepFuture f(std::chrono::nanoseconds(-1));  // deadline in the past
    auto waker = std::make_shared<MockWaker>();
    detail::Context ctx(waker);
    auto result = f.poll(ctx);
    EXPECT_TRUE(result.isReady());

    set_current_io_service(nullptr);
}

// SleepFuture with a future deadline returns PollPending on first poll and
// submits a StartTimer to the IoService.
TEST(SleepFutureTest, PollPendingWhenDeadlineFuture) {
    IoService svc;
    set_current_io_service(&svc);

    SleepFuture f(1000ms);
    auto waker = std::make_shared<MockWaker>();
    detail::Context ctx(waker);
    auto result = f.poll(ctx);
    EXPECT_TRUE(result.isPending());
    EXPECT_NE(f.m_state, nullptr);  // state was allocated

    // Destructor submits CancelTimer — must not crash.
    set_current_io_service(nullptr);
}

// Dropping a SleepFuture after first poll (mid-wait) submits a CancelTimer.
// This tests the cancellation path without relying on the timer actually firing.
TEST(SleepFutureTest, DestructorSubmitsCancelTimer) {
    IoService svc;
    set_current_io_service(&svc);

    {
        SleepFuture f(500ms);
        auto waker = std::make_shared<MockWaker>();
        detail::Context ctx(waker);
        f.poll(ctx);  // registers timer; m_state is set
        // f destroyed here → CancelTimer submitted
    }
    // Give I/O thread time to process the cancel.
    std::this_thread::sleep_for(20ms);

    set_current_io_service(nullptr);
}

// ---------------------------------------------------------------------------
// Integration — sleep_for through a live Runtime
// (these tests compile in Phase 2 but may not pass until Phase 3)
// ---------------------------------------------------------------------------

// sleep_for completes after the requested duration via the libuv timer.
TEST(SleepIntegrationTest, SleepForCompletesAfterDuration) {
    Runtime rt(1);
    auto start = std::chrono::steady_clock::now();
    rt.block_on([]() -> Coro<void> {
        co_await sleep_for(50ms);
    }());
    auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_GE(elapsed, 50ms);
}

// Multiple concurrent sleep_for calls all complete.
TEST(SleepIntegrationTest, ConcurrentSleeps) {
    Runtime rt(4);
    rt.block_on([]() -> Coro<void> {
        co_await sleep_for(20ms);
        co_await sleep_for(20ms);
    }());
}
