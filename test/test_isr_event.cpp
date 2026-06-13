#include <gtest/gtest.h>
#include "executor_traits.h"
#include <coro/sync/isr_event.h>
#include <thread>
#include <chrono>

using namespace coro;
using namespace std::chrono_literals;

// ---------------------------------------------------------------------------
// IsrEvent tests
//
// std::thread simulates the ISR — it calls signal_from_isr() while block_on()
// is driving the executor. On x86 the volatile write is visible across threads
// via cache coherence; this is sufficient for behavioral testing even though
// volatile does not imply std::atomic ordering on the C++ memory model.
// ---------------------------------------------------------------------------

template<typename Traits>
class IsrEventTest : public testing::Test {
protected:
    Traits traits;
};
TYPED_TEST_SUITE(IsrEventTest, AllExecutors);

TYPED_TEST(IsrEventTest, WaitResumesAfterSignal) {
    IsrEvent ev;
    bool completed = false;

    std::thread t([&ev]() {
        std::this_thread::sleep_for(5ms);
        ev.signal_from_isr();
    });

    this->traits.rt.block_on([](IsrEvent& ev, bool& done) -> Coro<void> {
        co_await ev.wait();
        done = true;
    }(ev, completed));

    t.join();
    EXPECT_TRUE(completed);
}

TYPED_TEST(IsrEventTest, CanBeReusedAcrossMultipleSignals) {
    IsrEvent ev;
    int count = 0;

    std::thread t([&ev]() {
        for (int i = 0; i < 3; ++i) {
            std::this_thread::sleep_for(5ms);
            ev.signal_from_isr();
        }
    });

    this->traits.rt.block_on([](IsrEvent& ev, int& count) -> Coro<void> {
        for (int i = 0; i < 3; ++i) {
            co_await ev.wait();
            ++count;
        }
    }(ev, count));

    t.join();
    EXPECT_EQ(count, 3);
}

TYPED_TEST(IsrEventTest, WaitClearsStaleFlagOnEntry) {
    IsrEvent ev;
    // signal before wait() — wait() should discard this and block until
    // the next signal.
    ev.signal_from_isr();

    bool completed = false;
    std::thread t([&ev]() {
        std::this_thread::sleep_for(5ms);
        ev.signal_from_isr();
    });

    this->traits.rt.block_on([](IsrEvent& ev, bool& done) -> Coro<void> {
        co_await ev.wait();   // discards the pre-signal, waits for the thread's signal
        done = true;
    }(ev, completed));

    t.join();
    EXPECT_TRUE(completed);
}

// ---------------------------------------------------------------------------
// IsrChannel tests
// ---------------------------------------------------------------------------

template<typename Traits>
class IsrChannelTest : public testing::Test {
protected:
    Traits traits;
};
TYPED_TEST_SUITE(IsrChannelTest, AllExecutors);

TYPED_TEST(IsrChannelTest, ReceivesValueFromIsrThread) {
    IsrChannel<int> ch;
    int received = -1;

    std::thread t([&ch]() {
        std::this_thread::sleep_for(5ms);
        ch.send_from_isr(42);
    });

    this->traits.rt.block_on([](IsrChannel<int>& ch, int& out) -> Coro<void> {
        out = co_await ch.receive();
    }(ch, received));

    t.join();
    EXPECT_EQ(received, 42);
}

TYPED_TEST(IsrChannelTest, CanBeReusedAcrossMultipleSends) {
    IsrChannel<int> ch;
    int sum = 0;

    std::thread t([&ch]() {
        for (int i = 1; i <= 3; ++i) {
            std::this_thread::sleep_for(5ms);
            ch.send_from_isr(i);
        }
    });

    this->traits.rt.block_on([](IsrChannel<int>& ch, int& sum) -> Coro<void> {
        for (int i = 0; i < 3; ++i)
            sum += co_await ch.receive();
    }(ch, sum));

    t.join();
    EXPECT_EQ(sum, 6);  // 1 + 2 + 3
}

TYPED_TEST(IsrChannelTest, WorksWithTrivialStruct) {
    struct Point { int x; int y; };
    static_assert(std::is_trivially_copyable_v<Point>);

    IsrChannel<Point> ch;
    Point received{};

    std::thread t([&ch]() {
        std::this_thread::sleep_for(5ms);
        ch.send_from_isr(Point{3, 7});
    });

    this->traits.rt.block_on([](IsrChannel<Point>& ch, Point& out) -> Coro<void> {
        out = co_await ch.receive();
    }(ch, received));

    t.join();
    EXPECT_EQ(received.x, 3);
    EXPECT_EQ(received.y, 7);
}
