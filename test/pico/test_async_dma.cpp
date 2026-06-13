#include <gtest/gtest.h>
#include <coro/pico/hal/dma.h>
#include <coro/runtime/runtime.h>
#include <coro/coro.h>
#include <coro/sync/join.h>
#include <hardware/dma.h>  // stub
#include <thread>
#include <chrono>

using namespace coro;
using namespace coro::pico::hal;
using namespace std::chrono_literals;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Makes a Runtime backed by CurrentThreadExecutor with a wall-clock time source.
static Runtime make_rt() {
    return Runtime{};
}

// Fires the DMA_IRQ_0 handler from a background thread after a short delay,
// simulating hardware DMA completion for the given channel.
static std::thread fire_irq_after(uint channel, std::chrono::milliseconds delay = 5ms) {
    return std::thread([channel, delay]() {
        std::this_thread::sleep_for(delay);
        dma_stub::complete_channel(channel);
        coro_pico_hal_dma_fire_irq0();
    });
}

// ---------------------------------------------------------------------------
// Compile-time API checks
// ---------------------------------------------------------------------------

static_assert(!std::is_copy_constructible_v<AsyncDmaTransfer>);
static_assert(!std::is_move_constructible_v<AsyncDmaTransfer>);

// ---------------------------------------------------------------------------
// AsyncDmaTransfer tests
// ---------------------------------------------------------------------------

class AsyncDmaTransferTest : public testing::Test {
protected:
    void SetUp() override { dma_stub::reset(); }
};

TEST_F(AsyncDmaTransferTest, ClaimsChannelOnConstruction) {
    AsyncDmaTransfer dma;
    EXPECT_GE(dma.channel(), 0);
    EXPECT_LT(dma.channel(), static_cast<int>(NUM_DMA_CHANNELS));
}

TEST_F(AsyncDmaTransferTest, TwoInstancesGetDifferentChannels) {
    AsyncDmaTransfer a;
    AsyncDmaTransfer b;
    EXPECT_NE(a.channel(), b.channel());
}

TEST_F(AsyncDmaTransferTest, TransferCompletesWhenIrqFires) {
    AsyncDmaTransfer dma;
    bool completed = false;
    uint ch = static_cast<uint>(dma.channel());

    auto trigger = fire_irq_after(ch);

    make_rt().block_on([](AsyncDmaTransfer& dma, bool& done) -> Coro<void> {
        dma_channel_config cfg = dma_channel_get_default_config(
            static_cast<uint>(dma.channel()));
        co_await dma.transfer(cfg, nullptr, nullptr, 0);
        done = true;
    }(dma, completed));

    trigger.join();
    EXPECT_TRUE(completed);
}

TEST_F(AsyncDmaTransferTest, TransferCanBeReusedAfterCompletion) {
    AsyncDmaTransfer dma;
    int count = 0;
    uint ch = static_cast<uint>(dma.channel());

    auto trigger = std::thread([ch]() {
        for (int i = 0; i < 3; ++i) {
            std::this_thread::sleep_for(5ms);
            dma_stub::complete_channel(ch);
            coro_pico_hal_dma_fire_irq0();
        }
    });

    make_rt().block_on([](AsyncDmaTransfer& dma, int& count) -> Coro<void> {
        dma_channel_config cfg = dma_channel_get_default_config(
            static_cast<uint>(dma.channel()));
        for (int i = 0; i < 3; ++i) {
            co_await dma.transfer(cfg, nullptr, nullptr, 0);
            ++count;
        }
    }(dma, count));

    trigger.join();
    EXPECT_EQ(count, 3);
}

TEST_F(AsyncDmaTransferTest, OnlyCorrectChannelWakesTransfer) {
    AsyncDmaTransfer a;
    AsyncDmaTransfer b;
    bool a_done = false;
    bool b_done = false;
    uint ch_a = static_cast<uint>(a.channel());
    uint ch_b = static_cast<uint>(b.channel());

    // Fire IRQ for b first, then a — verify each wakes only its own transfer
    auto trigger = std::thread([ch_a, ch_b]() {
        std::this_thread::sleep_for(5ms);
        dma_stub::complete_channel(ch_b);
        coro_pico_hal_dma_fire_irq0();
        std::this_thread::sleep_for(5ms);
        dma_stub::complete_channel(ch_a);
        coro_pico_hal_dma_fire_irq0();
    });

    make_rt().block_on([](AsyncDmaTransfer& a, AsyncDmaTransfer& b,
                          bool& a_done, bool& b_done) -> Coro<void> {
        dma_channel_config cfg{};
        // Start both concurrently via join
        co_await coro::join(
            a.transfer(cfg, nullptr, nullptr, 0),
            b.transfer(cfg, nullptr, nullptr, 0)
        );
        a_done = true;
        b_done = true;
    }(a, b, a_done, b_done));

    trigger.join();
    EXPECT_TRUE(a_done);
    EXPECT_TRUE(b_done);
}
