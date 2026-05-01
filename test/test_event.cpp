#include <gtest/gtest.h>
#include <coro/sync/event.h>
#include <coro/sync/select.h>
#include <coro/runtime/runtime.h>
#include <coro/task/spawn_blocking.h>
#include <coro/coro.h>
#include <chrono>
#include <thread>

using namespace coro;

// ---------------------------------------------------------------------------
// Concept check
// ---------------------------------------------------------------------------

static_assert(Future<Event::WaitFuture>);

// ---------------------------------------------------------------------------
// Already-set event resolves immediately (latch semantics)
// ---------------------------------------------------------------------------

TEST(EventTest, AlreadySetResolvesImmediately) {
    Event ev;
    ev.set();

    bool reached = false;
    Runtime rt;
    rt.block_on([](auto &ev, auto &reached) -> Coro<void> {
        co_await ev.wait();
        reached = true;
    }(ev, reached));

    EXPECT_TRUE(reached);
}

// ---------------------------------------------------------------------------
// Wait suspends until set() is called from another thread
// ---------------------------------------------------------------------------

TEST(EventTest, WaitsUntilSet) {
    Event ev;
    bool reached = false;

    Runtime rt;
    rt.block_on([](auto &ev, auto &reached) -> Coro<void> {
        auto setter = spawn_blocking([&ev] {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            ev.set();
        });

        co_await ev.wait();
        reached = true;
        co_await setter;
    }(ev, reached));

    EXPECT_TRUE(reached);
}

// ---------------------------------------------------------------------------
// WaitFuture dropped by a cancelled select() branch clears its waker so no
// spurious wake reaches the task after it has moved on.
// ---------------------------------------------------------------------------

TEST(EventTest, WaitFutureDroppedInSelectClearsWaker) {
    Event ev;
    int spurious_wakes = 0;

    struct ImmediateVoid {
        using OutputType = void;
        PollResult<void> poll(detail::Context&) { return PollReady; }
    };

    Runtime rt;
    rt.block_on([](auto& ev, auto& spurious_wakes) -> Coro<void> {
        // ImmediateVoid wins; ev.wait() branch is cancelled and its WaitFuture
        // is dropped — the waker must be cleared from the event.
        co_await select(ev.wait(), ImmediateVoid{});

        EXPECT_FALSE(ev.is_set());

        // set() with no registered waker should not schedule a spurious wake.
        ev.set();

        // Count how many times this future is polled. Without the waker fix
        // a spurious extra poll would push count to 2.
        struct Counter {
            using OutputType = void;
            int& count;
            PollResult<void> poll(detail::Context&) { ++count; return PollReady; }
        };
        co_await Counter{spurious_wakes};
    }(ev, spurious_wakes));

    EXPECT_EQ(spurious_wakes, 1);
}

// ---------------------------------------------------------------------------
// clear() resets the event; subsequent wait() suspends again
// ---------------------------------------------------------------------------

TEST(EventTest, ClearAllowsReuse) {
    Event ev;
    ev.set();
    ev.clear();

    int count = 0;
    Runtime rt;
    rt.block_on([](auto &ev, auto &count) -> Coro<void> {
        // First wait — event is cleared, must block.
        auto setter = spawn_blocking([&ev] {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            ev.set();
        });
        co_await ev.wait();
        ++count;
        co_await setter;

        // Second wait — set again immediately.
        ev.clear();
        ev.set();
        co_await ev.wait();
        ++count;
    }(ev, count));

    EXPECT_EQ(count, 2);
}

// ---------------------------------------------------------------------------
// is_set() reflects current state
// ---------------------------------------------------------------------------

TEST(EventTest, IsSetReflectsState) {
    Event ev;
    EXPECT_FALSE(ev.is_set());

    ev.set();
    EXPECT_TRUE(ev.is_set());

    ev.clear();
    EXPECT_FALSE(ev.is_set());
}

// ---------------------------------------------------------------------------
// set() before wait() is idempotent (multiple set() calls don't break things)
// ---------------------------------------------------------------------------

TEST(EventTest, MultipleSetCallsAreIdempotent) {
    Event ev;
    ev.set();
    ev.set();  // second call is a no-op

    bool reached = false;
    Runtime rt;
    rt.block_on([](auto &ev, auto &reached) -> Coro<void> {
        co_await ev.wait();
        reached = true;
    }(ev, reached));

    EXPECT_TRUE(reached);
}

// ---------------------------------------------------------------------------
// Event used as a start-gate: producer signals, multiple sequential consumers
// each wait then clear for the next round.
// ---------------------------------------------------------------------------

TEST(EventTest, StartGatePattern) {
    Event gate;
    std::vector<int> order;

    Runtime rt;
    rt.block_on([](auto &gate, auto &order) -> Coro<void> {
        for (int i = 0; i < 3; ++i) {
            auto setter = spawn_blocking([&gate, i] {
                std::this_thread::sleep_for(std::chrono::milliseconds(5 * (i + 1)));
                gate.set();
            });
            co_await gate.wait();
            order.push_back(i);
            gate.clear();
            co_await setter;
        }
    }(gate, order));

    ASSERT_EQ(order.size(), 3u);
    EXPECT_EQ(order[0], 0);
    EXPECT_EQ(order[1], 1);
    EXPECT_EQ(order[2], 2);
}

// ---------------------------------------------------------------------------
// set() called from a spawned task wakes the waiter
// ---------------------------------------------------------------------------

TEST(EventTest, SetFromSpawnedTask) {
    Event ev;
    bool reached = false;

    Runtime rt;
    rt.block_on([](auto &ev, auto &reached) -> Coro<void> {
        auto setter = spawn([](auto &ev) -> Coro<void> {
            ev.set();
            co_return;
        }(ev));

        co_await ev.wait();
        reached = true;
        co_await setter;
    }(ev, reached));

    EXPECT_TRUE(reached);
}

// ---------------------------------------------------------------------------
// Destroying an Event while a waiter is suspended wakes the waiter and
// resolves the WaitFuture as if set() had been called.
// ---------------------------------------------------------------------------

TEST(EventTest, DroppingEventWakesWaiter) {
    std::optional<Event> gate;
    gate.emplace();
    bool reached = false;

    Runtime rt;
    rt.block_on([](auto &gate, auto &reached) -> Coro<void> {
        auto setter = spawn_blocking([&gate] {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            gate.reset();
        });
        co_await gate->wait();
        reached = true;
        co_await setter;
    }(gate, reached));

    EXPECT_TRUE(reached);
}