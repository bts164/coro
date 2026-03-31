#include <gtest/gtest.h>
#include <coro/coro.h>
#include <coro/sync/watch.h>
#include <coro/future.h>
#include <coro/runtime/runtime.h>

using namespace coro;

// --- Concept checks ---
static_assert(Future<watch::ChangedFuture<int>>);

// --- Construction ---

TEST(WatchTest, ChannelReturnsLinkedPair) {
    auto [tx, rx] = watch::channel<int>(0);
    (void)tx; (void)rx;
}

TEST(WatchTest, SenderIsMovable) {
    auto [tx, rx] = watch::channel<int>(0);
    auto tx2 = std::move(tx);
    (void)tx2; (void)rx;
}

TEST(WatchTest, ReceiverIsMovable) {
    auto [tx, rx] = watch::channel<int>(0);
    auto rx2 = std::move(rx);
    (void)tx; (void)rx2;
}

TEST(WatchTest, ReceiverIsCloneable) {
    auto [tx, rx] = watch::channel<int>(0);
    auto rx2 = rx.clone();
    (void)tx; (void)rx2;
}

// --- Synchronous send / borrow ---

TEST(WatchTest, SendSucceedsWithLiveReceiver) {
    auto [tx, rx] = watch::channel<int>(0);
    auto r = tx.send(42);
    EXPECT_TRUE(r.has_value());
    (void)rx;
}

TEST(WatchTest, SendReturnsValueWhenAllReceiversDropped) {
    auto [tx, rx] = watch::channel<int>(0);
    { auto dropped = std::move(rx); }
    auto r = tx.send(42);
    EXPECT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), 42);
}

TEST(WatchTest, BorrowReadsInitialValue) {
    auto [tx, rx] = watch::channel<int>(7);
    auto guard = rx.borrow();
    EXPECT_EQ(*guard, 7);
    (void)tx;
}

TEST(WatchTest, BorrowReflectsLatestSend) {
    auto [tx, rx] = watch::channel<int>(0);
    tx.send(99);
    auto guard = rx.borrow();
    EXPECT_EQ(*guard, 99);
}

TEST(WatchTest, BorrowArrowOperator) {
    struct Point { int x, y; };
    auto [tx, rx] = watch::channel<Point>({1, 2});
    auto guard = rx.borrow();
    EXPECT_EQ(guard->x, 1);
    EXPECT_EQ(guard->y, 2);
    (void)tx;
}

TEST(WatchTest, ClonesAreIndependent) {
    auto [tx, rx] = watch::channel<int>(0);
    auto rx2 = rx.clone();
    tx.send(10);
    EXPECT_EQ(*rx.borrow(),  10);
    EXPECT_EQ(*rx2.borrow(), 10);
}

TEST(WatchTest, MultipleReceiversSeeLatestValue) {
    auto [tx, rx] = watch::channel<int>(0);
    auto rx2 = rx.clone();
    auto rx3 = rx.clone();
    tx.send(55);
    EXPECT_EQ(*rx.borrow(),  55);
    EXPECT_EQ(*rx2.borrow(), 55);
    EXPECT_EQ(*rx3.borrow(), 55);
}

// --- Async integration (require Phase 3 implementation) ---

TEST(WatchTest, ChangedResolvesWhenValueSent) {
    Runtime rt;
    rt.block_on([&]() -> Coro<void> {
        auto [tx, rx] = watch::channel<int>(0);
        co_await coro::spawn([tx = std::move(tx)]() mutable -> Coro<void> {
            tx.send(42);
            co_return;
        }()).submit();
        auto r = co_await rx.changed();
        EXPECT_TRUE(r.has_value());
        EXPECT_EQ(*rx.borrow(), 42);
    }());
}

TEST(WatchTest, ChangedResolvesImmediatelyIfAlreadyChanged) {
    Runtime rt;
    rt.block_on([&]() -> Coro<void> {
        auto [tx, rx] = watch::channel<int>(0);
        tx.send(1); // version now 1; last_seen is 0
        auto r = co_await rx.changed(); // should resolve immediately
        EXPECT_TRUE(r.has_value());
    }());
}

TEST(WatchTest, ChangedErrorWhenSenderDropped) {
    Runtime rt;
    rt.block_on([&]() -> Coro<void> {
        auto [tx, rx] = watch::channel<int>(0);
        { auto dropped = std::move(tx); }
        auto r = co_await rx.changed();
        EXPECT_FALSE(r.has_value());
        EXPECT_EQ(r.error(), ChannelError::SenderDropped);
    }());
}

TEST(WatchTest, MultipleReceiversWokenOnSend) {
    Runtime rt;
    int count = 0;
    rt.block_on([&]() -> Coro<void> {
        auto [tx, rx1] = watch::channel<int>(0);
        auto rx2 = rx1.clone();
        auto rx3 = rx1.clone();
        tx.send(1);
        if ((co_await rx1.changed()).has_value()) ++count;
        if ((co_await rx2.changed()).has_value()) ++count;
        if ((co_await rx3.changed()).has_value()) ++count;
    }());
    EXPECT_EQ(count, 3);
}

TEST(WatchTest, LastWriteWins) {
    Runtime rt;
    rt.block_on([&]() -> Coro<void> {
        auto [tx, rx] = watch::channel<int>(0);
        tx.send(1);
        tx.send(2);
        tx.send(3);
        co_await rx.changed();
        EXPECT_EQ(*rx.borrow(), 3);
    }());
}
