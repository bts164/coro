#include <gtest/gtest.h>
#include "executor_traits.h"
#include <coro/coro.h>
#include <coro/co_invoke.h>
#include <coro/sync/mpsc.h>
#include <coro/sync/select.h>
#include <coro/future.h>
#include <coro/stream.h>
#include <coro/runtime/runtime.h>
#include <memory>
#include <optional>
#include <vector>

#ifndef CORO_PICO
#include <coro/task/spawn_blocking.h>
#endif

using namespace coro;

static_assert(Stream<MpscReceiver<int>>);
static_assert(Future<MpscSendFuture<int>>);
static_assert(Future<MpscSendFuture<std::unique_ptr<int>>>);
static_assert(Future<MpscRecvFuture<int>>);
static_assert(Future<MpscRecvFuture<std::unique_ptr<int>>>);

// ---------------------------------------------------------------------------
// MpscTest — executor-agnostic tests
// ---------------------------------------------------------------------------

template<typename Traits>
class MpscTest : public testing::Test {
protected:
    Traits traits;
};
TYPED_TEST_SUITE(MpscTest, AllExecutors);

// --- Construction / sync API ---

TYPED_TEST(MpscTest, ChannelReturnsLinkedPair) {
    auto [tx, rx] = mpsc_channel<int>(16);
    (void)tx; (void)rx;
}

TYPED_TEST(MpscTest, SenderIsMovable) {
    auto [tx, rx] = mpsc_channel<int>(4);
    auto tx2 = std::move(tx);
    (void)tx2; (void)rx;
}

TYPED_TEST(MpscTest, ReceiverIsMovable) {
    auto [tx, rx] = mpsc_channel<int>(4);
    auto rx2 = std::move(rx);
    (void)tx; (void)rx2;
}

TYPED_TEST(MpscTest, SenderIsCloneable) {
    auto [tx, rx] = mpsc_channel<int>(4);
    auto tx2 = tx.clone();
    (void)tx2; (void)rx;
}

TYPED_TEST(MpscTest, TrySendSucceedsWhenBufferHasSpace) {
    auto [tx, rx] = mpsc_channel<int>(4);
    EXPECT_TRUE(tx.try_send(1).has_value());
    (void)rx;
}

TYPED_TEST(MpscTest, TrySendFullWhenBufferFull) {
    auto [tx, rx] = mpsc_channel<int>(2);
    tx.try_send(1); tx.try_send(2);
    auto r = tx.try_send(3);
    EXPECT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, TrySendError<int>::Kind::Full);
    EXPECT_EQ(r.error().value, 3);
    (void)rx;
}

TYPED_TEST(MpscTest, TrySendDisconnectedWhenReceiverDropped) {
    auto [tx, rx] = mpsc_channel<int>(4);
    { auto dropped = std::move(rx); }
    auto r = tx.try_send(42);
    EXPECT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, TrySendError<int>::Kind::Disconnected);
    EXPECT_EQ(r.error().value, 42);
}

TYPED_TEST(MpscTest, TrySendReturnsMoveOnlyValueOnFailure) {
    auto [tx, rx] = mpsc_channel<std::unique_ptr<int>>(1);
    tx.try_send(std::make_unique<int>(1));
    auto r = tx.try_send(std::make_unique<int>(2));
    EXPECT_FALSE(r.has_value());
    EXPECT_NE(r.error().value, nullptr);
    EXPECT_EQ(*r.error().value, 2);
    (void)rx;
}

TYPED_TEST(MpscTest, TryRecvEmptyWhenNothingBuffered) {
    auto [tx, rx] = mpsc_channel<int>(4);
    auto r = rx.try_recv();
    EXPECT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), ChannelError::Empty);
    (void)tx;
}

TYPED_TEST(MpscTest, TryRecvValueAfterTrySend) {
    auto [tx, rx] = mpsc_channel<int>(4);
    tx.try_send(99);
    auto r = rx.try_recv();
    EXPECT_TRUE(r.has_value());
    EXPECT_EQ(*r, 99);
}

TYPED_TEST(MpscTest, TryRecvSenderDroppedAfterDrain) {
    auto [tx, rx] = mpsc_channel<int>(4);
    { auto dropped = std::move(tx); }
    auto r = rx.try_recv();
    EXPECT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), ChannelError::SenderDropped);
}

TYPED_TEST(MpscTest, TryRecvFifoOrder) {
    auto [tx, rx] = mpsc_channel<int>(8);
    for (int i = 0; i < 5; ++i) tx.try_send(i);
    for (int i = 0; i < 5; ++i) {
        auto r = rx.try_recv();
        EXPECT_TRUE(r.has_value());
        EXPECT_EQ(*r, i);
    }
}

TYPED_TEST(MpscTest, IsClosed) {
    auto [tx, rx] = mpsc_channel<int>(4);
    EXPECT_FALSE(tx.is_closed());
    { auto dropped = std::move(rx); }
    EXPECT_TRUE(tx.is_closed());
}

TYPED_TEST(MpscTest, AllSendersDropped) {
    auto [tx, rx] = mpsc_channel<int>(4);
    EXPECT_FALSE(rx.all_senders_dropped());
    auto tx2 = tx.clone();
    { auto dropped = std::move(tx); }
    EXPECT_FALSE(rx.all_senders_dropped());
    { auto dropped = std::move(tx2); }
    EXPECT_TRUE(rx.all_senders_dropped());
}

// Move-assigning into a live MpscSender must drop the OLD channel's sender
// count (running the destructor's protocol on it), not just silently drop
// the old Rc. Regression test for the bug that caused a displaced
// MqttClient::subscribe() stream to hang instead of ending.
TYPED_TEST(MpscTest, ReassigningLiveSenderDropsOldChannelSender) {
    auto chan_a = mpsc_channel<int>(4);
    auto tx_a   = std::move(chan_a.first);
    auto rx_a   = std::move(chan_a.second);
    auto chan_b = mpsc_channel<int>(4);
    auto tx_b   = std::move(chan_b.first);

    tx_a = std::move(tx_b); // displaces the sender for channel A

    EXPECT_TRUE(rx_a.all_senders_dropped());
}

// --- Async executor-agnostic tests ---

TYPED_TEST(MpscTest, SingleSenderSingleReceiver) {
    std::vector<int> received;
    this->traits.rt.block_on([](std::vector<int>& received) -> Coro<void> {
        // GCC bug: structured bindings in coroutines leak when spanning a suspension.
        auto _ch = mpsc_channel<int>(4);
        auto tx = std::move(_ch.first);
        auto rx = std::move(_ch.second);
        co_await tx.send(1);
        co_await tx.send(2);
        co_await tx.send(3);
        { auto dropped = std::move(tx); }
        while (auto v = co_await next(rx))
            received.push_back(*v);
    }(received));
    EXPECT_EQ(received, (std::vector<int>{1, 2, 3}));
}

TYPED_TEST(MpscTest, MultipleSenders) {
    std::vector<int> received;
    this->traits.rt.block_on([](std::vector<int>& received) -> Coro<void> {
        auto _ch = mpsc_channel<int>(8);
        auto tx = std::move(_ch.first);
        auto rx = std::move(_ch.second);
        auto tx2 = tx.clone();
        co_await tx.send(1);
        co_await tx2.send(2);
        { auto d1 = std::move(tx); auto d2 = std::move(tx2); }
        while (auto v = co_await next(rx))
            received.push_back(*v);
    }(received));
    EXPECT_EQ(received.size(), 2u);
}

TYPED_TEST(MpscTest, ReceiverExhaustedAfterAllSendersDropped) {
    this->traits.rt.block_on([]() -> Coro<void> {
        auto _ch = mpsc_channel<int>(4);
        auto tx = std::move(_ch.first);
        auto rx = std::move(_ch.second);
        { auto dropped = std::move(tx); }
        auto v = co_await next(rx);
        EXPECT_FALSE(v.has_value());
    }());
}

TYPED_TEST(MpscTest, ReceiverDroppedReturnsSendError) {
    this->traits.rt.block_on([]() -> Coro<void> {
        auto _ch = mpsc_channel<int>(1);
        auto tx = std::move(_ch.first);
        auto rx = std::move(_ch.second);
        co_await tx.send(1);
        { auto dropped = std::move(rx); }
        auto r = co_await tx.send(2);
        EXPECT_FALSE(r.has_value());
        EXPECT_EQ(r.error(), 2);
    }());
}

TYPED_TEST(MpscTest, ErrorChannelPattern) {
    this->traits.rt.block_on([]() -> Coro<void> {
        auto _ch = mpsc_channel<std::expected<int, std::string>>(4);
        auto tx = std::move(_ch.first);
        auto rx = std::move(_ch.second);
        co_await tx.send(42);
        co_await tx.send(std::unexpected(std::string("oops")));
        { auto dropped = std::move(tx); }
        auto r1 = co_await next(rx);
        EXPECT_TRUE(r1.has_value() && r1->has_value());
        EXPECT_EQ(**r1, 42);
        auto r2 = co_await next(rx);
        EXPECT_TRUE(r2.has_value() && !r2->has_value());
        EXPECT_EQ(r2->error(), "oops");
    }());
}

TYPED_TEST(MpscTest, RecvReceivesValue) {
    this->traits.rt.block_on([]() -> Coro<void> {
        auto _ch = mpsc_channel<int>(4);
        auto tx = std::move(_ch.first);
        auto rx = std::move(_ch.second);
        co_await tx.send(42);
        { auto dropped = std::move(tx); }
        auto v = co_await rx.recv();
        EXPECT_TRUE(v.has_value());
        EXPECT_EQ(*v, 42);
        auto end = co_await rx.recv();
        EXPECT_FALSE(end.has_value());
    }());
}

TYPED_TEST(MpscTest, RecvCanBeReusedAfterCancelledSelect) {
    this->traits.rt.block_on([]() -> Coro<void> {
        auto _ch = mpsc_channel<int>(4);
        auto tx = std::move(_ch.first);
        auto rx = std::move(_ch.second);
        struct ImmediateInt {
            using OutputType = int;
            PollResult<int> poll(detail::Context&) { return 99; }
        };
        auto r1 = co_await select(rx.recv(), ImmediateInt{});
        EXPECT_EQ(r1.index(), 1u);
        tx.try_send(7);
        auto v = co_await rx.recv();
        EXPECT_TRUE(v.has_value());
        EXPECT_EQ(*v, 7);
    }());
}

TYPED_TEST(MpscTest, ZeroCopyNoDoubleSendInt) {
    std::vector<int> received;
    this->traits.rt.block_on([](std::vector<int>& out) -> Coro<void> {
        auto _ch = mpsc_channel<int>(1);
        auto tx = std::move(_ch.first);
        auto rx = std::move(_ch.second);
        auto producer = coro::spawn([](MpscSender<int> tx) -> Coro<void> {
            for (int i = 0; i < 5; ++i) co_await tx.send(i);
        }(std::move(tx)));
        while (auto v = co_await next(rx)) out.push_back(*v);
        co_await producer;
    }(received));
    EXPECT_EQ(received, (std::vector<int>{0, 1, 2, 3, 4}));
}

TYPED_TEST(MpscTest, ZeroCopyNoDoubleSendString) {
    std::vector<std::string> received;
    this->traits.rt.block_on([](std::vector<std::string>& out) -> Coro<void> {
        auto _ch = mpsc_channel<std::string>(1);
        auto tx = std::move(_ch.first);
        auto rx = std::move(_ch.second);
        auto producer = coro::spawn([](MpscSender<std::string> tx) -> Coro<void> {
            for (int i = 0; i < 5; ++i) co_await tx.send("value_" + std::to_string(i));
        }(std::move(tx)));
        while (auto v = co_await next(rx)) out.push_back(std::move(*v));
        co_await producer;
    }(received));
    EXPECT_EQ(received, (std::vector<std::string>{"value_0","value_1","value_2","value_3","value_4"}));
}

TYPED_TEST(MpscTest, ZeroCopyExactItemCount) {
    int count = 0;
    this->traits.rt.block_on([](int& cnt) -> Coro<void> {
        auto _ch = mpsc_channel<int>(1);
        auto tx = std::move(_ch.first);
        auto rx = std::move(_ch.second);
        auto producer = coro::spawn([](MpscSender<int> tx) -> Coro<void> {
            for (int i = 0; i < 10; ++i) co_await tx.send(i);
        }(std::move(tx)));
        while (co_await next(rx)) ++cnt;
        co_await producer;
    }(count));
    EXPECT_EQ(count, 10);
}

TYPED_TEST(MpscTest, CapacityZeroDirectZeroCopyPath) {
    std::vector<int> received;
    this->traits.rt.block_on([](std::vector<int>& out) -> Coro<void> {
        auto _ch = mpsc_channel<int>(0);
        auto tx = std::move(_ch.first);
        auto rx = std::move(_ch.second);
        auto producer = coro::spawn([](MpscSender<int> tx) -> Coro<void> {
            for (int i = 0; i < 5; ++i) co_await tx.send(i);
        }(std::move(tx)));
        while (auto v = co_await next(rx)) out.push_back(*v);
        co_await producer;
    }(received));
    EXPECT_EQ(received, (std::vector<int>{0, 1, 2, 3, 4}));
}

TYPED_TEST(MpscTest, MultipleSendersCapacity1NoDuplicates) {
    int sum = 0, count = 0;
    this->traits.rt.block_on([](int& s, int& c) -> Coro<void> {
        auto _ch = mpsc_channel<int>(1);
        auto tx = std::move(_ch.first);
        auto rx = std::move(_ch.second);
        auto tx2 = tx.clone();
        auto tx3 = tx.clone();
        auto s1 = coro::spawn([](MpscSender<int> t) -> Coro<void> {
            co_await t.send(10); co_await t.send(11);
        }(std::move(tx)));
        auto s2 = coro::spawn([](MpscSender<int> t) -> Coro<void> {
            co_await t.send(20); co_await t.send(21);
        }(std::move(tx2)));
        auto s3 = coro::spawn([](MpscSender<int> t) -> Coro<void> {
            co_await t.send(30); co_await t.send(31);
        }(std::move(tx3)));
        while (auto v = co_await next(rx)) { s += *v; ++c; }
        co_await s1; co_await s2; co_await s3;
    }(sum, count));
    EXPECT_EQ(count, 6);
    EXPECT_EQ(sum, 10 + 11 + 20 + 21 + 30 + 31);
}

// ---------------------------------------------------------------------------
// Single-threaded-specific tests (rely on deterministic scheduling order)
// ---------------------------------------------------------------------------

#ifndef CORO_PICO

TEST(MpscSingleThreadedTest, SenderSuspendsWhenFull) {
    Runtime rt(1);
    rt.block_on([]() -> Coro<void> {
        int x[3] = {0, 0, 0};
        auto _ch = mpsc_channel<int*>(2);
        auto tx = std::move(_ch.first);
        auto rx = std::move(_ch.second);
        co_await tx.send(&x[0]);
        co_await tx.send(&x[1]);
        auto h = coro::spawn(coro::co_invoke([rx = std::move(rx)]() mutable -> Coro<void> {
            auto v1 = co_await next(rx); ++(*v1.value());
            auto v2 = co_await next(rx); ++(*v2.value());
            auto v3 = co_await next(rx); ++(*v3.value());
        }));
        EXPECT_EQ(x[0], 0);
        EXPECT_EQ(x[1], 0);
        EXPECT_EQ(x[2], 0);
        co_await tx.send(&x[2]);
        EXPECT_EQ(x[0], 1);
        EXPECT_EQ(x[1], 1);
        EXPECT_EQ(x[2], 1);
        co_await h;
    }());
}

TEST(MpscSingleThreadedTest, RecvFutureDirectZeroCopyNoDuplicates) {
    Runtime rt(1);
    std::vector<int> received;
    rt.block_on([](std::vector<int>& out) -> Coro<void> {
        auto _ch = mpsc_channel<int>(0);
        auto tx = std::move(_ch.first);
        auto rx = std::move(_ch.second);
        auto producer = coro::spawn([](MpscSender<int> tx) -> Coro<void> {
            for (int i = 0; i < 3; ++i) co_await tx.send(i);
        }(std::move(tx)));
        auto noop = coro::spawn([](int n) -> Coro<int> { co_return n; }(0));
        co_await noop;
        while (auto v = co_await rx.recv()) out.push_back(*v);
        co_await producer;
    }(received));
    EXPECT_EQ(received, (std::vector<int>{0, 1, 2}));
}

TEST(MpscSingleThreadedTest, ReceiverSuspendsBeforeSenderSends) {
    Runtime rt(1);
    int received = -1;
    rt.block_on([](int& out) -> Coro<void> {
        auto _ch = mpsc_channel<int>(4);
        auto tx = std::move(_ch.first);
        auto rx = std::move(_ch.second);
        auto sender = coro::spawn([](MpscSender<int> tx) -> Coro<void> {
            co_await tx.send(42);
        }(std::move(tx)));
        auto v = co_await rx.recv();
        EXPECT_TRUE(v.has_value());
        out = v.value_or(-1);
        co_await sender;
    }(received));
    EXPECT_EQ(received, 42);
}

TEST(MpscSingleThreadedTest, TrySendWakesAsyncReceiver) {
    Runtime rt(1);
    int received = -1;
    rt.block_on([](int& out) -> Coro<void> {
        auto _ch = mpsc_channel<int>(4);
        auto tx = std::move(_ch.first);
        auto rx = std::move(_ch.second);
        auto sender = coro::spawn([](MpscSender<int> tx) -> Coro<void> {
            tx.try_send(77);
            co_return;
        }(std::move(tx)));
        auto v = co_await rx.recv();
        out = v.value_or(-1);
        co_await sender;
    }(received));
    EXPECT_EQ(received, 77);
}

TEST(MpscSingleThreadedTest, ReceiverDroppedWhileSenderInWaiters) {
    Runtime rt(1);
    bool got_error = false;
    rt.block_on([](bool& err) -> Coro<void> {
        auto _ch = mpsc_channel<int>(1);
        auto tx = std::move(_ch.first);
        auto rx = std::move(_ch.second);
        co_await tx.send(1);
        auto h = coro::spawn([](MpscSender<int> tx, bool& err) -> Coro<void> {
            auto r = co_await tx.send(2);
            err = !r.has_value() && r.error() == 2;
        }(tx.clone(), err));
        auto noop = coro::spawn([](int n) -> Coro<int> { co_return n; }(0));
        co_await noop;
        { auto dropped = std::move(rx); }
        co_await h;
    }(got_error));
    EXPECT_TRUE(got_error);
}

// ---------------------------------------------------------------------------
// spawn_blocking tests — desktop only
// ---------------------------------------------------------------------------

TEST(MpscBlockingTest, BlockingRecvGetsPreloadedValue) {
    Runtime rt(1);
    int result = -1;
    rt.block_on([](int& out) -> Coro<void> {
        auto _ch = mpsc_channel<int>(4);
        auto tx = std::move(_ch.first);
        auto rx = std::move(_ch.second);
        co_await tx.send(42);
        out = co_await coro::spawn_blocking(
            [rx = std::move(rx)]() mutable -> int {
                auto v = rx.blocking_recv();
                return v ? *v : -1;
            });
    }(result));
    EXPECT_EQ(result, 42);
}

TEST(MpscBlockingTest, BlockingRecvBlocksUntilSent) {
    Runtime rt(1);
    int result = -1;
    rt.block_on([](int& out) -> Coro<void> {
        auto _ch = mpsc_channel<int>(4);
        auto tx = std::move(_ch.first);
        auto rx = std::move(_ch.second);
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

TEST(MpscBlockingTest, BlockingRecvReturnsNulloptWhenAllSendersDropped) {
    Runtime rt(1);
    bool got_nullopt = false;
    rt.block_on([](bool& out) -> Coro<void> {
        auto _ch = mpsc_channel<int>(4);
        auto tx = std::move(_ch.first);
        auto rx = std::move(_ch.second);
        { auto dropped = std::move(tx); }
        out = co_await coro::spawn_blocking(
            [rx = std::move(rx)]() mutable -> bool {
                return !rx.blocking_recv().has_value();
            });
    }(got_nullopt));
    EXPECT_TRUE(got_nullopt);
}

TEST(MpscBlockingTest, BlockingSendDeliversToAsyncReceiver) {
    Runtime rt(1);
    int result = -1;
    rt.block_on([](int& out) -> Coro<void> {
        auto _ch = mpsc_channel<int>(4);
        auto tx = std::move(_ch.first);
        auto rx = std::move(_ch.second);
        auto handle = coro::spawn_blocking(
            [tx = std::move(tx)]() mutable { tx.blocking_send(77); });
        auto v = co_await next(rx);
        out = *v;
        co_await std::move(handle);
    }(result));
    EXPECT_EQ(result, 77);
}

TEST(MpscBlockingTest, BlockingSendBlocksUntilSpace) {
    Runtime rt(1);
    std::vector<int> received;
    rt.block_on([](std::vector<int>& out) -> Coro<void> {
        auto _ch = mpsc_channel<int>(2);
        auto tx = std::move(_ch.first);
        auto rx = std::move(_ch.second);
        co_await tx.send(1);
        co_await tx.send(2);
        auto handle = coro::spawn_blocking(
            [tx = std::move(tx)]() mutable { tx.blocking_send(3); });
        auto v = co_await next(rx);
        out.push_back(*v);
        co_await std::move(handle);
        while (auto w = co_await next(rx)) out.push_back(*w);
    }(received));
    EXPECT_EQ(received, (std::vector<int>{1, 2, 3}));
}

TEST(MpscBlockingTest, BlockingSendReturnsUnsentValueWhenReceiverDropped) {
    Runtime rt(1);
    std::optional<int> unsent;
    rt.block_on([](std::optional<int>& out) -> Coro<void> {
        auto _ch = mpsc_channel<int>(1);
        auto tx = std::move(_ch.first);
        auto rx = std::move(_ch.second);
        co_await tx.send(1);
        { auto dropped = std::move(rx); }
        out = co_await coro::spawn_blocking(
            [tx = std::move(tx)]() mutable -> std::optional<int> {
                auto r = tx.blocking_send(2);
                return r.has_value() ? std::nullopt : std::optional<int>{r.error()};
            });
    }(unsent));
    EXPECT_TRUE(unsent.has_value());
    EXPECT_EQ(*unsent, 2);
}

TEST(MpscBlockingTest, BlockingWorkerPipeline) {
    Runtime rt(1);
    std::vector<int> results;
    rt.block_on([](std::vector<int>& out) -> Coro<void> {
        auto _ch_in  = mpsc_channel<int>(4);
        auto in_tx   = std::move(_ch_in.first);
        auto in_rx   = std::move(_ch_in.second);
        auto _ch_out = mpsc_channel<int>(4);
        auto out_tx  = std::move(_ch_out.first);
        auto out_rx  = std::move(_ch_out.second);
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
        while (auto v = co_await next(out_rx)) out.push_back(*v);
        co_await std::move(worker);
    }(results));
    EXPECT_EQ(results, (std::vector<int>{2, 4, 6}));
}

TEST(MpscBlockingTest, BlockingRecvZeroCopyNoDuplicates) {
    Runtime rt(1);
    std::vector<int> received;
    rt.block_on([](std::vector<int>& out) -> Coro<void> {
        auto _ch = mpsc_channel<int>(1);
        auto tx = std::move(_ch.first);
        auto rx = std::move(_ch.second);
        auto producer = coro::spawn([](MpscSender<int> tx) -> Coro<void> {
            for (int i = 0; i < 4; ++i) co_await tx.send(i);
        }(std::move(tx)));
        auto worker = coro::spawn_blocking(
            [rx = std::move(rx)]() mutable -> std::vector<int> {
                std::vector<int> vals;
                while (auto v = rx.blocking_recv()) vals.push_back(*v);
                return vals;
            });
        out = co_await std::move(worker);
        co_await producer;
    }(received));
    EXPECT_EQ(received.size(), 4u);
    EXPECT_EQ(received, (std::vector<int>{0, 1, 2, 3}));
}

#endif  // !CORO_PICO
