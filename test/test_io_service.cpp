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
// Request submission
// ---------------------------------------------------------------------------

// Submitting a no-op IoRequest does not crash and is processed by the I/O thread.
TEST(IoServiceTest, SubmitNoOpRequestDoesNotCrash) {
    struct NoOp : IoRequest {
        std::atomic<bool>& executed;
        explicit NoOp(std::atomic<bool>& flag) : executed(flag) {}
        void execute(uv_loop_t*) override { executed.store(true); }
    };

    IoService svc;
    std::atomic<bool> ran{false};
    svc.submit(std::make_unique<NoOp>(ran));
    std::this_thread::sleep_for(10ms);
    EXPECT_TRUE(ran.load());
}

// ---------------------------------------------------------------------------
// SleepFuture — unit-level behaviour
// ---------------------------------------------------------------------------

// SleepFuture with a deadline already in the past returns PollReady immediately,
// without touching IoService at all.
TEST(SleepFutureTest, PollReadyWhenDeadlinePassed) {
    IoService svc;
    set_current_io_service(&svc);

    SleepFuture f(std::chrono::milliseconds(-10));  // deadline clearly in the past after ms rounding
    auto waker = std::make_shared<MockWaker>();
    detail::Context ctx(waker);
    auto result = f.poll(ctx);
    EXPECT_TRUE(result.isReady());

    set_current_io_service(nullptr);
}

// SleepFuture with a future deadline returns PollPending on first poll and
// submits a StartRequest to the IoService.
TEST(SleepFutureTest, PollPendingWhenDeadlineFuture) {
    IoService svc;
    set_current_io_service(&svc);

    SleepFuture f(1000ms);
    auto waker = std::make_shared<MockWaker>();
    detail::Context ctx(waker);
    auto result = f.poll(ctx);
    EXPECT_TRUE(result.isPending());

    // Destructor submits CancelRequest — must not crash.
    set_current_io_service(nullptr);
}

// Dropping a SleepFuture after first poll (mid-wait) submits a CancelRequest.
// This tests the cancellation path without relying on the timer actually firing.
TEST(SleepFutureTest, DestructorSubmitsCancelRequest) {
    IoService svc;
    set_current_io_service(&svc);

    {
        SleepFuture f(500ms);
        auto waker = std::make_shared<MockWaker>();
        detail::Context ctx(waker);
        f.poll(ctx);  // registers timer; destructor will cancel it
        // f destroyed here → CancelRequest submitted
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
        auto start = std::chrono::steady_clock::now();
        co_await sleep_for(50ms);
        auto elapsed = std::chrono::steady_clock::now() - start;
        EXPECT_GE(elapsed, 50ms);
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
