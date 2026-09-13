#include <gtest/gtest.h>
#include "executor_traits.h"
#include <coro/sync/isr_event.h>
#include <thread>
#include <chrono>
#include <atomic>

using namespace coro;
using namespace std::chrono_literals;

// ---------------------------------------------------------------------------
// IsrEvent tests
//
// std::thread simulates the ISR — it calls signal_from_isr() while block_on()
// is driving the executor. Unlike a real single-core interrupt, a std::thread
// is genuinely concurrent under the C++ memory model, so this only gives
// well-defined behavior because IsrEvent/IsrChannel serialize every access
// through a real lock (a std::mutex-backed stub of the hardware spin lock on
// the host build; see test/pico/stub/hardware/sync.h and
// doc/design/isr_safety.md, "Cross-core ISR delivery").
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
    // Signals sent so far, updated by the ISR thread immediately before each
    // signal_from_isr(). If a later wait() ever resolved on an earlier,
    // already-observed signal instead of genuinely waiting for its own, this
    // would still pass with count==3 at the end -- so each iteration checks
    // that at least that many signals have actually been sent by the time it
    // resolves, catching a wait() that resolves early on a stale epoch.
    std::atomic<int> signals_sent{0};

    std::thread t([&ev, &signals_sent]() {
        for (int i = 0; i < 3; ++i) {
            std::this_thread::sleep_for(5ms);
            signals_sent.store(i + 1, std::memory_order_relaxed);
            ev.signal_from_isr();
        }
    });

    this->traits.rt.block_on(
        [](IsrEvent& ev, int& count, std::atomic<int>& signals_sent) -> Coro<void> {
            for (int i = 0; i < 3; ++i) {
                co_await ev.wait();
                ++count;
                EXPECT_GE(signals_sent.load(std::memory_order_relaxed), count);
            }
        }(ev, count, signals_sent));

    t.join();
    EXPECT_EQ(count, 3);
}

TYPED_TEST(IsrEventTest, WaitDoesNotResolveOnASignalThatAlreadyHappenedBeforeItStarted) {
    IsrEvent ev;
    // A signal from before wait() is even called is stale by the time
    // wait() starts -- it must not resolve immediately on it. See
    // IsrEvent's class comment in isr_event.h for why: each wait() captures
    // the current epoch as its own baseline and only resolves on a signal
    // strictly after that point.
    ev.signal_from_isr();

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

TYPED_TEST(IsrEventTest, ConcurrentWaitersBothResolveOnOneSignal) {
    // Broadcast: two waiters started before the signal must both resolve on
    // it, not just whichever one's poll() happens to run first. See
    // doc/design/isr_safety.md, "Multiple waiters".
    IsrEvent ev;
    bool a_done = false, b_done = false;

    std::thread t([&ev]() {
        std::this_thread::sleep_for(5ms);
        ev.signal_from_isr();
    });

    this->traits.rt.block_on(
        [](IsrEvent& ev, bool& a_done, bool& b_done) -> Coro<void> {
            auto h1 = spawn([](IsrEvent& ev, bool& done) -> Coro<void> {
                co_await ev.wait();
                done = true;
            }(ev, a_done));
            auto h2 = spawn([](IsrEvent& ev, bool& done) -> Coro<void> {
                co_await ev.wait();
                done = true;
            }(ev, b_done));
            co_await h1;
            co_await h2;
        }(ev, a_done, b_done));

    t.join();
    EXPECT_TRUE(a_done);
    EXPECT_TRUE(b_done);
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

TYPED_TEST(IsrChannelTest, ConcurrentReceiversEachClaimOneSend) {
    // Two concurrent receive()s on the same channel, two sends — each send
    // must be claimed exactly once, so the two receivers' results must be
    // {1, 2} in some order (never both 1, never both 2). See
    // doc/design/isr_safety.md, "Multiple waiters".
    IsrChannel<int> ch;
    int a = -1, b = -1;

    std::thread t([&ch]() {
        std::this_thread::sleep_for(5ms);
        ch.send_from_isr(1);
        std::this_thread::sleep_for(5ms);
        ch.send_from_isr(2);
    });

    this->traits.rt.block_on(
        [](IsrChannel<int>& ch, int& a, int& b) -> Coro<void> {
            auto h1 = spawn(ch.receive());
            auto h2 = spawn(ch.receive());
            a = co_await h1;
            b = co_await h2;
        }(ch, a, b));

    t.join();
    EXPECT_NE(a, b);
    EXPECT_TRUE((a == 1 && b == 2) || (a == 2 && b == 1));
}

// ---------------------------------------------------------------------------
// IsrSemaphore tests
// ---------------------------------------------------------------------------

template<typename Traits>
class IsrSemaphoreTest : public testing::Test {
protected:
    Traits traits;
};
TYPED_TEST_SUITE(IsrSemaphoreTest, AllExecutors);

TYPED_TEST(IsrSemaphoreTest, AcquireResumesAfterRelease) {
    IsrSemaphore counter;
    bool completed = false;

    std::thread t([&counter]() {
        std::this_thread::sleep_for(5ms);
        counter.release_from_isr();
    });

    this->traits.rt.block_on([](IsrSemaphore& counter, bool& done) -> Coro<void> {
        co_await counter.acquire();
        done = true;
    }(counter, completed));

    t.join();
    EXPECT_TRUE(completed);
}

TYPED_TEST(IsrSemaphoreTest, AcquireClaimsOneCountPerCall) {
    IsrSemaphore counter;
    int acquired = 0;

    std::thread t([&counter]() {
        for (int i = 0; i < 3; ++i) {
            std::this_thread::sleep_for(5ms);
            counter.release_from_isr();
        }
    });

    this->traits.rt.block_on([](IsrSemaphore& counter, int& acquired) -> Coro<void> {
        for (int i = 0; i < 3; ++i) {
            co_await counter.acquire();
            ++acquired;
        }
    }(counter, acquired));

    t.join();
    EXPECT_EQ(acquired, 3);
}

TYPED_TEST(IsrSemaphoreTest, ReleasesBeforeAcquireAreNotLost) {
    IsrSemaphore counter;
    counter.release_from_isr();
    counter.release_from_isr();

    int acquired = 0;
    this->traits.rt.block_on([](IsrSemaphore& counter, int& acquired) -> Coro<void> {
        co_await counter.acquire();
        ++acquired;
        co_await counter.acquire();
        ++acquired;
    }(counter, acquired));

    EXPECT_EQ(acquired, 2);
}
