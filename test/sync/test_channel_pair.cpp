// ChannelPair, what every channel factory returns, and the GCC 15 leak it works around
// (doc/known_issues.md, KI.2): a structured binding through the tuple protocol, as a
// std::pair's is, never has its hidden object destroyed when that object lives in a
// coroutine frame across a suspension. Each test here binds a channel's ends that way,
// keeps the sending end bound across a suspension, and checks from outside that it was
// destroyed with the frame. With a std::pair they fail on GCC 15 at -O1 and above.

#include <gtest/gtest.h>
#include "executor_traits.h"
#include "tuple_protocol.h"
#include <coro/coro.h>
#include <coro/coro_stream.h>
#include <coro/stream.h>
#include <coro/sync/broadcast.h>
#include <coro/sync/channel_pair.h>
#include <coro/sync/mpsc.h>
#include <coro/sync/oneshot.h>
#include <coro/sync/watch.h>
#include <coro/task/yield_now.h>
#include <coro/runtime/runtime.h>
#include <cstddef>
#include <optional>
#include <type_traits>
#include <utility>

using namespace coro;

namespace {

// --- Compile-time: the factories return ChannelPair, which has no tuple protocol ---

static_assert(std::is_same_v<decltype(mpsc_channel<int>(1)), ChannelPair<MpscSender<int>, MpscReceiver<int>>>);
static_assert(std::is_same_v<decltype(oneshot_channel<int>()), ChannelPair<OneshotSender<int>, OneshotReceiver<int>>>);
static_assert(std::is_same_v<decltype(watch_channel<int>(0)), ChannelPair<WatchSender<int>, WatchReceiver<int>>>);
static_assert(std::is_same_v<decltype(broadcast_channel<int>(1)),
                             ChannelPair<BroadcastSender<int>, BroadcastReceiver<int>>>);

// The tuple protocol is what triggers the GCC bug, so ChannelPair must never gain one.
static_assert(coro_test::kBindsMemberwise<ChannelPair<MpscSender<int>, MpscReceiver<int>>>);

// --- Helpers: each binds a channel in a stream's frame, hands the receiving end out, and
// yields with the sending end still bound, so the binding's hidden object is in the frame
// across the yield ---

CoroStream<int> bind_mpsc(std::optional<MpscReceiver<int>>& out) {
    auto [tx, rx] = mpsc_channel<int>(1);
    out.emplace(std::move(rx));
    co_yield 0;
    (void)tx;
}

CoroStream<int> bind_oneshot(std::optional<OneshotReceiver<int>>& out) {
    auto [tx, rx] = oneshot_channel<int>();
    out.emplace(std::move(rx));
    co_yield 0;
    (void)tx;
}

CoroStream<int> bind_watch(std::optional<WatchReceiver<int>>& out) {
    auto [tx, rx] = watch_channel<int>(0);
    out.emplace(std::move(rx));
    co_yield 0;
    (void)tx;
}

CoroStream<int> bind_broadcast(std::optional<BroadcastReceiver<int>>& out) {
    auto [tx, rx] = broadcast_channel<int>(1);
    out.emplace(std::move(rx));
    co_yield 0;
    (void)tx;
}

// Takes the stream's first item and drops the stream, destroying its frame at the yield.
Coro<void> take_first(CoroStream<int> stream) {
    (void)co_await next(stream);
}

// The same through co_await in a Coro, run to its end.
Coro<void> bind_mpsc_across_await(std::optional<MpscReceiver<int>>& out) {
    auto [tx, rx] = mpsc_channel<int>(1);
    out.emplace(std::move(rx));
    co_await yield_now();
    (void)tx;
}

} // namespace

template<typename Traits>
class ChannelPairTest : public testing::Test {
protected:
    Traits traits;
};
TYPED_TEST_SUITE(ChannelPairTest, AllExecutors);

TYPED_TEST(ChannelPairTest, MpscSenderBoundInAStreamIsDestroyedWithIt) {
    std::optional<MpscReceiver<int>> rx;
    this->traits.rt.block_on(take_first(bind_mpsc(rx)));
    ASSERT_TRUE(rx.has_value());
    auto r = rx->try_recv();
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), ChannelError::SenderDropped);
}

TYPED_TEST(ChannelPairTest, OneshotSenderBoundInAStreamIsDestroyedWithIt) {
    std::optional<OneshotReceiver<int>> rx;
    this->traits.rt.block_on(take_first(bind_oneshot(rx)));
    ASSERT_TRUE(rx.has_value());
    EXPECT_TRUE(rx->sender_dropped());
}

TYPED_TEST(ChannelPairTest, WatchSenderBoundInAStreamIsDestroyedWithIt) {
    std::optional<WatchReceiver<int>> rx;
    this->traits.rt.block_on(take_first(bind_watch(rx)));
    ASSERT_TRUE(rx.has_value());
    EXPECT_TRUE(rx->sender_dropped());
}

TYPED_TEST(ChannelPairTest, BroadcastSenderBoundInAStreamIsDestroyedWithIt) {
    std::optional<BroadcastReceiver<int>> rx;
    this->traits.rt.block_on(take_first(bind_broadcast(rx)));
    ASSERT_TRUE(rx.has_value());
    auto r = rx->try_recv();
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, BroadcastRecvError::Kind::Closed);
}

TYPED_TEST(ChannelPairTest, MpscSenderBoundAcrossAnAwaitIsDestroyed) {
    std::optional<MpscReceiver<int>> rx;
    this->traits.rt.block_on(bind_mpsc_across_await(rx));
    ASSERT_TRUE(rx.has_value());
    auto r = rx->try_recv();
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), ChannelError::SenderDropped);
}
