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

// ---------------------------------------------------------------------------
// Zero-copy correctness
//
// When a sender is waiting in sender_waiters and the receiver consumes its
// value (either directly or via _tryPromoteSender), the sender is woken and
// re-polled by the executor's spurious-wake guard. The re-poll must return
// Ready immediately without pushing the already-consumed value a second time.
//
// These tests catch bugs where node->value is not reset after the zero-copy
// handoff, causing duplicate delivery.
// ---------------------------------------------------------------------------

// capacity=1: tight buffer forces both the _tryPromoteSender and direct
// zero-copy paths. Each value must appear exactly once.
TEST(MpscTest, ZeroCopyNoDoubleSendInt) {
    Runtime rt(1);
    std::vector<int> received;
    rt.block_on([](std::vector<int>& out) -> Coro<void> {
        auto [tx, rx] = mpsc_channel<int>(1);
        auto producer = coro::spawn([](MpscSender<int> tx) -> Coro<void> {
            for (int i = 0; i < 5; ++i)
                co_await tx.send(i);
        }(std::move(tx)));
        while (auto v = co_await next(rx))
            out.push_back(*v);
        co_await producer;
    }(received));
    EXPECT_EQ(received, (std::vector<int>{0, 1, 2, 3, 4}));
}

// Non-primitive type: a moved-from std::string is distinct from its original,
// so duplicates show up as empty strings rather than correct values.
TEST(MpscTest, ZeroCopyNoDoubleSendString) {
    Runtime rt(1);
    std::vector<std::string> received;
    rt.block_on([](std::vector<std::string>& out) -> Coro<void> {
        auto [tx, rx] = mpsc_channel<std::string>(1);
        auto producer = coro::spawn([](MpscSender<std::string> tx) -> Coro<void> {
            for (int i = 0; i < 5; ++i)
                co_await tx.send("value_" + std::to_string(i));
        }(std::move(tx)));
        while (auto v = co_await next(rx))
            out.push_back(std::move(*v));
        co_await producer;
    }(received));
    EXPECT_EQ(received, (std::vector<std::string>{
        "value_0", "value_1", "value_2", "value_3", "value_4"}));
}

// Verify item count is exactly N — catches duplicates even if values happen
// to be identical.
TEST(MpscTest, ZeroCopyExactItemCount) {
    Runtime rt(1);
    int count = 0;
    rt.block_on([](int& cnt) -> Coro<void> {
        auto [tx, rx] = mpsc_channel<int>(1);
        auto producer = coro::spawn([](MpscSender<int> tx) -> Coro<void> {
            for (int i = 0; i < 10; ++i)
                co_await tx.send(i);
        }(std::move(tx)));
        while (co_await next(rx))
            ++cnt;
        co_await producer;
    }(count));
    EXPECT_EQ(count, 10);
}

// capacity=0: buffer-less channel — every send goes directly to sender_waiters,
// exercising the direct zero-copy path in poll_next on every receive.
TEST(MpscTest, CapacityZeroDirectZeroCopyPath) {
    Runtime rt(1);
    std::vector<int> received;
    rt.block_on([](std::vector<int>& out) -> Coro<void> {
        auto [tx, rx] = mpsc_channel<int>(0);
        auto producer = coro::spawn([](MpscSender<int> tx) -> Coro<void> {
            for (int i = 0; i < 5; ++i)
                co_await tx.send(i);
        }(std::move(tx)));
        while (auto v = co_await next(rx))
            out.push_back(*v);
        co_await producer;
    }(received));
    EXPECT_EQ(received, (std::vector<int>{0, 1, 2, 3, 4}));
}

// Multiple senders at capacity=1: every sender that suspends in sender_waiters
// must deliver its value exactly once.
TEST(MpscTest, MultipleSendersCapacity1NoDuplicates) {
    Runtime rt(1);
    int sum = 0, count = 0;
    rt.block_on([](int& s, int& c) -> Coro<void> {
        auto [tx, rx] = mpsc_channel<int>(1);
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
// MpscRecvFuture (rx.recv()) code paths
//
// Most tests above use coro::next(rx) which routes through MpscReceiver::poll_next().
// The following tests exercise MpscRecvFuture::poll() via co_await rx.recv().
//
// Three distinct paths in MpscRecvFuture::poll():
//   (a) Buffer non-empty  → take from buffer, call _tryPromoteSender.
//   (b) sender_waiters non-empty → direct zero-copy from suspended sender.
//   (c) Both empty, sender alive → set receiver_waiter, return Pending.
//
// Path (b) requires the sender to already be suspended when the receiver polls.
// In a single-threaded executor this is achieved by spawning a quick no-op task
// between the sender spawn and the recv call, which yields execution to the
// sender (letting it suspend in sender_waiters) before the receiver polls.
// ---------------------------------------------------------------------------

// Path (b): direct zero-copy from sender_waiters. Without node->value.reset() in
// MpscRecvFuture::poll(), the woken sender re-polls and pushes the moved-from
// value a second time — same double-delivery bug fixed elsewhere.
TEST(MpscTest, RecvFutureDirectZeroCopyNoDuplicates) {
    Runtime rt(1);
    std::vector<int> received;
    rt.block_on([](std::vector<int>& out) -> Coro<void> {
        auto [tx, rx] = mpsc_channel<int>(0);  // capacity=0: sender always suspends

        auto producer = coro::spawn([](MpscSender<int> tx) -> Coro<void> {
            for (int i = 0; i < 3; ++i)
                co_await tx.send(i);
        }(std::move(tx)));

        // Yield so producer runs first and suspends in sender_waiters before recv.
        auto noop = coro::spawn([](int n) -> Coro<int> { co_return n; }(0));
        co_await noop;

        // producer is now in sender_waiters; rx.recv() hits path (b).
        while (auto v = co_await rx.recv())
            out.push_back(*v);
        co_await producer;
    }(received));
    EXPECT_EQ(received, (std::vector<int>{0, 1, 2}));
}

// Path (c) → path (b): receiver suspends first (receiver_waiter set), then spawned
// sender sends and observes receiver_waiter inside MpscSendFuture::poll().
// That path pushes the value to the buffer and wakes the receiver; receiver then
// takes it via path (a). Exercises the receiver_waiter branch of MpscSendFuture.
TEST(MpscTest, ReceiverSuspendsBeforeSenderSends) {
    Runtime rt(1);
    int received = -1;
    rt.block_on([](int& out) -> Coro<void> {
        auto [tx, rx] = mpsc_channel<int>(4);

        // Sender is queued but hasn't run yet.
        auto sender = coro::spawn([](MpscSender<int> tx) -> Coro<void> {
            co_await tx.send(42);
        }(std::move(tx)));

        // recv() on empty channel → path (c): receiver_waiter set, parent suspends.
        // Executor now runs the sender, which sees receiver_waiter → wakes parent.
        auto v = co_await rx.recv();
        EXPECT_TRUE(v.has_value());
        out = v.value_or(-1);
        co_await sender;
    }(received));
    EXPECT_EQ(received, 42);
}

// try_send() must also wake an async-suspended receiver via receiver_waiter.
TEST(MpscTest, TrySendWakesAsyncReceiver) {
    Runtime rt(1);
    int received = -1;
    rt.block_on([](int& out) -> Coro<void> {
        auto [tx, rx] = mpsc_channel<int>(4);

        auto sender = coro::spawn([](MpscSender<int> tx) -> Coro<void> {
            tx.try_send(77);  // non-async; sees receiver_waiter → wakes parent
            co_return;
        }(std::move(tx)));

        auto v = co_await rx.recv();  // sets receiver_waiter → suspends
        out = v.value_or(-1);
        co_await sender;
    }(received));
    EXPECT_EQ(received, 77);
}

// ~MpscReceiver() must wake all senders suspended in sender_waiters. The woken
// sender re-polls, sees !receiver_alive, and returns the unsent value as an error.
TEST(MpscTest, ReceiverDroppedWhileSenderInWaiters) {
    Runtime rt(1);
    bool got_error = false;
    rt.block_on([](bool& err) -> Coro<void> {
        auto [tx, rx] = mpsc_channel<int>(1);
        co_await tx.send(1);  // fill buffer (capacity=1)

        auto h = coro::spawn([](MpscSender<int> tx, bool& err) -> Coro<void> {
            auto r = co_await tx.send(2);  // buffer full → suspends in sender_waiters
            err = !r.has_value() && r.error() == 2;
        }(tx.clone(), err));

        // Yield so h runs and suspends in sender_waiters before rx is dropped.
        auto noop = coro::spawn([](int n) -> Coro<int> { co_return n; }(0));
        co_await noop;

        // h is now in sender_waiters; dropping rx wakes it with an error.
        { auto dropped = std::move(rx); }
        co_await h;
    }(got_error));
    EXPECT_TRUE(got_error);
}

// ---------------------------------------------------------------------------
// State observer methods
// ---------------------------------------------------------------------------

TEST(MpscTest, IsClosed) {
    auto [tx, rx] = mpsc_channel<int>(4);
    EXPECT_FALSE(tx.is_closed());
    { auto dropped = std::move(rx); }
    EXPECT_TRUE(tx.is_closed());
}

TEST(MpscTest, AllSendersDropped) {
    auto [tx, rx] = mpsc_channel<int>(4);
    EXPECT_FALSE(rx.all_senders_dropped());
    auto tx2 = tx.clone();
    { auto dropped = std::move(tx); }
    EXPECT_FALSE(rx.all_senders_dropped());  // tx2 still alive
    { auto dropped = std::move(tx2); }
    EXPECT_TRUE(rx.all_senders_dropped());
}

// blocking_recv: zero-copy path (buffer empty, sender in sender_waiters) must
// not cause the woken sender to push its already-consumed value again.
TEST(MpscTest, BlockingRecvZeroCopyNoDuplicates) {
    Runtime rt(1);
    std::vector<int> received;
    rt.block_on([](std::vector<int>& out) -> Coro<void> {
        auto [tx, rx] = mpsc_channel<int>(1);

        // Async producer: first send fills the buffer, subsequent sends suspend.
        auto producer = coro::spawn([](MpscSender<int> tx) -> Coro<void> {
            for (int i = 0; i < 4; ++i)
                co_await tx.send(i);
        }(std::move(tx)));

        auto worker = coro::spawn_blocking(
            [rx = std::move(rx)]() mutable -> std::vector<int> {
                std::vector<int> vals;
                while (auto v = rx.blocking_recv())
                    vals.push_back(*v);
                return vals;
            });

        out = co_await std::move(worker);
        co_await producer;
    }(received));
    EXPECT_EQ(received.size(), 4u);
    EXPECT_EQ(received, (std::vector<int>{0, 1, 2, 3}));
}
