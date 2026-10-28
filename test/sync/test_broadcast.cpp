#include <gtest/gtest.h>
#include "executor_traits.h"
#include <coro/coro.h>
#include <coro/sync/broadcast.h>
#include <coro/sync/select.h>
#include <coro/future.h>
#include <coro/runtime/runtime.h>
#include <memory>
#include <string>
#include <vector>

using namespace coro;

static_assert(Future<BroadcastRecvFuture<int>>);
static_assert(Future<BroadcastReceiversActiveFuture<int>>);

template<typename Traits>
class BroadcastTest : public testing::Test {
protected:
    Traits traits;
};
TYPED_TEST_SUITE(BroadcastTest, AllExecutors);

// --- Construction / synchronous API ---

TYPED_TEST(BroadcastTest, ChannelReturnsLinkedPair) {
    auto [tx, rx] = broadcast_channel<int>(4);
    (void)tx; (void)rx;
}

TYPED_TEST(BroadcastTest, ZeroCapacityThrows) {
    EXPECT_THROW({ [[maybe_unused]] auto ch = broadcast_channel<int>(0); }, std::invalid_argument);
}

TYPED_TEST(BroadcastTest, SenderIsMovable) {
    auto [tx, rx] = broadcast_channel<int>(4);
    auto tx2 = std::move(tx);
    (void)tx2; (void)rx;
}

TYPED_TEST(BroadcastTest, ReceiverIsMovable) {
    auto [tx, rx] = broadcast_channel<int>(4);
    auto rx2 = std::move(rx);
    (void)tx; (void)rx2;
}

TYPED_TEST(BroadcastTest, SenderIsCloneable) {
    auto [tx, rx] = broadcast_channel<int>(4);
    auto tx2 = tx.clone();
    (void)tx2; (void)rx;
}

TYPED_TEST(BroadcastTest, SubscribeCreatesIndependentReceiver) {
    auto [tx, rx1] = broadcast_channel<int>(4);
    auto rx2 = tx.subscribe();
    (void)rx1; (void)rx2;
}

TYPED_TEST(BroadcastTest, ResubscribeCreatesIndependentReceiver) {
    auto [tx, rx1] = broadcast_channel<int>(4);
    auto rx2 = rx1.resubscribe();
    (void)tx; (void)rx2;
}

TYPED_TEST(BroadcastTest, SendSucceedsWithLiveReceiver) {
    auto [tx, rx] = broadcast_channel<int>(4);
    auto r = tx.send(42);
    EXPECT_TRUE(r.has_value());
    EXPECT_EQ(*r, 1u);
    (void)rx;
}

TYPED_TEST(BroadcastTest, SendNotifiesCountOfReceivers) {
    auto [tx, rx1] = broadcast_channel<int>(4);
    auto rx2 = tx.subscribe();
    auto rx3 = tx.subscribe();
    auto r = tx.send(1);
    EXPECT_TRUE(r.has_value());
    EXPECT_EQ(*r, 3u);
    (void)rx1; (void)rx2; (void)rx3;
}

TYPED_TEST(BroadcastTest, SendReturnsValueWhenNoReceivers) {
    auto [tx, rx] = broadcast_channel<int>(4);
    { auto dropped = std::move(rx); }
    auto r = tx.send(42);
    EXPECT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), 42);
}

// Dropping every receiver does not close the channel or invalidate the
// sender — a later subscribe() works exactly as it would have before any
// receiver ever existed.
TYPED_TEST(BroadcastTest, SubscribingAfterAllReceiversDroppedWorks) {
    auto [tx, rx] = broadcast_channel<int>(4);
    { auto dropped = std::move(rx); }
    EXPECT_TRUE(tx.all_receivers_dropped());
    auto rx2 = tx.subscribe();
    auto r = tx.send(7);
    EXPECT_TRUE(r.has_value());
    EXPECT_EQ(*r, 1u);
    (void)rx2;
}

TYPED_TEST(BroadcastTest, AllSendersDropped) {
    auto [tx, rx] = broadcast_channel<int>(4);
    EXPECT_FALSE(rx.all_senders_dropped());
    auto tx2 = tx.clone();
    { auto dropped = std::move(tx); }
    EXPECT_FALSE(rx.all_senders_dropped());
    { auto dropped = std::move(tx2); }
    EXPECT_TRUE(rx.all_senders_dropped());
}

// Move-assigning into a live BroadcastSender/BroadcastReceiver must run the
// decrement-and-wake protocol on the OLD channel, not just silently drop the
// old Rc. Regression test mirroring the analogous mpsc/watch bug.
TYPED_TEST(BroadcastTest, ReassigningLiveSenderDropsOldChannelSender) {
    auto chan_a = broadcast_channel<int>(4);
    auto tx_a   = std::move(chan_a.first);
    auto rx_a   = std::move(chan_a.second);
    auto chan_b = broadcast_channel<int>(4);
    auto tx_b   = std::move(chan_b.first);

    tx_a = std::move(tx_b); // displaces the sender for channel A

    EXPECT_TRUE(rx_a.all_senders_dropped());
}

TYPED_TEST(BroadcastTest, ReassigningLiveReceiverDropsOldChannelReceiver) {
    auto chan_a = broadcast_channel<int>(4);
    auto tx_a   = std::move(chan_a.first);
    auto rx_a   = std::move(chan_a.second);
    auto chan_b = broadcast_channel<int>(4);
    auto rx_b   = std::move(chan_b.second);

    rx_a = std::move(rx_b); // displaces the receiver for channel A

    EXPECT_TRUE(tx_a.all_receivers_dropped());
}

// --- try_recv ---

TYPED_TEST(BroadcastTest, TryRecvEmptyWhenNothingBuffered) {
    auto [tx, rx] = broadcast_channel<int>(4);
    auto r = rx.try_recv();
    EXPECT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, BroadcastRecvError::Kind::Empty);
    (void)tx;
}

TYPED_TEST(BroadcastTest, TryRecvValueAfterSend) {
    auto [tx, rx] = broadcast_channel<int>(4);
    tx.send(99);
    auto r = rx.try_recv();
    EXPECT_TRUE(r.has_value());
    EXPECT_EQ(*r, 99);
}

TYPED_TEST(BroadcastTest, TryRecvFifoOrder) {
    auto [tx, rx] = broadcast_channel<int>(8);
    for (int i = 0; i < 5; ++i) tx.send(i);
    for (int i = 0; i < 5; ++i) {
        auto r = rx.try_recv();
        EXPECT_TRUE(r.has_value());
        EXPECT_EQ(*r, i);
    }
}

TYPED_TEST(BroadcastTest, TryRecvClosedAfterSenderDroppedAndDrained) {
    auto [tx, rx] = broadcast_channel<int>(4);
    tx.send(1);
    { auto dropped = std::move(tx); }
    EXPECT_TRUE(rx.try_recv().has_value()); // drains the buffered value
    auto r = rx.try_recv();
    EXPECT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, BroadcastRecvError::Kind::Closed);
}

TYPED_TEST(BroadcastTest, TryRecvLaggedWhenCursorEvicted) {
    auto [tx, rx] = broadcast_channel<int>(2);
    tx.send(1); tx.send(2); tx.send(3); // capacity 2 — overwrites seq 0's slot
    auto r = rx.try_recv();
    EXPECT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, BroadcastRecvError::Kind::Lagged);
    EXPECT_EQ(r.error().skipped, 1u);
    // Cursor snapped forward to the oldest remaining value — next call succeeds.
    auto r2 = rx.try_recv();
    EXPECT_TRUE(r2.has_value());
    EXPECT_EQ(*r2, 2);
    auto r3 = rx.try_recv();
    EXPECT_TRUE(r3.has_value());
    EXPECT_EQ(*r3, 3);
}

TYPED_TEST(BroadcastTest, SubscribeOnlySeesFutureValues) {
    auto [tx, rx1] = broadcast_channel<int>(4);
    tx.send(1);
    auto rx2 = tx.subscribe();
    tx.send(2);
    auto r1a = rx1.try_recv();
    auto r1b = rx1.try_recv();
    EXPECT_EQ(*r1a, 1);
    EXPECT_EQ(*r1b, 2);
    auto r2a = rx2.try_recv();
    EXPECT_EQ(*r2a, 2);
    auto r2b = rx2.try_recv();
    EXPECT_FALSE(r2b.has_value());
    EXPECT_EQ(r2b.error().kind, BroadcastRecvError::Kind::Empty);
}

TYPED_TEST(BroadcastTest, ResubscribeStartsFreshNotAtOriginalCursor) {
    auto [tx, rx1] = broadcast_channel<int>(4);
    tx.send(1);
    tx.send(2);
    [[maybe_unused]] auto skip = rx1.try_recv(); // advance rx1's cursor past seq 0
    auto rx2 = rx1.resubscribe();
    tx.send(3);
    auto r = rx2.try_recv();
    EXPECT_TRUE(r.has_value());
    EXPECT_EQ(*r, 3); // not 2 — resubscribe starts at "now", not at rx1's cursor
}

TYPED_TEST(BroadcastTest, MultipleReceiversEachSeeEveryValue) {
    auto [tx, rx1] = broadcast_channel<int>(4);
    auto rx2 = tx.subscribe();
    auto rx3 = tx.subscribe();
    tx.send(10);
    tx.send(20);
    for (auto* rx : {&rx1, &rx2, &rx3}) {
        EXPECT_EQ(*rx->try_recv(), 10);
        EXPECT_EQ(*rx->try_recv(), 20);
    }
}

// --- Async ---

TYPED_TEST(BroadcastTest, RecvReceivesValue) {
    this->traits.rt.block_on([]() -> Coro<void> {
        auto _ch = broadcast_channel<int>(4);
        auto tx = std::move(_ch.first);
        auto rx = std::move(_ch.second);
        tx.send(42);
        auto v = co_await rx.recv();
        EXPECT_TRUE(v.has_value());
        EXPECT_EQ(*v, 42);
    }());
}

TYPED_TEST(BroadcastTest, RecvSuspendsUntilSend) {
    this->traits.rt.block_on([]() -> Coro<void> {
        auto _ch = broadcast_channel<int>(4);
        auto tx = std::move(_ch.first);
        auto rx = std::move(_ch.second);
        auto producer = coro::spawn([](BroadcastSender<int> tx) -> Coro<void> {
            tx.send(7);
            co_return;
        }(std::move(tx)));
        auto v = co_await rx.recv();
        EXPECT_TRUE(v.has_value());
        EXPECT_EQ(*v, 7);
        co_await producer;
    }());
}

TYPED_TEST(BroadcastTest, RecvClosedWhenAllSendersDroppedAndDrained) {
    this->traits.rt.block_on([]() -> Coro<void> {
        auto _ch = broadcast_channel<int>(4);
        auto tx = std::move(_ch.first);
        auto rx = std::move(_ch.second);
        tx.send(1);
        { auto dropped = std::move(tx); }
        auto v1 = co_await rx.recv();
        EXPECT_TRUE(v1.has_value());
        EXPECT_EQ(*v1, 1);
        auto v2 = co_await rx.recv();
        EXPECT_FALSE(v2.has_value());
        EXPECT_EQ(v2.error().kind, BroadcastRecvError::Kind::Closed);
    }());
}

TYPED_TEST(BroadcastTest, RecvLaggedThenRecoversToOldestRemaining) {
    this->traits.rt.block_on([]() -> Coro<void> {
        auto _ch = broadcast_channel<int>(2);
        auto tx = std::move(_ch.first);
        auto rx = std::move(_ch.second);
        tx.send(1); tx.send(2); tx.send(3);
        auto v1 = co_await rx.recv();
        EXPECT_FALSE(v1.has_value());
        EXPECT_EQ(v1.error().kind, BroadcastRecvError::Kind::Lagged);
        EXPECT_EQ(v1.error().skipped, 1u);
        auto v2 = co_await rx.recv();
        EXPECT_TRUE(v2.has_value());
        EXPECT_EQ(*v2, 2);
    }());
}

TYPED_TEST(BroadcastTest, MultipleReceiversWokenOnSend) {
    int count = 0;
    this->traits.rt.block_on([](int& count) -> Coro<void> {
        auto _ch = broadcast_channel<int>(4);
        auto tx  = std::move(_ch.first);
        auto rx1 = std::move(_ch.second);
        auto rx2 = tx.subscribe();
        auto rx3 = tx.subscribe();
        auto producer = coro::spawn([](BroadcastSender<int> tx) -> Coro<void> {
            tx.send(1);
            co_return;
        }(std::move(tx)));
        // GCC fails to deduce the awaited type when .has_value() is chained
        // directly onto a parenthesized co_await expression — bind to a
        // local first (see test_watch.cpp for the same workaround).
        auto r1 = co_await rx1.recv();
        if (r1.has_value()) ++count;
        auto r2 = co_await rx2.recv();
        if (r2.has_value()) ++count;
        auto r3 = co_await rx3.recv();
        if (r3.has_value()) ++count;
        co_await producer;
    }(count));
    EXPECT_EQ(count, 3);
}

// Regression test for the cancellation-safety requirement documented in
// channels.md: a losing select() branch's RecvFuture must unlink its node
// from receiver_waiters on drop, and the receiver must remain usable for a
// fresh recv() call afterward — broadcast's select()-heavy usage pattern
// (e.g. KitchenLED's mqtt_dispatch_loop) exercises this every iteration.
TYPED_TEST(BroadcastTest, RecvCanBeReusedAfterCancelledSelect) {
    this->traits.rt.block_on([]() -> Coro<void> {
        auto _ch = broadcast_channel<int>(4);
        auto tx = std::move(_ch.first);
        auto rx = std::move(_ch.second);
        struct ImmediateInt {
            using OutputType = int;
            PollResult<int> poll(detail::Context&) { return 99; }
        };
        auto r1 = co_await select(rx.recv(), ImmediateInt{});
        EXPECT_EQ(r1.index(), 1u);
        tx.send(7);
        auto v = co_await rx.recv();
        EXPECT_TRUE(v.has_value());
        EXPECT_EQ(*v, 7);
    }());
}

TYPED_TEST(BroadcastTest, MultipleSendersClone) {
    this->traits.rt.block_on([]() -> Coro<void> {
        auto _ch = broadcast_channel<int>(4);
        auto tx = std::move(_ch.first);
        auto rx = std::move(_ch.second);
        auto tx2 = tx.clone();
        tx.send(1);
        tx2.send(2);
        auto v1 = co_await rx.recv();
        auto v2 = co_await rx.recv();
        EXPECT_EQ(*v1, 1);
        EXPECT_EQ(*v2, 2);
    }());
}

// Expensive-to-copy values should be wrapped in coro::detail::Rc<T> per
// channels.md — confirm a string-like T still works directly for small payloads.
TYPED_TEST(BroadcastTest, WorksWithStringPayload) {
    auto [tx, rx1] = broadcast_channel<std::string>(4);
    auto rx2 = tx.subscribe();
    tx.send("hello");
    EXPECT_EQ(*rx1.try_recv(), "hello");
    EXPECT_EQ(*rx2.try_recv(), "hello");
}

// --- Releasing read values ---

TYPED_TEST(BroadcastTest, ValueReleasedOnceEveryReceiverReadsIt) {
    auto [tx, rx1] = broadcast_channel<std::shared_ptr<int>>(4);
    auto rx2 = tx.subscribe();
    auto value = std::make_shared<int>(1);
    std::weak_ptr<int> watch = value;
    EXPECT_TRUE(tx.send(std::move(value)).has_value());
    { auto copy = rx1.try_recv(); EXPECT_TRUE(copy.has_value()); }
    EXPECT_FALSE(watch.expired());  // rx2 hasn't read it yet
    { auto last = rx2.try_recv(); EXPECT_TRUE(last.has_value()); }
    EXPECT_TRUE(watch.expired());
}

TYPED_TEST(BroadcastTest, LastReaderMovesValueOut) {
    auto [tx, rx] = broadcast_channel<std::shared_ptr<int>>(4);
    tx.send(std::make_shared<int>(1));
    auto r = rx.try_recv();
    EXPECT_TRUE(r.has_value());
    EXPECT_EQ(r->use_count(), 1);  // no copy left in the ring
}

TYPED_TEST(BroadcastTest, DroppingReceiverReleasesItsUnreadValues) {
    auto [tx, rx1] = broadcast_channel<std::shared_ptr<int>>(4);
    auto rx2 = tx.subscribe();
    auto value = std::make_shared<int>(1);
    std::weak_ptr<int> watch = value;
    tx.send(std::move(value));
    { auto copy = rx1.try_recv(); EXPECT_TRUE(copy.has_value()); }
    { auto dropped = std::move(rx2); }
    EXPECT_TRUE(watch.expired());
}

TYPED_TEST(BroadcastTest, DeactivatingReceiverReleasesItsUnreadValues) {
    auto [tx, rx] = broadcast_channel<std::shared_ptr<int>>(4);
    auto value = std::make_shared<int>(1);
    std::weak_ptr<int> watch = value;
    tx.send(std::move(value));
    auto handle = std::move(rx).deactivate();
    EXPECT_TRUE(watch.expired());
    (void)handle;
}

TYPED_TEST(BroadcastTest, ReceiverSubscribedAfterSendDoesNotHoldValue) {
    auto [tx, rx1] = broadcast_channel<std::shared_ptr<int>>(4);
    auto value = std::make_shared<int>(1);
    std::weak_ptr<int> watch = value;
    tx.send(std::move(value));
    auto rx2 = tx.subscribe();
    { auto last = rx1.try_recv(); EXPECT_TRUE(last.has_value()); }
    EXPECT_TRUE(watch.expired());
    EXPECT_EQ(rx2.try_recv().error().kind, BroadcastRecvError::Kind::Empty);
}

// A lagged receiver is still counted in every value from the oldest remaining on,
// so reading them, or dropping it, releases them.
TYPED_TEST(BroadcastTest, LaggedReceiverReleasesRemainingValues) {
    auto [tx, rx] = broadcast_channel<std::shared_ptr<int>>(2);
    tx.send(std::make_shared<int>(1));
    auto second = std::make_shared<int>(2);
    auto third  = std::make_shared<int>(3);
    std::weak_ptr<int> watch2 = second;
    std::weak_ptr<int> watch3 = third;
    tx.send(std::move(second));
    tx.send(std::move(third));
    EXPECT_EQ(rx.try_recv().error().kind, BroadcastRecvError::Kind::Lagged);
    { auto v = rx.try_recv(); EXPECT_EQ(**v, 2); }
    EXPECT_TRUE(watch2.expired());
    EXPECT_FALSE(watch3.expired());
    { auto dropped = std::move(rx); }
    EXPECT_TRUE(watch3.expired());
}

TYPED_TEST(BroadcastTest, OverwriteReplacesUnreadValue) {
    auto [tx, rx] = broadcast_channel<std::shared_ptr<int>>(1);
    auto first = std::make_shared<int>(1);
    std::weak_ptr<int> watch = first;
    tx.send(std::move(first));
    tx.send(std::make_shared<int>(2));
    EXPECT_TRUE(watch.expired());
    auto r = rx.try_recv();
    EXPECT_EQ(r.error().kind, BroadcastRecvError::Kind::Lagged);
    EXPECT_EQ(r.error().skipped, 1u);
    EXPECT_EQ(**rx.try_recv(), 2);
}

TYPED_TEST(BroadcastTest, RecvFutureReleasesValueAfterLastRead) {
    this->traits.rt.block_on([]() -> Coro<void> {
        auto _ch = broadcast_channel<std::shared_ptr<int>>(4);
        auto tx = std::move(_ch.first);
        auto rx = std::move(_ch.second);
        tx.send(std::make_shared<int>(5));
        auto v = co_await rx.recv();
        EXPECT_TRUE(v.has_value());
        EXPECT_EQ(v->use_count(), 1);
    }());
}

// --- Inactive receiver ---

TYPED_TEST(BroadcastTest, InactiveReceiverIsCopyable) {
    auto [tx, rx] = broadcast_channel<int>(4);
    auto handle  = tx.inactive();
    auto handle2 = handle;
    handle = handle2;
    (void)rx;
}

TYPED_TEST(BroadcastTest, InactiveReceiverIsNotCounted) {
    auto [tx, rx] = broadcast_channel<int>(4);
    auto from_tx = tx.inactive();
    auto from_rx = std::move(rx).deactivate();
    EXPECT_TRUE(tx.all_receivers_dropped());
    auto r = tx.send(1);
    EXPECT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), 1);
    (void)from_tx; (void)from_rx;
}

TYPED_TEST(BroadcastTest, ActivateSeesOnlyFutureValues) {
    auto [tx, rx] = broadcast_channel<int>(4);
    auto handle = tx.inactive();
    tx.send(1);
    auto live = handle.activate();
    EXPECT_FALSE(tx.all_receivers_dropped());
    EXPECT_EQ(live.try_recv().error().kind, BroadcastRecvError::Kind::Empty);
    auto r = tx.send(2);
    EXPECT_EQ(*r, 2u);
    EXPECT_EQ(*live.try_recv(), 2);
    (void)rx;
}

TYPED_TEST(BroadcastTest, EachActivationIsIndependentReceiver) {
    auto [tx, rx] = broadcast_channel<int>(4);
    auto handle = std::move(rx).deactivate();
    auto copy = handle;
    auto live1 = handle.activate();
    auto live2 = copy.activate();
    EXPECT_EQ(*tx.send(3), 2u);
    EXPECT_EQ(*live1.try_recv(), 3);
    EXPECT_EQ(*live2.try_recv(), 3);
}

TYPED_TEST(BroadcastTest, InactiveReceiverDoesNotKeepChannelOpen) {
    auto [tx, rx] = broadcast_channel<int>(4);
    auto handle = tx.inactive();
    { auto dropped = std::move(tx); }
    auto live = handle.activate();
    EXPECT_TRUE(live.all_senders_dropped());
    EXPECT_EQ(live.try_recv().error().kind, BroadcastRecvError::Kind::Closed);
    (void)rx;
}

TYPED_TEST(BroadcastTest, ActivatedReceiverReceivesAsync) {
    this->traits.rt.block_on([]() -> Coro<void> {
        auto _ch = broadcast_channel<int>(4);
        auto tx = std::move(_ch.first);
        auto live = std::move(_ch.second).deactivate().activate();
        auto producer = coro::spawn([](BroadcastSender<int> tx) -> Coro<void> {
            tx.send(8);
            co_return;
        }(std::move(tx)));
        auto v = co_await live.recv();
        EXPECT_TRUE(v.has_value());
        EXPECT_EQ(v.value_or(-1), 8);
        co_await producer;
        auto end = co_await live.recv();
        EXPECT_FALSE(end.has_value());
        EXPECT_EQ(end.error().kind, BroadcastRecvError::Kind::Closed);
    }());
}

// --- receivers_active() ---

TYPED_TEST(BroadcastTest, ReceiversActiveCompletesAtOnceWithReceiver) {
    this->traits.rt.block_on([]() -> Coro<void> {
        auto _ch = broadcast_channel<int>(4);
        auto tx = std::move(_ch.first);
        auto rx = std::move(_ch.second);
        co_await tx.receivers_active();
        EXPECT_FALSE(tx.all_receivers_dropped());
    }());
}

TYPED_TEST(BroadcastTest, ReceiversActiveWaitsForActivate) {
    this->traits.rt.block_on([]() -> Coro<void> {
        auto _ch = broadcast_channel<int>(4);
        auto tx = std::move(_ch.first);
        auto handle = std::move(_ch.second).deactivate();
        auto producer = coro::spawn([](BroadcastSender<int> tx) -> Coro<void> {
            co_await tx.receivers_active();
            EXPECT_TRUE(tx.send(4).has_value());
        }(std::move(tx)));
        auto live = handle.activate();
        auto v = co_await live.recv();
        EXPECT_EQ(v.value_or(-1), 4);
        co_await producer;
    }());
}

TYPED_TEST(BroadcastTest, ReceiversActiveWokenBySubscribe) {
    this->traits.rt.block_on([]() -> Coro<void> {
        auto _ch = broadcast_channel<int>(4);
        auto tx = std::move(_ch.first);
        { auto dropped = std::move(_ch.second); }
        auto waiter = coro::spawn([](BroadcastSender<int> tx) -> Coro<void> {
            co_await tx.receivers_active();
        }(tx.clone()));
        auto rx = tx.subscribe();
        co_await waiter;
    }());
}

TYPED_TEST(BroadcastTest, ReceiversActiveCanBeReusedAfterCancelledSelect) {
    this->traits.rt.block_on([]() -> Coro<void> {
        auto _ch = broadcast_channel<int>(4);
        auto tx = std::move(_ch.first);
        auto handle = std::move(_ch.second).deactivate();
        struct ImmediateInt {
            using OutputType = int;
            PollResult<int> poll(detail::Context&) { return 99; }
        };
        auto r1 = co_await select(tx.receivers_active(), ImmediateInt{});
        EXPECT_EQ(r1.index(), 1u);
        auto live = handle.activate();
        co_await tx.receivers_active();
    }());
}

#ifndef CORO_PICO

TEST(BroadcastSingleThreadedTest, ReceiversActiveSuspendsUntilActivate) {
    Runtime rt(1);
    rt.block_on([]() -> Coro<void> {
        auto _ch = broadcast_channel<int>(4);
        auto tx = std::move(_ch.first);
        auto handle = std::move(_ch.second).deactivate();
        bool done = false;
        auto waiter = coro::spawn([](BroadcastSender<int> tx, bool& done) -> Coro<void> {
            co_await tx.receivers_active();
            done = true;
        }(tx.clone(), done));
        auto noop = coro::spawn([](int n) -> Coro<int> { co_return n; }(0));
        co_await noop;
        EXPECT_FALSE(done);
        auto live = handle.activate();
        co_await waiter;
        EXPECT_TRUE(done);
    }());
}

#endif
