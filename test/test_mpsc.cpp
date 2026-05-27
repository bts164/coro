#include <gtest/gtest.h>
#include <coro/coro.h>
#include <coro/co_invoke.h>
#include <coro/sync/mpsc.h>
#include <coro/sync/select.h>
#include <coro/future.h>
#include <coro/stream.h>
#include <coro/runtime/runtime.h>
#include <coro/task/spawn_blocking.h>
#include <memory>
#include <optional>
#include <vector>

using namespace coro;

// --- Concept checks ---
static_assert(Stream<MpscReceiver<int>>);
static_assert(Future<MpscSendFuture<int>>);
static_assert(Future<MpscSendFuture<std::unique_ptr<int>>>);
static_assert(Future<MpscRecvFuture<int>>);
static_assert(Future<MpscRecvFuture<std::unique_ptr<int>>>);

// --- Construction ---

TEST(MpscTest, ChannelReturnsLinkedPair) {
    auto [tx, rx] = mpsc_channel<int>(16);
    (void)tx; (void)rx;
}

TEST(MpscTest, SenderIsMovable) {
    auto [tx, rx] = mpsc_channel<int>(4);
    auto tx2 = std::move(tx);
    (void)tx2; (void)rx;
}

TEST(MpscTest, ReceiverIsMovable) {
    auto [tx, rx] = mpsc_channel<int>(4);
    auto rx2 = std::move(rx);
    (void)tx; (void)rx2;
}

TEST(MpscTest, SenderIsCloneable) {
    auto [tx, rx] = mpsc_channel<int>(4);
    auto tx2 = tx.clone();
    (void)tx2; (void)rx;
}

// --- try_send ---

TEST(MpscTest, TrySendSucceedsWhenBufferHasSpace) {
    auto [tx, rx] = mpsc_channel<int>(4);
    auto r = tx.try_send(1);
    EXPECT_TRUE(r.has_value());
    (void)rx;
}

TEST(MpscTest, TrySendFullWhenBufferFull) {
    auto [tx, rx] = mpsc_channel<int>(2);
    tx.try_send(1);
    tx.try_send(2);
    auto r = tx.try_send(3);
    EXPECT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, TrySendError<int>::Kind::Full);
    EXPECT_EQ(r.error().value, 3);
    (void)rx;
}

TEST(MpscTest, TrySendDisconnectedWhenReceiverDropped) {
    auto [tx, rx] = mpsc_channel<int>(4);
    { auto dropped = std::move(rx); }
    auto r = tx.try_send(42);
    EXPECT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, TrySendError<int>::Kind::Disconnected);
    EXPECT_EQ(r.error().value, 42);
}

TEST(MpscTest, TrySendReturnsMoveOnlyValueOnFailure) {
    auto [tx, rx] = mpsc_channel<std::unique_ptr<int>>(1);
    tx.try_send(std::make_unique<int>(1));
    auto r = tx.try_send(std::make_unique<int>(2));
    EXPECT_FALSE(r.has_value());
    EXPECT_NE(r.error().value, nullptr);
    EXPECT_EQ(*r.error().value, 2);
    (void)rx;
}

// --- try_recv ---

TEST(MpscTest, TryRecvEmptyWhenNothingBuffered) {
    auto [tx, rx] = mpsc_channel<int>(4);
    auto r = rx.try_recv();
    EXPECT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), ChannelError::Empty);
    (void)tx;
}

TEST(MpscTest, TryRecvValueAfterTrySend) {
    auto [tx, rx] = mpsc_channel<int>(4);
    tx.try_send(99);
    auto r = rx.try_recv();
    EXPECT_TRUE(r.has_value());
    EXPECT_EQ(*r, 99);
}

TEST(MpscTest, TryRecvSenderDroppedAfterDrain) {
    auto [tx, rx] = mpsc_channel<int>(4);
    { auto dropped = std::move(tx); }
    auto r = rx.try_recv();
    EXPECT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), ChannelError::SenderDropped);
}

TEST(MpscTest, TryRecvFifoOrder) {
    auto [tx, rx] = mpsc_channel<int>(8);
    for (int i = 0; i < 5; ++i) tx.try_send(i);
    for (int i = 0; i < 5; ++i) {
        auto r = rx.try_recv();
        EXPECT_TRUE(r.has_value());
        EXPECT_EQ(*r, i);
    }
}

// --- Async integration (require Phase 3 implementation) ---

TEST(MpscTest, SingleSenderSingleReceiver) {
    Runtime rt(1);
    std::vector<int> received;
    rt.block_on([&]() -> Coro<void> {
        auto [tx, rx] = mpsc_channel<int>(4);
        co_await tx.send(1);
        co_await tx.send(2);
        co_await tx.send(3);
        { auto dropped = std::move(tx); }
        while (auto v = co_await next(rx))
            received.push_back(*v);
    }());
    EXPECT_EQ(received, (std::vector<int>{1, 2, 3}));
}

TEST(MpscTest, SenderSuspendsWhenFull) {
    Runtime rt(1);
    rt.block_on([&]() -> Coro<void> {
        int x[3] = {0, 0, 0};
        auto [tx, rx] = mpsc_channel<int*>(2);
        co_await tx.send(&x[0]);
        co_await tx.send(&x[1]);
        // Buffer full — spawn sender that will suspend until we recv.
        auto h = coro::spawn(coro::co_invoke([rx = std::move(rx)]() mutable -> Coro<void> {
            auto v1 = co_await next(rx); ++(*v1.value());
            auto v2 = co_await next(rx); ++(*v2.value());
            auto v3 = co_await next(rx); ++(*v3.value());
        }));
        EXPECT_EQ(x[0], 0);
        EXPECT_EQ(x[1], 0);
        EXPECT_EQ(x[2], 0);
        co_await tx.send(&x[2]); // suspends until receiver drains
        EXPECT_EQ(x[0], 1);
        EXPECT_EQ(x[1], 1);
        EXPECT_EQ(x[2], 1);
        co_await h; // wait for receiver to finish
    }());
}

TEST(MpscTest, MultipleSenders) {
    Runtime rt(1);
    std::vector<int> received;
    rt.block_on([&]() -> Coro<void> {
        auto [tx, rx] = mpsc_channel<int>(8);
        auto tx2 = tx.clone();
        co_await tx.send(1);
        co_await tx2.send(2);
        { auto d1 = std::move(tx); auto d2 = std::move(tx2); }
        while (auto v = co_await next(rx))
            received.push_back(*v);
    }());
    EXPECT_EQ(received.size(), 2u);
}

TEST(MpscTest, ReceiverExhaustedAfterAllSendersDropped) {
    Runtime rt(1);
    rt.block_on([&]() -> Coro<void> {
        auto [tx, rx] = mpsc_channel<int>(4);
        { auto dropped = std::move(tx); }
        auto v = co_await next(rx);
        EXPECT_FALSE(v.has_value()); // nullopt — stream exhausted
    }());
}

TEST(MpscTest, ReceiverDroppedReturnsSendError) {
    Runtime rt(1);
    rt.block_on([&]() -> Coro<void> {
        auto [tx, rx] = mpsc_channel<int>(1);
        co_await tx.send(1); // fills buffer
        { auto dropped = std::move(rx); }
        auto r = co_await tx.send(2); // buffer full + receiver gone
        EXPECT_FALSE(r.has_value());
        EXPECT_EQ(r.error(), 2);
    }());
}

TEST(MpscTest, ErrorChannelPattern) {
    Runtime rt(1);
    rt.block_on([&]() -> Coro<void> {
        auto [tx, rx] = mpsc_channel<std::expected<int, std::string>>(4);
        co_await tx.send(42);
        co_await tx.send(std::unexpected(std::string("oops")));
        { auto dropped = std::move(tx); }

        auto r1 = co_await next(rx);
        EXPECT_TRUE(r1.has_value());
        EXPECT_TRUE(r1->has_value());
        EXPECT_EQ(**r1, 42);

        auto r2 = co_await next(rx);
        EXPECT_TRUE(r2.has_value());
        EXPECT_FALSE(r2->has_value());
        EXPECT_EQ(r2->error(), "oops");
    }());
}

// --- recv() API ---

TEST(MpscTest, RecvReceivesValue) {
    Runtime rt(1);
    rt.block_on([&]() -> Coro<void> {
        auto [tx, rx] = mpsc_channel<int>(4);
        co_await tx.send(42);
        { auto dropped = std::move(tx); }
        auto v = co_await rx.recv();
        EXPECT_TRUE(v.has_value());
        EXPECT_EQ(*v, 42);
        auto end = co_await rx.recv();
        EXPECT_FALSE(end.has_value()); // exhausted
    }());
}

TEST(MpscTest, RecvCanBeReusedAfterCancelledSelect) {
    // recv() returns a separate future each time, so the receiver survives a
    // select() branch that did not win.
    Runtime rt(1);
    rt.block_on([&]() -> Coro<void> {
        auto [tx, rx] = mpsc_channel<int>(4);

        // First select: ImmediateInt wins, recv() branch is cancelled.
        struct ImmediateInt {
            using OutputType = int;
            PollResult<int> poll(detail::Context&) { return 99; }
        };
        auto r1 = co_await select(rx.recv(), ImmediateInt{});
        EXPECT_EQ(r1.index(), 1u);

        // Receiver is still usable — send a value and recv it.
        tx.try_send(7);
        auto v = co_await rx.recv();
        EXPECT_TRUE(v.has_value());
        EXPECT_EQ(*v, 7);
    }());
}

// --- blocking_recv / blocking_send ---

TEST(MpscTest, BlockingRecvGetsPreloadedValue) {
    Runtime rt(1);
    int result = -1;
    rt.block_on([](int& out) -> Coro<void> {
        auto [tx, rx] = mpsc_channel<int>(4);
        co_await tx.send(42);
        out = co_await coro::spawn_blocking(
            [rx = std::move(rx)]() mutable -> int {
                auto v = rx.blocking_recv();
                return v ? *v : -1;
            });
    }(result));
    EXPECT_EQ(result, 42);
}

TEST(MpscTest, BlockingRecvBlocksUntilSent) {
    Runtime rt(1);
    int result = -1;
    rt.block_on([](int& out) -> Coro<void> {
        auto [tx, rx] = mpsc_channel<int>(4);
        auto handle = coro::spawn_blocking(
            [rx = std::move(rx)]() mutable -> int {
                auto v = rx.blocking_recv();
                return v ? *v : -1;
            });
        co_await tx.send(99);
        out = co_await std::move(handle);
    }(result));
    EXPECT_EQ(result, 99);
}

TEST(MpscTest, BlockingRecvReturnsNulloptWhenAllSendersDropped) {
    Runtime rt(1);
    bool got_nullopt = false;
    rt.block_on([](bool& out) -> Coro<void> {
        auto [tx, rx] = mpsc_channel<int>(4);
        { auto dropped = std::move(tx); }
        out = co_await coro::spawn_blocking(
            [rx = std::move(rx)]() mutable -> bool {
                return !rx.blocking_recv().has_value();
            });
    }(got_nullopt));
    EXPECT_TRUE(got_nullopt);
}

TEST(MpscTest, BlockingSendDeliversToAsyncReceiver) {
    Runtime rt(1);
    int result = -1;
    rt.block_on([](int& out) -> Coro<void> {
        auto [tx, rx] = mpsc_channel<int>(4);
        auto handle = coro::spawn_blocking(
            [tx = std::move(tx)]() mutable {
                tx.blocking_send(77);
            });
        auto v = co_await next(rx);
        out = *v;
        co_await std::move(handle);
    }(result));
    EXPECT_EQ(result, 77);
}

TEST(MpscTest, BlockingSendBlocksUntilSpace) {
    Runtime rt(1);
    std::vector<int> received;
    rt.block_on([](std::vector<int>& out) -> Coro<void> {
        auto [tx, rx] = mpsc_channel<int>(2);
        co_await tx.send(1);
        co_await tx.send(2);  // channel now full
        auto handle = coro::spawn_blocking(
            [tx = std::move(tx)]() mutable {
                tx.blocking_send(3);  // must wait for space
            });
        // drain one item to open a slot
        auto v = co_await next(rx);
        out.push_back(*v);
        co_await std::move(handle);
        while (auto w = co_await next(rx))
            out.push_back(*w);
    }(received));
    EXPECT_EQ(received, (std::vector<int>{1, 2, 3}));
}

TEST(MpscTest, BlockingSendReturnsUnsentValueWhenReceiverDropped) {
    Runtime rt(1);
    std::optional<int> unsent;
    rt.block_on([](std::optional<int>& out) -> Coro<void> {
        auto [tx, rx] = mpsc_channel<int>(1);
        co_await tx.send(1);           // fill the only slot
        { auto dropped = std::move(rx); }  // drop receiver
        out = co_await coro::spawn_blocking(
            [tx = std::move(tx)]() mutable -> std::optional<int> {
                auto r = tx.blocking_send(2);
                return r.has_value() ? std::nullopt : std::optional<int>{r.error()};
            });
    }(unsent));
    EXPECT_TRUE(unsent.has_value());
    EXPECT_EQ(*unsent, 2);
}

TEST(MpscTest, BlockingWorkerPipeline) {
    // Blocking thread receives from one channel, transforms, sends to another —
    // the pattern used in the DSP pipeline example.
    Runtime rt(1);
    std::vector<int> results;
    rt.block_on([](std::vector<int>& out) -> Coro<void> {
        auto [in_tx,  in_rx]  = mpsc_channel<int>(4);
        auto [out_tx, out_rx] = mpsc_channel<int>(4);
        auto worker = coro::spawn_blocking(
            [in_rx  = std::move(in_rx),
             out_tx = std::move(out_tx)]() mutable {
                while (auto v = in_rx.blocking_recv())
                    if (!out_tx.blocking_send(*v * 2).has_value()) break;
            });
        co_await in_tx.send(1);
        co_await in_tx.send(2);
        co_await in_tx.send(3);
        { auto dropped = std::move(in_tx); }
        while (auto v = co_await next(out_rx))
            out.push_back(*v);
        co_await std::move(worker);
    }(results));
    EXPECT_EQ(results, (std::vector<int>{2, 4, 6}));
}
