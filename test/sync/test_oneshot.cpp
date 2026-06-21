#include <gtest/gtest.h>
#include "executor_traits.h"
#include <coro/coro.h>
#include <coro/sync/oneshot.h>
#include <coro/sync/select.h>
#include <coro/future.h>
#include <coro/runtime/runtime.h>
#include <memory>
#include <string>

#ifndef CORO_PICO
#include <coro/task/spawn_blocking.h>
#endif

using namespace coro;

static_assert(Future<OneshotRecvFuture<int>>);
static_assert(Future<OneshotRecvFuture<std::unique_ptr<int>>>);
static_assert(Future<OneshotRecvFuture<void>>);

// ---------------------------------------------------------------------------
// OneshotTest — all executors
// ---------------------------------------------------------------------------

template<typename Traits>
class OneshotTest : public testing::Test {
protected:
    Traits traits;
};
TYPED_TEST_SUITE(OneshotTest, AllExecutors);

TYPED_TEST(OneshotTest, ChannelReturnsLinkedPair) {
    auto [tx, rx] = oneshot_channel<int>();
    (void)tx; (void)rx;
}

TYPED_TEST(OneshotTest, SenderIsMovable) {
    auto [tx, rx] = oneshot_channel<int>();
    auto tx2 = std::move(tx);
    (void)tx2; (void)rx;
}

TYPED_TEST(OneshotTest, ReceiverIsMovable) {
    auto [tx, rx] = oneshot_channel<int>();
    auto rx2 = std::move(rx);
    (void)tx; (void)rx2;
}

TYPED_TEST(OneshotTest, SendReturnsSuccessWhenReceiverAlive) {
    auto [tx, rx] = oneshot_channel<int>();
    EXPECT_TRUE(tx.send(42).has_value());
    (void)rx;
}

TYPED_TEST(OneshotTest, SendReturnsValueWhenReceiverDropped) {
    auto [tx, rx] = oneshot_channel<int>();
    { auto dropped = std::move(rx); }
    auto result = tx.send(42);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), 42);
}

TYPED_TEST(OneshotTest, SenderDropWithoutSendCloseChannel) {
    auto [tx, rx] = oneshot_channel<int>();
    { auto dropped = std::move(tx); }
    (void)rx;
}

TYPED_TEST(OneshotTest, SendAcceptsMoveOnlyType) {
    auto [tx, rx] = oneshot_channel<std::unique_ptr<int>>();
    EXPECT_TRUE(tx.send(std::make_unique<int>(99)).has_value());
    (void)rx;
}

TYPED_TEST(OneshotTest, FailedSendReturnsMoveOnlyValue) {
    auto [tx, rx] = oneshot_channel<std::unique_ptr<int>>();
    { auto dropped = std::move(rx); }
    auto result = tx.send(std::make_unique<int>(99));
    EXPECT_FALSE(result.has_value());
    EXPECT_NE(result.error(), nullptr);
    EXPECT_EQ(*result.error(), 99);
}

TYPED_TEST(OneshotTest, ReceiverGetsValueAfterSend) {
    std::optional<int> received;
    this->traits.rt.block_on([](std::optional<int>& received) -> Coro<void> {
        auto [tx, rx] = oneshot_channel<int>();
        tx.send(42);
        auto result = co_await rx.recv();
        EXPECT_TRUE(result.has_value());
        received = *result;
    }(received));
    EXPECT_EQ(received, 42);
}

TYPED_TEST(OneshotTest, ReceiverErrorWhenSenderDropped) {
    this->traits.rt.block_on([]() -> Coro<void> {
        auto [tx, rx] = oneshot_channel<int>();
        { auto dropped = std::move(tx); }
        auto result = co_await rx.recv();
        EXPECT_FALSE(result.has_value());
        EXPECT_EQ(result.error(), ChannelError::Closed);
    }());
}

TYPED_TEST(OneshotTest, ReceiverSuspendsUntilSend) {
    this->traits.rt.block_on([]() -> Coro<void> {
        auto [tx, rx] = oneshot_channel<int>();
        co_await coro::spawn([](OneshotSender<int> tx) -> Coro<void> {
            tx.send(7);
            co_return;
        }(std::move(tx)));
        auto result = co_await rx.recv();
        EXPECT_TRUE(result.has_value());
        EXPECT_EQ(*result, 7);
    }());
}

TYPED_TEST(OneshotTest, RecvCanBeReusedAfterCancelledSelect) {
    this->traits.rt.block_on([]() -> Coro<void> {
        auto [tx, rx] = oneshot_channel<int>();
        struct ImmediateInt {
            using OutputType = int;
            PollResult<int> poll(detail::Context&) { return 99; }
        };
        auto r = co_await select(rx.recv(), ImmediateInt{});
        EXPECT_EQ(r.index(), 1u);
        tx.send(42);
        auto result = co_await rx.recv();
        EXPECT_TRUE(result.has_value());
        EXPECT_EQ(*result, 42);
    }());
}

TYPED_TEST(OneshotTest, SecondRecvReturnsClosedInt) {
    this->traits.rt.block_on([]() -> Coro<void> {
        auto [tx, rx] = oneshot_channel<int>();
        tx.send(42);
        auto r1 = co_await rx.recv();
        EXPECT_TRUE(r1.has_value());
        EXPECT_EQ(*r1, 42);
        auto r2 = co_await rx.recv();
        EXPECT_FALSE(r2.has_value());
        EXPECT_EQ(r2.error(), ChannelError::Closed);
    }());
}

TYPED_TEST(OneshotTest, SecondRecvReturnsClosedString) {
    this->traits.rt.block_on([]() -> Coro<void> {
        auto [tx, rx] = oneshot_channel<std::string>();
        tx.send("hello");
        auto r1 = co_await rx.recv();
        EXPECT_TRUE(r1.has_value());
        EXPECT_EQ(*r1, "hello");
        auto r2 = co_await rx.recv();
        EXPECT_FALSE(r2.has_value());
        EXPECT_EQ(r2.error(), ChannelError::Closed);
    }());
}


TYPED_TEST(OneshotTest, SenderDroppedReturnsFalseWhenSenderAlive) {
    auto [tx, rx] = oneshot_channel<int>();
    EXPECT_FALSE(rx.sender_dropped());
    (void)tx;
}

TYPED_TEST(OneshotTest, SenderDroppedReturnsTrueAfterDropWithoutSend) {
    auto [tx, rx] = oneshot_channel<int>();
    { auto dropped = std::move(tx); }
    EXPECT_TRUE(rx.sender_dropped());
}

TYPED_TEST(OneshotTest, SenderDroppedReturnsFalseAfterSend) {
    auto [tx, rx] = oneshot_channel<int>();
    tx.send(42);
    EXPECT_FALSE(rx.sender_dropped());
}

TYPED_TEST(OneshotTest, SenderDroppedReturnsTrueAfterValueConsumed) {
    bool dropped_after_recv = false;
    this->traits.rt.block_on([](bool& out) -> Coro<void> {
        auto [tx, rx] = oneshot_channel<int>();
        tx.send(99);
        co_await rx.recv();
        out = rx.sender_dropped();
    }(dropped_after_recv));
    EXPECT_TRUE(dropped_after_recv);
}

// Move-assigning into a live OneshotSender/OneshotReceiver must close the OLD
// channel (run the destructor's alive-flag-clear-and-wake protocol on it),
// not just silently drop the old Rc. Regression test for a defaulted
// move-assignment bug found in MpscSender and fixed analogously here.
TYPED_TEST(OneshotTest, ReassigningLiveSenderClosesOldChannel) {
    bool old_channel_closed = false;
    this->traits.rt.block_on([](bool& out) -> Coro<void> {
        auto chan_a = oneshot_channel<int>();
        auto tx_a   = std::move(chan_a.first);
        auto rx_a   = std::move(chan_a.second);
        auto chan_b = oneshot_channel<int>();
        auto tx_b   = std::move(chan_b.first);

        tx_a = std::move(tx_b); // displaces the sender for channel A

        auto result = co_await rx_a.recv();
        out = !result.has_value() && result.error() == ChannelError::Closed;
    }(old_channel_closed));
    EXPECT_TRUE(old_channel_closed);
}

TYPED_TEST(OneshotTest, ReassigningLiveReceiverDropsOldChannelReceiver) {
    auto chan_a = oneshot_channel<int>();
    auto tx_a   = std::move(chan_a.first);
    auto rx_a   = std::move(chan_a.second);
    auto chan_b = oneshot_channel<int>();
    auto rx_b   = std::move(chan_b.second);

    rx_a = std::move(rx_b); // displaces the receiver for channel A

    auto result = tx_a.send(42);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), 42);
}

// ---------------------------------------------------------------------------
// OneshotVoidTest — all executors
// ---------------------------------------------------------------------------

template<typename Traits>
class OneshotVoidTest : public testing::Test {
protected:
    Traits traits;
};
TYPED_TEST_SUITE(OneshotVoidTest, AllExecutors);

TYPED_TEST(OneshotVoidTest, ChannelReturnsLinkedPair) {
    auto [tx, rx] = oneshot_channel<void>();
    (void)tx; (void)rx;
}

TYPED_TEST(OneshotVoidTest, SendReturnsSuccessWhenReceiverAlive) {
    auto [tx, rx] = oneshot_channel<void>();
    EXPECT_TRUE(tx.send().has_value());
    (void)rx;
}

TYPED_TEST(OneshotVoidTest, SendReturnsClosedWhenReceiverDropped) {
    auto [tx, rx] = oneshot_channel<void>();
    { auto dropped = std::move(rx); }
    auto result = tx.send();
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), ChannelError::Closed);
}

TYPED_TEST(OneshotVoidTest, ReceiverGetsSignalAfterSend) {
    this->traits.rt.block_on([]() -> Coro<void> {
        auto [tx, rx] = oneshot_channel<void>();
        tx.send();
        auto result = co_await rx.recv();
        EXPECT_TRUE(result.has_value());
    }());
}

TYPED_TEST(OneshotVoidTest, ReceiverErrorWhenSenderDropped) {
    this->traits.rt.block_on([]() -> Coro<void> {
        auto [tx, rx] = oneshot_channel<void>();
        { auto dropped = std::move(tx); }
        auto result = co_await rx.recv();
        EXPECT_FALSE(result.has_value());
        EXPECT_EQ(result.error(), ChannelError::Closed);
    }());
}

TYPED_TEST(OneshotVoidTest, ReceiverSuspendsUntilSend) {
    this->traits.rt.block_on([]() -> Coro<void> {
        auto [tx, rx] = oneshot_channel<void>();
        co_await coro::spawn([](OneshotSender<void> tx) -> Coro<void> {
            tx.send();
            co_return;
        }(std::move(tx)));
        auto result = co_await rx.recv();
        EXPECT_TRUE(result.has_value());
    }());
}

TYPED_TEST(OneshotVoidTest, SecondRecvReturnsClosed) {
    this->traits.rt.block_on([]() -> Coro<void> {
        auto [tx, rx] = oneshot_channel<void>();
        tx.send();
        auto r1 = co_await rx.recv();
        EXPECT_TRUE(r1.has_value());
        auto r2 = co_await rx.recv();
        EXPECT_FALSE(r2.has_value());
    }());
}

// ---------------------------------------------------------------------------
// spawn_blocking and direct blocking_recv tests — desktop only
// (blocking_recv uses cv.wait; on CORO_PICO that spins forever)
// ---------------------------------------------------------------------------

#ifndef CORO_PICO

TEST(OneshotBlockingTest, BlockingRecvTwiceSecondClosed) {
    auto [tx, rx] = oneshot_channel<int>();
    tx.send(99);
    auto r1 = rx.blocking_recv();
    EXPECT_TRUE(r1.has_value());
    EXPECT_EQ(*r1, 99);
    auto r2 = rx.blocking_recv();
    EXPECT_FALSE(r2.has_value());
}

TEST(OneshotBlockingTest, BlockingRecvTwiceClosedString) {
    auto [tx, rx] = oneshot_channel<std::string>();
    tx.send("world");
    auto r1 = rx.blocking_recv();
    EXPECT_TRUE(r1.has_value());
    EXPECT_EQ(*r1, "world");
    auto r2 = rx.blocking_recv();
    EXPECT_FALSE(r2.has_value());
}

TEST(OneshotBlockingTest, BlockingRecvGetsValue) {
    Runtime rt(1);
    int result = -1;
    rt.block_on([](int& out) -> Coro<void> {
        auto [tx, rx] = oneshot_channel<int>();
        tx.send(42);
        out = co_await coro::spawn_blocking(
            [rx = std::move(rx)]() mutable -> int {
                auto r = rx.blocking_recv();
                return r.has_value() ? *r : -1;
            });
    }(result));
    EXPECT_EQ(result, 42);
}

TEST(OneshotBlockingTest, BlockingRecvBlocksUntilSend) {
    Runtime rt(1);
    int result = -1;
    rt.block_on([](int& out) -> Coro<void> {
        auto [tx, rx] = oneshot_channel<int>();
        auto handle = coro::spawn_blocking(
            [rx = std::move(rx)]() mutable -> int {
                auto r = rx.blocking_recv();
                return r.has_value() ? *r : -1;
            });
        tx.send(99);
        out = co_await std::move(handle);
    }(result));
    EXPECT_EQ(result, 99);
}

TEST(OneshotBlockingTest, BlockingRecvReturnsClosedWhenSenderDropped) {
    Runtime rt(1);
    bool got_closed = false;
    rt.block_on([](bool& out) -> Coro<void> {
        auto [tx, rx] = oneshot_channel<int>();
        { auto dropped = std::move(tx); }
        out = co_await coro::spawn_blocking(
            [rx = std::move(rx)]() mutable -> bool {
                auto r = rx.blocking_recv();
                return !r.has_value() && r.error() == ChannelError::Closed;
            });
    }(got_closed));
    EXPECT_TRUE(got_closed);
}

TEST(OneshotBlockingTest, SenderDroppedReturnsTrueAfterValueConsumedBlocking) {
    Runtime rt(1);
    bool dropped_after_recv = false;
    rt.block_on([](bool& out) -> Coro<void> {
        auto [tx, rx] = oneshot_channel<int>();
        tx.send(99);
        co_await rx.recv();
        out = rx.sender_dropped();
    }(dropped_after_recv));
    EXPECT_TRUE(dropped_after_recv);
}

TEST(OneshotVoidBlockingTest, BlockingRecvGetsSignal) {
    Runtime rt(1);
    bool got_signal = false;
    rt.block_on([](bool& out) -> Coro<void> {
        auto [tx, rx] = oneshot_channel<void>();
        tx.send();
        out = co_await coro::spawn_blocking(
            [rx = std::move(rx)]() mutable -> bool {
                return rx.blocking_recv().has_value();
            });
    }(got_signal));
    EXPECT_TRUE(got_signal);
}

TEST(OneshotVoidBlockingTest, BlockingRecvReturnsClosedWhenSenderDropped) {
    Runtime rt(1);
    bool got_closed = false;
    rt.block_on([](bool& out) -> Coro<void> {
        auto [tx, rx] = oneshot_channel<void>();
        { auto dropped = std::move(tx); }
        out = co_await coro::spawn_blocking(
            [rx = std::move(rx)]() mutable -> bool {
                auto r = rx.blocking_recv();
                return !r.has_value() && r.error() == ChannelError::Closed;
            });
    }(got_closed));
    EXPECT_TRUE(got_closed);
}

#endif  // !CORO_PICO
