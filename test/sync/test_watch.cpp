#include <gtest/gtest.h>
#include "executor_traits.h"
#include <coro/coro.h>
#include <coro/co_invoke.h>
#include <coro/sync/watch.h>
#include <coro/future.h>
#include <coro/runtime/runtime.h>

using namespace coro;

static_assert(Future<WatchChangedFuture<int>>);

template<typename Traits>
class WatchTest : public testing::Test {
protected:
    Traits traits;
};
TYPED_TEST_SUITE(WatchTest, AllExecutors);

// --- Construction / synchronous API ---

TYPED_TEST(WatchTest, ChannelReturnsLinkedPair) {
    auto [tx, rx] = watch_channel<int>(0);
    (void)tx; (void)rx;
}

TYPED_TEST(WatchTest, SenderIsMovable) {
    auto [tx, rx] = watch_channel<int>(0);
    auto tx2 = std::move(tx);
    (void)tx2; (void)rx;
}

TYPED_TEST(WatchTest, ReceiverIsMovable) {
    auto [tx, rx] = watch_channel<int>(0);
    auto rx2 = std::move(rx);
    (void)tx; (void)rx2;
}

TYPED_TEST(WatchTest, ReceiverIsCloneable) {
    auto [tx, rx] = watch_channel<int>(0);
    auto rx2 = rx.clone();
    (void)tx; (void)rx2;
}

TYPED_TEST(WatchTest, SenderIsCloneable) {
    auto [tx, rx] = watch_channel<int>(0);
    auto tx2 = tx.clone();
    (void)tx2; (void)rx;
}

TYPED_TEST(WatchTest, ClonedSenderCanSend) {
    auto [tx, rx] = watch_channel<int>(0);
    auto tx2 = tx.clone();
    EXPECT_TRUE(tx2.send(99).has_value());
    EXPECT_EQ(*rx.borrow(), 99);
}

TYPED_TEST(WatchTest, ChannelRemainsOpenWhenOneSenderDropped) {
    auto [tx, rx] = watch_channel<int>(0);
    auto tx2 = tx.clone();
    { auto dropped = std::move(tx); }
    EXPECT_TRUE(tx2.send(42).has_value());
    EXPECT_EQ(*rx.borrow(), 42);
}

TYPED_TEST(WatchTest, ChannelClosedWhenAllSendersDropped) {
    auto [tx, rx] = watch_channel<int>(0);
    auto tx2 = tx.clone();
    { auto d1 = std::move(tx); }
    { auto d2 = std::move(tx2); }
    EXPECT_TRUE(rx.sender_dropped());
}

TYPED_TEST(WatchTest, ChangedErrorOnlyAfterAllSendersDropped) {
    this->traits.rt.block_on([]() -> Coro<void> {
        // GCC bug: structured bindings in coroutines leak when spanning a suspension.
        auto _ch = watch_channel<int>(0);
        auto tx = std::move(_ch.first);
        auto rx = std::move(_ch.second);
        auto tx2 = tx.clone();
        { auto dropped = std::move(tx); }
        tx2.send(1);
        auto r1 = co_await rx.changed();
        EXPECT_TRUE(r1.has_value());
        { auto dropped = std::move(tx2); }
        auto r2 = co_await rx.changed();
        EXPECT_FALSE(r2.has_value());
        EXPECT_EQ(r2.error(), ChannelError::SenderDropped);
    }());
}

TYPED_TEST(WatchTest, SendSucceedsWithLiveReceiver) {
    auto [tx, rx] = watch_channel<int>(0);
    auto r = tx.send(42);
    EXPECT_TRUE(r.has_value());
    (void)rx;
}

TYPED_TEST(WatchTest, SendReturnsValueWhenAllReceiversDropped) {
    auto [tx, rx] = watch_channel<int>(0);
    { auto dropped = std::move(rx); }
    auto r = tx.send(42);
    EXPECT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), 42);
}

// Move-assigning into a live WatchSender/WatchReceiver must decrement the
// OLD channel's sender_count/receiver_count (running the destructor's
// protocol on it), not just silently drop the old Rc. Regression test for a
// defaulted move-assignment bug found in MpscSender and fixed analogously here.
TYPED_TEST(WatchTest, ReassigningLiveSenderDropsOldChannelSender) {
    auto chan_a = watch_channel<int>(0);
    auto tx_a   = std::move(chan_a.first);
    auto rx_a   = std::move(chan_a.second);
    auto chan_b = watch_channel<int>(0);
    auto tx_b   = std::move(chan_b.first);

    tx_a = std::move(tx_b); // displaces the sender for channel A

    EXPECT_TRUE(rx_a.sender_dropped());
}

TYPED_TEST(WatchTest, ReassigningLiveReceiverDropsOldChannelReceiver) {
    auto chan_a = watch_channel<int>(0);
    auto tx_a   = std::move(chan_a.first);
    auto rx_a   = std::move(chan_a.second);
    auto chan_b = watch_channel<int>(0);
    auto rx_b   = std::move(chan_b.second);

    rx_a = std::move(rx_b); // displaces the receiver for channel A

    EXPECT_TRUE(tx_a.all_receivers_dropped());
    auto r = tx_a.send(42);
    EXPECT_FALSE(r.has_value());
}

TYPED_TEST(WatchTest, BorrowReadsInitialValue) {
    auto [tx, rx] = watch_channel<int>(7);
    EXPECT_EQ(*rx.borrow(), 7);
    (void)tx;
}

TYPED_TEST(WatchTest, BorrowReflectsLatestSend) {
    auto [tx, rx] = watch_channel<int>(0);
    tx.send(99);
    EXPECT_EQ(*rx.borrow(), 99);
}

TYPED_TEST(WatchTest, BorrowArrowOperator) {
    struct Point { int x, y; };
    auto [tx, rx] = watch_channel<Point>({1, 2});
    auto guard = rx.borrow();
    EXPECT_EQ(guard->x, 1);
    EXPECT_EQ(guard->y, 2);
    (void)tx;
}

TYPED_TEST(WatchTest, ClonesAreIndependent) {
    auto [tx, rx] = watch_channel<int>(0);
    auto rx2 = rx.clone();
    tx.send(10);
    EXPECT_EQ(*rx.borrow(),  10);
    EXPECT_EQ(*rx2.borrow(), 10);
}

TYPED_TEST(WatchTest, MultipleReceiversSeeLatestValue) {
    auto [tx, rx] = watch_channel<int>(0);
    auto rx2 = rx.clone();
    auto rx3 = rx.clone();
    tx.send(55);
    EXPECT_EQ(*rx.borrow(),  55);
    EXPECT_EQ(*rx2.borrow(), 55);
    EXPECT_EQ(*rx3.borrow(), 55);
}

// --- Async ---

TYPED_TEST(WatchTest, ChangedResolvesWhenValueSent) {
    this->traits.rt.block_on([]() -> Coro<void> {
        auto _ch = watch_channel<int>(0);
        auto tx = std::move(_ch.first);
        auto rx = std::move(_ch.second);
        co_await coro::spawn(coro::co_invoke([tx = std::move(tx)]() mutable -> Coro<void> {
            tx.send(42);
            co_return;
        }));
        auto r = co_await rx.changed();
        EXPECT_TRUE(r.has_value());
        EXPECT_EQ(*rx.borrow(), 42);
    }());
}

TYPED_TEST(WatchTest, ChangedResolvesImmediatelyIfAlreadyChanged) {
    this->traits.rt.block_on([]() -> Coro<void> {
        auto _ch = watch_channel<int>(0);
        auto tx = std::move(_ch.first);
        auto rx = std::move(_ch.second);
        tx.send(1);
        auto r = co_await rx.changed();
        EXPECT_TRUE(r.has_value());
    }());
}

TYPED_TEST(WatchTest, ChangedErrorWhenSenderDropped) {
    this->traits.rt.block_on([]() -> Coro<void> {
        auto _ch = watch_channel<int>(0);
        auto tx = std::move(_ch.first);
        auto rx = std::move(_ch.second);
        { auto dropped = std::move(tx); }
        auto r = co_await rx.changed();
        EXPECT_FALSE(r.has_value());
        EXPECT_EQ(r.error(), ChannelError::SenderDropped);
    }());
}

TYPED_TEST(WatchTest, MultipleReceiversWokenOnSend) {
    int count = 0;
    this->traits.rt.block_on([](int& count) -> Coro<void> {
        auto _ch = watch_channel<int>(0);
        auto tx  = std::move(_ch.first);
        auto rx1 = std::move(_ch.second);
        auto rx2 = rx1.clone();
        auto rx3 = rx1.clone();
        tx.send(1);
        if ((co_await rx1.changed()).has_value()) ++count;
        if ((co_await rx2.changed()).has_value()) ++count;
        if ((co_await rx3.changed()).has_value()) ++count;
    }(count));
    EXPECT_EQ(count, 3);
}

// --- borrow_and_update ---

TYPED_TEST(WatchTest, BorrowAndUpdateReadsCurrentValue) {
    auto [tx, rx] = watch_channel<int>(7);
    tx.send(42);
    EXPECT_EQ(*rx.borrow_and_update(), 42);
}

TYPED_TEST(WatchTest, BorrowAndUpdateMarksVersionSeen) {
    this->traits.rt.block_on([]() -> Coro<void> {
        auto _ch = watch_channel<int>(0);
        auto tx = std::move(_ch.first);
        auto rx = std::move(_ch.second);
        tx.send(1);
        { auto guard = rx.borrow_and_update(); (void)guard; }
        spawn(co_invoke([tx = std::move(tx)]() mutable -> Coro<void> {
            tx.send(2);
            co_return;
        })).detach();
        auto r = co_await rx.changed();
        EXPECT_TRUE(r.has_value());
        EXPECT_EQ(*rx.borrow(), 2);
    }());
}

TYPED_TEST(WatchTest, BorrowWithoutUpdateStillSeesExistingChange) {
    this->traits.rt.block_on([]() -> Coro<void> {
        auto _ch = watch_channel<int>(0);
        auto tx = std::move(_ch.first);
        auto rx = std::move(_ch.second);
        tx.send(1);
        { auto guard = rx.borrow(); (void)guard; }
        auto r = co_await rx.changed();
        EXPECT_TRUE(r.has_value());
    }());
}

// --- send_if_modified ---

TYPED_TEST(WatchTest, SendIfModifiedReturnsTrueWhenModified) {
    auto [tx, rx] = watch_channel<int>(0);
    bool result = tx.send_if_modified([](int& v) { v = 42; return true; });
    EXPECT_TRUE(result);
    EXPECT_EQ(*rx.borrow(), 42);
}

TYPED_TEST(WatchTest, SendIfModifiedReturnsFalseWhenNotModified) {
    auto [tx, rx] = watch_channel<int>(0);
    bool result = tx.send_if_modified([](int&) { return false; });
    EXPECT_FALSE(result);
    EXPECT_EQ(*rx.borrow(), 0);
}

TYPED_TEST(WatchTest, SendIfModifiedDoesNotWakeReceiversWhenNotModified) {
    this->traits.rt.block_on([]() -> Coro<void> {
        auto _ch = watch_channel<int>(0);
        auto tx = std::move(_ch.first);
        auto rx = std::move(_ch.second);
        tx.send_if_modified([](int&) { return false; });
        spawn(co_invoke([tx = std::move(tx)]() mutable -> Coro<void> {
            tx.send(99);
            co_return;
        })).detach();
        auto r = co_await rx.changed();
        EXPECT_TRUE(r.has_value());
        EXPECT_EQ(*rx.borrow(), 99);
    }());
}

TYPED_TEST(WatchTest, SendIfModifiedWakesReceiversWhenModified) {
    this->traits.rt.block_on([]() -> Coro<void> {
        auto _ch = watch_channel<int>(0);
        auto tx = std::move(_ch.first);
        auto rx = std::move(_ch.second);
        spawn(co_invoke([tx = std::move(tx)]() mutable -> Coro<void> {
            tx.send_if_modified([](int& v) { v = 7; return true; });
            co_return;
        })).detach();
        auto r = co_await rx.changed();
        EXPECT_TRUE(r.has_value());
        EXPECT_EQ(*rx.borrow(), 7);
    }());
}

// --- borrow_mut ---

TYPED_TEST(WatchTest, BorrowMutProvidesWriteAccess) {
    auto [tx, rx] = watch_channel<int>(0);
    { auto guard = tx.borrow_mut(); *guard = 99; }
    EXPECT_EQ(*rx.borrow(), 99);
}

TYPED_TEST(WatchTest, BorrowMutArrowOperator) {
    struct Point { int x, y; };
    auto [tx, rx] = watch_channel<Point>({0, 0});
    { auto guard = tx.borrow_mut(); guard->x = 3; guard->y = 7; }
    auto g = rx.borrow();
    EXPECT_EQ(g->x, 3);
    EXPECT_EQ(g->y, 7);
}

TYPED_TEST(WatchTest, BorrowMutIncrementsVersion) {
    this->traits.rt.block_on([]() -> Coro<void> {
        auto _ch = watch_channel<int>(0);
        auto tx = std::move(_ch.first);
        auto rx = std::move(_ch.second);
        { auto guard = tx.borrow_mut(); *guard = 5; }
        auto r = co_await rx.changed();
        EXPECT_TRUE(r.has_value());
        EXPECT_EQ(*rx.borrow(), 5);
    }());
}

TYPED_TEST(WatchTest, BorrowMutNotifiesMultipleReceivers) {
    int count = 0;
    this->traits.rt.block_on([](int& count) -> Coro<void> {
        auto _ch = watch_channel<int>(0);
        auto tx  = std::move(_ch.first);
        auto rx1 = std::move(_ch.second);
        auto rx2 = rx1.clone();
        auto rx3 = rx1.clone();
        { auto guard = tx.borrow_mut(); *guard = 42; }
        if ((co_await rx1.changed()).has_value()) ++count;
        if ((co_await rx2.changed()).has_value()) ++count;
        if ((co_await rx3.changed()).has_value()) ++count;
    }(count));
    EXPECT_EQ(count, 3);
}

TYPED_TEST(WatchTest, BorrowMutWakesWaitingReceivers) {
    this->traits.rt.block_on([]() -> Coro<void> {
        auto _ch = watch_channel<int>(0);
        auto tx = std::move(_ch.first);
        auto rx = std::move(_ch.second);
        spawn(co_invoke([tx = std::move(tx)]() mutable -> Coro<void> {
            auto guard = tx.borrow_mut();
            *guard = 77;
            co_return;
        })).detach();
        auto r = co_await rx.changed();
        EXPECT_TRUE(r.has_value());
        EXPECT_EQ(*rx.borrow(), 77);
    }());
}

TYPED_TEST(WatchTest, LastWriteWins) {
    this->traits.rt.block_on([]() -> Coro<void> {
        auto _ch = watch_channel<int>(0);
        auto tx = std::move(_ch.first);
        auto rx = std::move(_ch.second);
        tx.send(1); tx.send(2); tx.send(3);
        co_await rx.changed();
        EXPECT_EQ(*rx.borrow(), 3);
    }());
}
