#include <gtest/gtest.h>

#include <coro/coro.h>
#include <coro/sync/oneshot.h>
#include <coro/future.h>
#include <coro/runtime/runtime.h>
#include <memory>

using namespace coro;

// --- Concept checks ---
static_assert(Future<oneshot::OneshotReceiver<int>>);
static_assert(Future<oneshot::OneshotReceiver<std::unique_ptr<int>>>);

// --- Construction ---

TEST(OneshotTest, ChannelReturnsLinkedPair) {
    auto [tx, rx] = oneshot::channel<int>();
    (void)tx; (void)rx;
}

TEST(OneshotTest, SenderIsMovable) {
    auto [tx, rx] = oneshot::channel<int>();
    auto tx2 = std::move(tx);
    (void)tx2; (void)rx;
}

TEST(OneshotTest, ReceiverIsMovable) {
    auto [tx, rx] = oneshot::channel<int>();
    auto rx2 = std::move(rx);
    (void)tx; (void)rx2;
}

// --- Synchronous send API ---

TEST(OneshotTest, SendReturnsSuccessWhenReceiverAlive) {
    auto [tx, rx] = oneshot::channel<int>();
    auto result = tx.send(42);
    EXPECT_TRUE(result.has_value());
    (void)rx;
}

TEST(OneshotTest, SendReturnsValueWhenReceiverDropped) {
    auto [tx, rx] = oneshot::channel<int>();
    { auto dropped = std::move(rx); } // drop receiver
    auto result = tx.send(42);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), 42);
}

TEST(OneshotTest, SenderDropWithoutSendCloseChannel) {
    auto [tx, rx] = oneshot::channel<int>();
    { auto dropped = std::move(tx); } // drop sender without sending
    // rx should be closable — construction test only; async behaviour tested below
    (void)rx;
}

// --- Move-only types ---

TEST(OneshotTest, SendAcceptsMoveOnlyType) {
    auto [tx, rx] = oneshot::channel<std::unique_ptr<int>>();
    auto result = tx.send(std::make_unique<int>(99));
    EXPECT_TRUE(result.has_value());
    (void)rx;
}

TEST(OneshotTest, FailedSendReturnsMoveOnlyValue) {
    auto [tx, rx] = oneshot::channel<std::unique_ptr<int>>();
    { auto dropped = std::move(rx); }
    auto result = tx.send(std::make_unique<int>(99));
    EXPECT_FALSE(result.has_value());
    EXPECT_NE(result.error(), nullptr);
    EXPECT_EQ(*result.error(), 99);
}

// --- Async integration (require Phase 3 implementation) ---

TEST(OneshotTest, ReceiverGetsValueAfterSend) {
    Runtime rt;
    std::optional<int> received;
    rt.block_on([&]() -> Coro<void> {
        auto [tx, rx] = oneshot::channel<int>();
        tx.send(42);
        auto result = co_await rx;
        EXPECT_TRUE(result.has_value());
        received = *result;
    }());
    EXPECT_EQ(received, 42);
}

TEST(OneshotTest, ReceiverErrorWhenSenderDropped) {
    Runtime rt;
    rt.block_on([&]() -> Coro<void> {
        auto [tx, rx] = oneshot::channel<int>();
        { auto dropped = std::move(tx); }
        auto result = co_await rx;
        EXPECT_FALSE(result.has_value());
        EXPECT_EQ(result.error(), ChannelError::Closed);
    }());
}

TEST(OneshotTest, ReceiverSuspendsUntilSend) {
    Runtime rt;
    rt.block_on([&]() -> Coro<void> {
        auto [tx, rx] = oneshot::channel<int>();
        // Spawn a task that sends after the receiver is waiting.
        co_await coro::spawn([tx = std::move(tx)]() mutable -> Coro<void> {
            tx.send(7);
            co_return;
        }()).submit();
        auto result = co_await rx;
        EXPECT_TRUE(result.has_value());
        EXPECT_EQ(*result, 7);
    }());
}
