#include <gtest/gtest.h>
#include "executor_traits.h"
#include <coro/sync/event.h>
#include <coro/sync/select.h>
#include <coro/runtime/runtime.h>
#include <coro/coro.h>

#ifndef CORO_PICO
#include <coro/task/spawn_blocking.h>
#include <chrono>
#include <thread>
#endif

using namespace coro;

static_assert(Future<Event::WaitFuture>);

template<typename Traits>
class EventTest : public testing::Test {
protected:
    Traits traits;
};
TYPED_TEST_SUITE(EventTest, AllExecutors);

TYPED_TEST(EventTest, IsSetReflectsState) {
    Event ev;
    EXPECT_FALSE(ev.is_set());
    ev.set();
    EXPECT_TRUE(ev.is_set());
    ev.clear();
    EXPECT_FALSE(ev.is_set());
}

TYPED_TEST(EventTest, AlreadySetResolvesImmediately) {
    Event ev;
    ev.set();
    bool reached = false;
    this->traits.rt.block_on([](Event& ev, bool& reached) -> Coro<void> {
        co_await ev.wait();
        reached = true;
    }(ev, reached));
    EXPECT_TRUE(reached);
}

TYPED_TEST(EventTest, MultipleSetCallsAreIdempotent) {
    Event ev;
    ev.set();
    ev.set();
    bool reached = false;
    this->traits.rt.block_on([](Event& ev, bool& reached) -> Coro<void> {
        co_await ev.wait();
        reached = true;
    }(ev, reached));
    EXPECT_TRUE(reached);
}

TYPED_TEST(EventTest, WaitFutureDroppedInSelectClearsWaker) {
    Event ev;
    int spurious_wakes = 0;
    struct ImmediateVoid {
        using OutputType = void;
        PollResult<void> poll(detail::Context&) { return PollReady; }
    };
    this->traits.rt.block_on([](Event& ev, int& spurious_wakes) -> Coro<void> {
        co_await select(ev.wait(), ImmediateVoid{});
        EXPECT_FALSE(ev.is_set());
        ev.set();
        struct Counter {
            using OutputType = void;
            int& count;
            PollResult<void> poll(detail::Context&) { ++count; return PollReady; }
        };
        co_await Counter{spurious_wakes};
    }(ev, spurious_wakes));
    EXPECT_EQ(spurious_wakes, 1);
}

TYPED_TEST(EventTest, SetFromSpawnedTask) {
    Event ev;
    bool reached = false;
    this->traits.rt.block_on([](Event& ev, bool& reached) -> Coro<void> {
        auto setter = spawn([](Event& ev) -> Coro<void> {
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
// spawn_blocking tests — desktop only (require BlockingPool)
// ---------------------------------------------------------------------------

#ifndef CORO_PICO

TEST(EventBlockingTest, WaitsUntilSet) {
    Event ev;
    bool reached = false;
    Runtime rt;
    rt.block_on([](Event& ev, bool& reached) -> Coro<void> {
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

TEST(EventBlockingTest, ClearAllowsReuse) {
    Event ev;
    ev.set();
    ev.clear();
    int count = 0;
    Runtime rt;
    rt.block_on([](Event& ev, int& count) -> Coro<void> {
        auto setter = spawn_blocking([&ev] {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            ev.set();
        });
        co_await ev.wait();
        ++count;
        co_await setter;
        ev.clear();
        ev.set();
        co_await ev.wait();
        ++count;
    }(ev, count));
    EXPECT_EQ(count, 2);
}

TEST(EventBlockingTest, StartGatePattern) {
    Event gate;
    std::vector<int> order;
    Runtime rt;
    rt.block_on([](Event& gate, std::vector<int>& order) -> Coro<void> {
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

TEST(EventBlockingTest, DroppingEventWakesWaiter) {
    std::optional<Event> gate;
    gate.emplace();
    bool reached = false;
    Runtime rt;
    rt.block_on([](std::optional<Event>& gate, bool& reached) -> Coro<void> {
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

#endif  // !CORO_PICO
