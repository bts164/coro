#include <gtest/gtest.h>
#include "executor_traits.h"
#include "isr_trigger.h"
#include <coro/runtime/clock.h>
#include <coro/sync/isr_event.h>
#include <coro/sync/select.h>
#include <coro/sync/sleep.h>
#include <coro/sync/timeout.h>
#include <chrono>
#include <atomic>
#include <cstdint>
#include <vector>

using namespace coro;
using namespace std::chrono_literals;

// ---------------------------------------------------------------------------
// IsrEvent tests
//
// IsrTrigger (isr_trigger.h) calls signal_from_isr() and its relatives while
// block_on() is driving the executor. On the board that call comes from a real
// interrupt handler, a hardware timer alarm. On the host a std::thread stands in
// for it. Unlike a single-core interrupt, a std::thread is genuinely concurrent
// under the C++ memory model, so the host run only gives well-defined behavior
// because IsrEvent/IsrChannel serialize every access through a real lock (a
// std::mutex-backed stub of the hardware spin lock; see
// test/pico/stub/hardware/sync.h and doc/design/isr_safety.md, "Cross-core ISR
// delivery").
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

    {
        IsrTrigger isr(5ms, 1, [&ev](int) { ev.signal_from_isr(); });

        this->traits.rt.block_on([](IsrEvent& ev, bool& done) -> Coro<void> {
            co_await ev.wait();
            done = true;
        }(ev, completed));
    }
    EXPECT_TRUE(completed);
}

TYPED_TEST(IsrEventTest, CanBeReusedAcrossMultipleSignals) {
    IsrEvent ev;
    int count = 0;
    // Signals sent so far, updated by the ISR immediately before each
    // signal_from_isr(). If a later wait() ever resolved on an earlier,
    // already-observed signal instead of genuinely waiting for its own, this
    // would still pass with count==3 at the end -- so each iteration checks
    // that at least that many signals have actually been sent by the time it
    // resolves, catching a wait() that resolves early on a stale epoch.
    std::atomic<int> signals_sent{0};

    {
        IsrTrigger isr(5ms, 3, [&ev, &signals_sent](int i) {
            signals_sent.store(i + 1, std::memory_order_relaxed);
            ev.signal_from_isr();
        });

        this->traits.rt.block_on(
            [](IsrEvent& ev, int& count, std::atomic<int>& signals_sent) -> Coro<void> {
                for (int i = 0; i < 3; ++i) {
                    co_await ev.wait();
                    ++count;
                    EXPECT_GE(signals_sent.load(std::memory_order_relaxed), count);
                }
            }(ev, count, signals_sent));
    }
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
    {
        IsrTrigger isr(5ms, 1, [&ev](int) { ev.signal_from_isr(); });

        this->traits.rt.block_on([](IsrEvent& ev, bool& done) -> Coro<void> {
            co_await ev.wait();
            done = true;
        }(ev, completed));
    }
    EXPECT_TRUE(completed);
}

TYPED_TEST(IsrEventTest, ConcurrentWaitersBothResolveOnOneSignal) {
    // Broadcast: two waiters started before the signal must both resolve on
    // it, not just whichever one's poll() happens to run first. See
    // doc/design/isr_safety.md, "Multiple waiters".
    IsrEvent ev;
    bool a_done = false, b_done = false;

    {
        IsrTrigger isr(5ms, 1, [&ev](int) { ev.signal_from_isr(); });

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
    }
    EXPECT_TRUE(a_done);
    EXPECT_TRUE(b_done);
}

namespace {
// Wakes itself on every poll until *m_stop is set, so the executor's local queue
// never runs empty. Gives up (false) after five seconds instead of hanging.
struct SpinUntilFuture {
    using OutputType = bool;
    const bool*                           m_stop;
    std::chrono::steady_clock::time_point m_deadline = std::chrono::steady_clock::now() + 5s;

    PollResult<bool> poll(detail::Context& ctx) {
        if (*m_stop) return true;
        if (std::chrono::steady_clock::now() > m_deadline) return false;
        ctx.getWaker()->wake();
        return PollPending;
    }
};
} // namespace

TYPED_TEST(IsrEventTest, SignalSeenWhileATaskKeepsTheLoopBusy) {
    // The root task is runnable all the time and never yields, so the executor
    // never finds its queue empty. The signal must still reach the waiter: the loop
    // looks for interrupt events after every batch of polls, not only when it has
    // run out of work.
    IsrEvent ev;
    bool waiter_done = false;
    bool seen = false;

    {
        IsrTrigger isr(5ms, 1, [&ev](int) { ev.signal_from_isr(); });

        seen = this->traits.rt.block_on(
            [](IsrEvent& ev, bool& waiter_done) -> Coro<bool> {
                auto waiter = spawn([](IsrEvent& ev, bool& done) -> Coro<void> {
                    co_await ev.wait();
                    done = true;
                }(ev, waiter_done));
                const bool seen = co_await SpinUntilFuture{&waiter_done};
                if (seen) co_await waiter;
                co_return seen;
            }(ev, waiter_done));
    }
    EXPECT_TRUE(seen);
}

TYPED_TEST(IsrEventTest, FortyWaitersAllResolve) {
    // More waiters than the driver wakes in one batch (IoDriver::kWakeBatch, 32).
    // Each waits from the epoch before the trigger starts, so one that first runs
    // after the signal still resolves on it.
    constexpr int kWaiters = 40;
    IsrEvent ev;
    const uint64_t baseline = ev.epoch();
    int resolved = 0;

    {
        IsrTrigger isr(5ms, 1, [&ev](int) { ev.signal_from_isr(); });

        this->traits.rt.block_on([](IsrEvent& ev, uint64_t baseline, int& resolved) -> Coro<void> {
            std::vector<JoinHandle<void>> waiters;
            for (int i = 0; i < kWaiters; ++i) {
                waiters.push_back(spawn([](IsrEvent& ev, uint64_t baseline, int& resolved) -> Coro<void> {
                    auto r = co_await timeout(5s, ev.wait_from(baseline));
                    if (r.index() == 0) ++resolved;
                }(ev, baseline, resolved)));
            }
            for (auto& w : waiters) co_await w;
        }(ev, baseline, resolved));
    }
    EXPECT_EQ(resolved, kWaiters);
}

TYPED_TEST(IsrEventTest, DroppedWaiterLeavesOthersWaiting) {
    // A wait that loses a select() unlinks itself from the shared registration, and
    // the waiter beside it still resolves. The next wait, made once both are gone,
    // registers again. The signals are 50 ms apart, after the 5 ms the dropped wait
    // gives up at; epoch baselines keep each wait on its own signal however late it
    // starts.
    IsrEvent ev;
    const uint64_t baseline = ev.epoch();
    bool dropped_gave_up = false, kept_done = false, later_done = false;

    {
        IsrTrigger isr(50ms, 2, [&ev](int) { ev.signal_from_isr(); });

        this->traits.rt.block_on(
            [](IsrEvent& ev, uint64_t baseline, bool& dropped_gave_up, bool& kept_done,
               bool& later_done) -> Coro<void> {
                auto kept = spawn([](IsrEvent& ev, uint64_t baseline, bool& done) -> Coro<void> {
                    auto r = co_await timeout(5s, ev.wait_from(baseline));
                    done = r.index() == 0;
                }(ev, baseline, kept_done));

                auto r = co_await select(ev.wait_from(baseline), sleep_for(5ms));
                dropped_gave_up = r.index() == 1;
                co_await kept;

                auto later = co_await timeout(5s, ev.wait_from(baseline + 1));
                later_done = later.index() == 0;
            }(ev, baseline, dropped_gave_up, kept_done, later_done));
    }
    EXPECT_TRUE(dropped_gave_up);
    EXPECT_TRUE(kept_done);
    EXPECT_TRUE(later_done);
}

TEST(IsrEventRuntimes, EventUsedByTwoRuntimesInTurn) {
    // The registration belongs to the runtime the first waiter ran on, and goes
    // with the last waiter, so the next wait registers on whichever runtime it runs
    // on. Both runtimes are alive throughout.
    IsrEvent ev;
    SingleThreadRuntime a;
    SingleThreadRuntime b;
    bool a_done = false, b_done = false;

    auto wait_once = [](IsrEvent& ev, bool& done) -> Coro<void> {
        auto r = co_await timeout(5s, ev.wait());
        done = r.index() == 0;
    };
    {
        IsrTrigger isr(5ms, 1, [&ev](int) { ev.signal_from_isr(); });
        a.block_on(wait_once(ev, a_done));
    }
    {
        IsrTrigger isr(5ms, 1, [&ev](int) { ev.signal_from_isr(); });
        b.block_on(wait_once(ev, b_done));
    }
    EXPECT_TRUE(a_done);
    EXPECT_TRUE(b_done);
}

TEST(IsrEventRuntimes, RuntimePollDeliversIsrSignals) {
    // Firmware with its own main loop calls Runtime::poll() rather than block_on():
    // it must deliver the interrupt's signal, and fire the sleep's timer after it.
    // The runtime is declared after the event, so a waiter left by a failure goes
    // first.
    IsrEvent ev;
    const uint64_t baseline = ev.epoch();
    IsrTrigger isr(5ms, 1, [&ev](int) { ev.signal_from_isr(); });
    SingleThreadRuntime rt;
    bool done = false;

    rt.spawn([](IsrEvent& ev, uint64_t baseline, bool& done) -> Coro<void> {
        co_await ev.wait_from(baseline);
        co_await sleep_for(10ms);
        done = true;
    }(ev, baseline, done)).detach();

    const Instant deadline = Clock::now() + 5s;
    while (!done && Clock::now() < deadline) rt.poll();
    EXPECT_TRUE(done);
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

TYPED_TEST(IsrChannelTest, ReceivesValueFromIsr) {
    IsrChannel<int> ch;
    int received = -1;
    {
        IsrTrigger isr(5ms, 1, [&ch](int) { ch.send_from_isr(42); });

        this->traits.rt.block_on([](IsrChannel<int>& ch, int& out) -> Coro<void> {
            out = co_await ch.receive();
        }(ch, received));
    }
    EXPECT_EQ(received, 42);
}

TYPED_TEST(IsrChannelTest, CanBeReusedAcrossMultipleSends) {
    IsrChannel<int> ch;
    int sum = 0;

    {
        IsrTrigger isr(5ms, 3, [&ch](int i) { ch.send_from_isr(i + 1); });

        this->traits.rt.block_on([](IsrChannel<int>& ch, int& sum) -> Coro<void> {
            for (int i = 0; i < 3; ++i)
                sum += co_await ch.receive();
        }(ch, sum));
    }
    EXPECT_EQ(sum, 6);  // 1 + 2 + 3
}

TYPED_TEST(IsrChannelTest, WorksWithTrivialStruct) {
    struct Point { int x; int y; };
    static_assert(std::is_trivially_copyable_v<Point>);

    IsrChannel<Point> ch;
    Point received{};

    {
        IsrTrigger isr(5ms, 1, [&ch](int) { ch.send_from_isr(Point{3, 7}); });

        this->traits.rt.block_on([](IsrChannel<Point>& ch, Point& out) -> Coro<void> {
            out = co_await ch.receive();
        }(ch, received));
    }
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

    {
        IsrTrigger isr(5ms, 2, [&ch](int i) { ch.send_from_isr(i + 1); });

        this->traits.rt.block_on(
            [](IsrChannel<int>& ch, int& a, int& b) -> Coro<void> {
                auto h1 = spawn(ch.receive());
                auto h2 = spawn(ch.receive());
                a = co_await h1;
                b = co_await h2;
            }(ch, a, b));
    }
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

    {
        IsrTrigger isr(5ms, 1, [&counter](int) { counter.release_from_isr(); });

        this->traits.rt.block_on([](IsrSemaphore& counter, bool& done) -> Coro<void> {
            co_await counter.acquire();
            done = true;
        }(counter, completed));
    }
    EXPECT_TRUE(completed);
}

TYPED_TEST(IsrSemaphoreTest, AcquireClaimsOneCountPerCall) {
    IsrSemaphore counter;
    int acquired = 0;

    {
        IsrTrigger isr(5ms, 3, [&counter](int) { counter.release_from_isr(); });

        this->traits.rt.block_on([](IsrSemaphore& counter, int& acquired) -> Coro<void> {
            for (int i = 0; i < 3; ++i) {
                co_await counter.acquire();
                ++acquired;
            }
        }(counter, acquired));
    }
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

TYPED_TEST(IsrSemaphoreTest, ReleaseNTimesWakesExactlyN) {
    // Four acquires wait, the interrupt releases twice, and exactly two resolve: each
    // claims one count, and the others wait again. Then two releases from the task
    // let the rest finish.
    constexpr int kWaiters  = 4;
    constexpr int kReleases = 2;
    IsrSemaphore sem;
    int acquired = 0;
    int after_settling = -1;

    {
        IsrTrigger isr(5ms, kReleases, [&sem](int) { sem.release_from_isr(); });

        this->traits.rt.block_on(
            [](IsrSemaphore& sem, int& acquired, int& after_settling) -> Coro<void> {
                std::vector<JoinHandle<void>> waiters;
                for (int i = 0; i < kWaiters; ++i) {
                    waiters.push_back(spawn([](IsrSemaphore& sem, int& acquired) -> Coro<void> {
                        auto r = co_await timeout(5s, sem.acquire());
                        if (r.index() == 0) ++acquired;
                    }(sem, acquired)));
                }
                const Instant deadline = Clock::now() + 5s;
                while (acquired < kReleases && Clock::now() < deadline)
                    co_await sleep_for(5ms);
                // Long enough for a third waiter to have run, had it claimed anything.
                co_await sleep_for(50ms);
                after_settling = acquired;

                for (int i = kReleases; i < kWaiters; ++i) sem.release_from_isr();
                for (auto& w : waiters) co_await w;
            }(sem, acquired, after_settling));
    }
    EXPECT_EQ(after_settling, kReleases);
    EXPECT_EQ(acquired, kWaiters);
}
