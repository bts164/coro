#include <gtest/gtest.h>
#include <coro/pico/hal/gpio.h>
#include <coro/runtime/runtime.h>
#include <coro/coro.h>
#include <coro/stream.h>
#include <hardware/gpio.h>  // stub
#include <thread>
#include <chrono>
#include <utility>
#include <co_assert.h>

using namespace coro;
using namespace coro::pico::hal;
using namespace std::chrono_literals;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static Runtime make_rt() {
    return Runtime{};
}

// Fires a GPIO IRQ for `pin` from a background thread after a short delay,
// simulating a real edge interrupt. Sets the stub's tracked level to
// `new_level` first, matching what a real edge would have just settled to.
static std::thread fire_irq_after(uint pin, bool new_level, uint32_t event_mask,
                                  std::chrono::milliseconds delay = 5ms) {
    return std::thread([pin, new_level, event_mask, delay]() {
        std::this_thread::sleep_for(delay);
        gpio_stub::set_level(pin, new_level);
        coro_pico_hal_gpio_fire_irq(pin, event_mask);
    });
}

// ---------------------------------------------------------------------------
// Compile-time API checks
// ---------------------------------------------------------------------------

static_assert(!std::is_copy_constructible_v<GpioPin>);
static_assert(!std::is_move_constructible_v<GpioPin>);

static_assert(!std::is_copy_constructible_v<GpioEdgeFuture>);
static_assert(std::is_move_constructible_v<GpioEdgeFuture>);

// ---------------------------------------------------------------------------
// GpioPin tests
//
// Note: wait_for_edge()/wait_for_level() broadcast off one shared IsrEvent
// per pin and do not filter a resumed waiter by which specific event_mask
// bit fired -- see the NOTE(race) in GpioPin::notify_irq() and
// doc/design/gpio_pin.md. This is permanent, documented behavior, not a
// gap expected to be filtered later.
// ---------------------------------------------------------------------------

class GpioPinTest : public testing::Test {
protected:
    void SetUp() override { gpio_stub::reset(); }
};

TEST_F(GpioPinTest, ConstructsWithoutThrowing) {
    GpioPin pin(2, Direction::In, Pull::Up);
    EXPECT_EQ(pin.pin(), 2u);
}

TEST_F(GpioPinTest, ReadReflectsStubbedLevel) {
    GpioPin pin(3, Direction::In);
    gpio_stub::set_level(3, true);
    EXPECT_TRUE(pin.read());
    gpio_stub::set_level(3, false);
    EXPECT_FALSE(pin.read());
}

TEST_F(GpioPinTest, WriteSetsStubbedLevel) {
    GpioPin pin(4, Direction::Out);
    pin.write(true);
    EXPECT_TRUE(pin.read());
    pin.write(false);
    EXPECT_FALSE(pin.read());
}

TEST_F(GpioPinTest, WaitForEdgeResumesWhenIrqFires) {
    GpioPin pin(5, Direction::In);
    bool completed = false;

    auto trigger = fire_irq_after(5, /*new_level=*/true, GPIO_IRQ_EDGE_RISE);

    make_rt().block_on([](GpioPin& pin, bool& done) -> Coro<void> {
        co_await pin.wait_for_edge(Edge::Rising);
        done = true;
    }(pin, completed));

    trigger.join();
    EXPECT_TRUE(completed);
}

TEST_F(GpioPinTest, WaitForLevelReturnsImmediatelyWhenAlreadyAtLevel) {
    GpioPin pin(6, Direction::In);
    gpio_stub::set_level(6, true);
    bool completed = false;

    make_rt().block_on([](GpioPin& pin, bool& done) -> Coro<void> {
        co_await pin.wait_for_high();
        done = true;
    }(pin, completed));

    EXPECT_TRUE(completed);
}

TEST_F(GpioPinTest, WaitForLevelResumesWhenIrqFires) {
    GpioPin pin(7, Direction::In);
    bool completed = false;

    auto trigger = fire_irq_after(7, /*new_level=*/true, GPIO_IRQ_LEVEL_HIGH);

    make_rt().block_on([](GpioPin& pin, bool& done) -> Coro<void> {
        co_await pin.wait_for_high();
        done = true;
    }(pin, completed));

    trigger.join();
    EXPECT_TRUE(completed);
}

TEST_F(GpioPinTest, EdgesStreamYieldsOnEachIrq) {
    GpioPin pin(8, Direction::In);
    int count = 0;

    auto trigger = std::thread([]() {
        for (int i = 0; i < 3; ++i) {
            std::this_thread::sleep_for(5ms);
            gpio_stub::set_level(8, i % 2 == 0);
            coro_pico_hal_gpio_fire_irq(8, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL);
        }
    });

    make_rt().block_on([](GpioPin& pin, int& count) -> Coro<void> {
        auto stream = pin.edges(Edge::Any);
        for (int i = 0; i < 3; ++i) {
            auto item = co_await next(stream);
            CO_ASSERT(item.has_value());
            ++count;
        }
    }(pin, count));

    trigger.join();
    EXPECT_EQ(count, 3);
}

TEST_F(GpioPinTest, TwoInstancesOnDifferentPinsAreIndependent) {
    GpioPin a(9, Direction::In);
    GpioPin b(10, Direction::In);
    EXPECT_NE(a.pin(), b.pin());
}

TEST_F(GpioPinTest, ConstructingOnAlreadyClaimedPinThrows) {
    GpioPin a(11, Direction::In);
    EXPECT_THROW((GpioPin(11, Direction::In)), std::logic_error);
}

TEST_F(GpioPinTest, PinCanBeReclaimedAfterOwnerDestructed) {
    {
        GpioPin a(12, Direction::In);
    }
    EXPECT_NO_THROW((GpioPin(12, Direction::In)));
}

// ---------------------------------------------------------------------------
// GpioEdgeFuture: refcounted arm/disarm
// ---------------------------------------------------------------------------

TEST_F(GpioPinTest, ConstructingFutureArmsIrqBitBeforeAnyAwait) {
    GpioPin pin(13, Direction::In);

    // Not yet awaited -- construction alone must have armed the bit.
    auto edge_future = pin.wait_for_edge(Edge::Rising);
    EXPECT_EQ(gpio_stub::enabled_mask(13), GPIO_IRQ_EDGE_RISE);
    EXPECT_EQ(gpio_stub::irq_enable_call_count(13), 1);
}

TEST_F(GpioPinTest, DestroyingFutureBeforeResolutionDisarmsIrqBit) {
    GpioPin pin(14, Direction::In);

    {
        auto edge_future = pin.wait_for_edge(Edge::Falling);
        EXPECT_EQ(gpio_stub::enabled_mask(14), GPIO_IRQ_EDGE_FALL);
    }
    EXPECT_EQ(gpio_stub::enabled_mask(14), 0u);
    EXPECT_EQ(gpio_stub::irq_enable_call_count(14), 2); // one enable, one disable
}

TEST_F(GpioPinTest, ConcurrentFuturesOnSameBitStayArmedUntilBothRelease) {
    GpioPin pin(15, Direction::In);

    auto first = pin.wait_for_edge(Edge::Rising);
    EXPECT_EQ(gpio_stub::irq_enable_call_count(15), 1); // 0 -> 1 transition enables

    {
        auto second = pin.wait_for_edge(Edge::Rising);
        EXPECT_EQ(gpio_stub::irq_enable_call_count(15), 1); // already armed, no-op
        EXPECT_EQ(gpio_stub::enabled_mask(15), GPIO_IRQ_EDGE_RISE);
    }
    // second released but first is still outstanding -- bit must stay armed.
    EXPECT_EQ(gpio_stub::enabled_mask(15), GPIO_IRQ_EDGE_RISE);
    EXPECT_EQ(gpio_stub::irq_enable_call_count(15), 1);
}

TEST_F(GpioPinTest, AlreadySatisfiedLevelWaitNeverArmsAnything) {
    GpioPin pin(16, Direction::In);
    gpio_stub::set_level(16, true);

    auto level_future = pin.wait_for_high();
    EXPECT_EQ(gpio_stub::enabled_mask(16), 0u);
    EXPECT_EQ(gpio_stub::irq_enable_call_count(16), 0);
}

// Level IRQs are not cleared by acknowledge (unlike edge IRQs) -- they stay
// pending for as long as the physical level holds, per hardware/gpio.h's
// gpio_acknowledge_irq() doc comment. If notify_irq() didn't disable the bit
// itself, this would storm the ISR forever on real hardware instead of
// giving the executor a chance to resume and disarm it the normal way. The
// stub can't reproduce a real re-entrant storm, but it can confirm the bit
// actually comes back off immediately after one simulated firing -- which is
// the mechanism the fix relies on.
TEST_F(GpioPinTest, LevelIrqIsDisabledImmediatelyOnFiringNotOnlyOnRelease) {
    GpioPin pin(20, Direction::In);

    auto level_future = pin.wait_for_low();
    EXPECT_EQ(gpio_stub::enabled_mask(20), GPIO_IRQ_LEVEL_LOW);

    gpio_stub::set_level(20, false);
    coro_pico_hal_gpio_fire_irq(20, GPIO_IRQ_LEVEL_LOW);

    // Disarmed by notify_irq() itself, before the future has resolved or
    // been destroyed -- release_mask() hasn't run yet at this point.
    EXPECT_EQ(gpio_stub::enabled_mask(20), 0u);
}

TEST_F(GpioPinTest, EdgeIrqStaysArmedAfterFiringUntilFutureReleases) {
    GpioPin pin(21, Direction::In);

    auto edge_future = pin.wait_for_edge(Edge::Rising);
    EXPECT_EQ(gpio_stub::enabled_mask(21), GPIO_IRQ_EDGE_RISE);

    gpio_stub::set_level(21, true);
    coro_pico_hal_gpio_fire_irq(21, GPIO_IRQ_EDGE_RISE);

    // Unlike the level case, edge IRQs don't need notify_irq() to disarm
    // them -- the bit stays armed until the future itself releases it.
    EXPECT_EQ(gpio_stub::enabled_mask(21), GPIO_IRQ_EDGE_RISE);
}

// ---------------------------------------------------------------------------
// GpioEdgeFuture: eager-arm-then-defer-await pattern
// ---------------------------------------------------------------------------

TEST_F(GpioPinTest, FutureIsArmedBeforeCoAwaitAndResumesOnLatchedEdge) {
    GpioPin pin(17, Direction::In);
    bool armed_before_await = false;
    bool completed = false;

    make_rt().block_on([](GpioPin& pin, bool& armed_before_await,
                          bool& done) -> Coro<void> {
        // Store the future without awaiting it yet -- the whole point of
        // GpioEdgeFuture is that arming already happened here.
        auto edge_future = pin.wait_for_edge(Edge::Rising);
        armed_before_await = (gpio_stub::enabled_mask(pin.pin()) & GPIO_IRQ_EDGE_RISE) != 0;

        // Simulate a synchronous triggering action happening between arm
        // and await, then fire the IRQ as if it happened during that gap.
        gpio_stub::set_level(pin.pin(), true);
        coro_pico_hal_gpio_fire_irq(pin.pin(), GPIO_IRQ_EDGE_RISE);

        co_await edge_future;
        done = true;
    }(pin, armed_before_await, completed));

    EXPECT_TRUE(armed_before_await);
    EXPECT_TRUE(completed);
}

// ---------------------------------------------------------------------------
// GpioEdgeFuture: move semantics
// ---------------------------------------------------------------------------

TEST_F(GpioPinTest, MovedFromFutureDoesNotDoubleReleaseOnDestruction) {
    GpioPin pin(18, Direction::In);

    {
        auto original = pin.wait_for_edge(Edge::Rising);
        EXPECT_EQ(gpio_stub::enabled_mask(18), GPIO_IRQ_EDGE_RISE);

        GpioEdgeFuture moved_to(std::move(original));
        // original is now moved-from; destroying it below must be a no-op,
        // not an extra release of a bit `moved_to` still owns.
    }
    // Only one release should have happened, from moved_to's destructor.
    EXPECT_EQ(gpio_stub::enabled_mask(18), 0u);
    EXPECT_EQ(gpio_stub::irq_enable_call_count(18), 2); // one enable, one disable
}

// ---------------------------------------------------------------------------
// edges(): participates in the same refcount as GpioEdgeFuture
// ---------------------------------------------------------------------------

TEST_F(GpioPinTest, EdgesStreamArmsAndDisarmsThroughSameRefcount) {
    GpioPin pin(19, Direction::In);
    uint32_t mask_while_active = 0;

    make_rt().block_on([](GpioPin& pin, uint32_t& mask_while_active) -> Coro<void> {
        auto stream = pin.edges(Edge::Any);

        auto trigger = std::thread([&pin]() {
            std::this_thread::sleep_for(5ms);
            gpio_stub::set_level(pin.pin(), true);
            coro_pico_hal_gpio_fire_irq(pin.pin(), GPIO_IRQ_EDGE_RISE);
        });
        auto item = co_await next(stream);
        CO_ASSERT(item.has_value());
        mask_while_active = gpio_stub::enabled_mask(pin.pin());
        trigger.join();
        // stream destructs here, releasing its refcount share.
    }(pin, mask_while_active));

    EXPECT_EQ(mask_while_active, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL);
    EXPECT_EQ(gpio_stub::enabled_mask(19), 0u);
}
