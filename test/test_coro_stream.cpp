#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <coro/coro_stream.h>
#include <coro/stream.h>
#include <stdexcept>
#include <string>
#include <vector>

using namespace coro;
using namespace coro::detail;

namespace {

class MockWaker : public detail::Waker {
public:
    MOCK_METHOD(void, wake, (), (override));
    MOCK_METHOD(Rc<detail::Waker>, clone, (), (override));
};

// Helper: drain a CoroStream synchronously into a vector, asserting no errors.
template<typename T, bool HFV>
std::vector<T> drain(CoroStream<T, HFV>& s, detail::Context& ctx) {
    std::vector<T> out;
    while (true) {
        auto r = s.poll_next(ctx);
        EXPECT_FALSE(r.isError()) << "unexpected error in drain()";
        if (r.isPending()) break;
        if (!r.value()) break;
        out.push_back(std::move(*r.value()));
    }
    return out;
}

// --- Helper generators ---

CoroStream<int> empty_stream() {
    co_return;
}

CoroStream<int> range(int from, int to) {
    for (int i = from; i < to; ++i)
        co_yield i;
}

CoroStream<int, true> range_with_sum(int from, int to) {
    int sum = 0;
    for (int i = from; i < to; ++i) {
        sum += i;
        co_yield i;
    }
    co_return sum;
}

CoroStream<int> throws_on_first_yield() {
    throw std::runtime_error("stream boom");
    co_yield 0;
}

CoroStream<int> throws_after_one_yield() {
    co_yield 1;
    throw std::runtime_error("mid-stream boom");
}

// Generator that co_awaits an inner Future between yields.
CoroStream<int> awaiting_stream() {
    struct ImmediateFuture {
        using OutputType = int;
        int m_value;
        PollResult<int> poll(detail::Context&) { return m_value; }
    };
    int a = co_await ImmediateFuture{10};
    co_yield a;
    int b = co_await ImmediateFuture{20};
    co_yield b;
}

// --- Controllable futures, for the suspension and cancellation tests ---

// Set and read back by a test, through the future it controls.
struct Gate {
    bool open    = false;  // poll() completes once set
    int  polls   = 0;
    int  cancels = 0;      // CancellableFuture only
    bool drained = false;  // CancellableFuture: once cancelled, poll() reports Dropped when set
};

// Pending until its gate opens, then Ready(value). A leaf: no cancel(), so not Cancellable.
class GateFuture {
public:
    using OutputType = int;
    GateFuture(Gate& gate, int value) : m_gate(&gate), m_value(value) {}

    PollResult<int> poll(detail::Context&) {
        ++m_gate->polls;
        if (m_gate->open) return m_value;
        return PollPending;
    }

private:
    Gate* m_gate;
    int   m_value;
};

// A GateFuture with a cancel(), so Cancellable. Once cancelled it stays Pending until the gate's
// `drained` is set, as a future with children of its own to drain would, then reports Dropped.
class CancellableFuture {
public:
    using OutputType = int;
    CancellableFuture(Gate& gate, int value) : m_gate(&gate), m_value(value) {}

    void cancel() noexcept {
        ++m_gate->cancels;
        m_cancelled = true;
    }

    PollResult<int> poll(detail::Context&) {
        ++m_gate->polls;
        if (m_cancelled) {
            if (m_gate->drained) return PollDropped;
            return PollPending;
        }
        if (m_gate->open) return m_value;
        return PollPending;
    }

private:
    Gate* m_gate;
    int   m_value;
    bool  m_cancelled = false;
};

static_assert(!Cancellable<GateFuture>);
static_assert(Cancellable<CancellableFuture>);

// A local whose destructor counts, so a test sees when the frame holding it is destroyed.
struct FrameMarker {
    int* destroyed;
    ~FrameMarker() { ++*destroyed; }
};

// Awaits a Cancellable future, yields, then awaits a leaf. The leaf's await once left the
// first future's cancel hook set, pointing at its destroyed awaitable.
CoroStream<int> cancellable_then_leaf(Gate& first, Gate& second, int& destroyed) {
    FrameMarker marker{&destroyed};
    co_yield co_await CancellableFuture(first, 1);
    co_yield co_await GateFuture(second, 2);
}

// Yields what one leaf future produces, then ends.
CoroStream<int> leaf_then_end(Gate& gate, int& destroyed) {
    FrameMarker marker{&destroyed};
    co_yield co_await GateFuture(gate, 1);
}

CoroStream<int> sets_started(bool& started) {
    started = true;
    co_yield 1;
}

// Awaits a leaf, then yields twice without awaiting anything.
CoroStream<int> leaf_then_two_yields(Gate& gate) {
    co_yield co_await GateFuture(gate, 1);
    co_yield 2;
}

CoroStream<int> awaits_failing_future() {
    struct FailingFuture {
        using OutputType = int;
        PollResult<int> poll(detail::Context&) {
            return PollError(std::make_exception_ptr(std::runtime_error("inner boom")));
        }
    };
    co_yield co_await FailingFuture{};
}

CoroStream<int> awaits_dropped_future() {
    struct DroppedFuture {
        using OutputType = int;
        PollResult<int> poll(detail::Context&) { return PollDropped; }  // as a cancelled task's handle
    };
    co_yield co_await DroppedFuture{};
}

// --- Concept checks (compile-time) ---

static_assert(Stream<CoroStream<int>>);
static_assert(Stream<CoroStream<int, false>>);
static_assert(Stream<CoroStream<int, true>>);
static_assert(Stream<CoroStream<std::string>>);
static_assert(std::same_as<CoroStream<int>::ItemType, int>);
static_assert(std::same_as<CoroStream<int, true>::ItemType, int>);

}  // namespace

// --- Construction and move tests ---

TEST(CoroStreamTest, DefaultIsConstructible) {
    auto s = range(0, 5);
    (void)s;
}

TEST(CoroStreamTest, WithFinalValueIsConstructible) {
    auto s = range_with_sum(0, 5);
    (void)s;
}

TEST(CoroStreamTest, IsMovable) {
    auto s1 = range(0, 3);
    auto s2 = std::move(s1);
    (void)s2;
}

// --- Empty stream ---

TEST(CoroStreamTest, EmptyStreamReturnsNullopt) {
    auto waker = make_rc<MockWaker>();
    detail::Context ctx(waker);
    auto s = empty_stream();
    auto r = s.poll_next(ctx);
    EXPECT_TRUE(r.isReady());
    EXPECT_FALSE(r.value().has_value());
}

TEST(CoroStreamTest, ExhaustedStreamKeepsReturningNullopt) {
    auto waker = make_rc<MockWaker>();
    detail::Context ctx(waker);
    auto s = empty_stream();
    s.poll_next(ctx);  // exhaust
    auto r = s.poll_next(ctx);
    EXPECT_TRUE(r.isReady());
    EXPECT_FALSE(r.value().has_value());
}

// --- co_yield items ---

TEST(CoroStreamTest, RangeYieldsExpectedValues) {
    auto waker = make_rc<MockWaker>();
    detail::Context ctx(waker);
    auto s = range(0, 4);
    auto items = drain(s, ctx);
    EXPECT_THAT(items, ::testing::ElementsAre(0, 1, 2, 3));
}

TEST(CoroStreamTest, ExhaustionAfterLastItem) {
    auto waker = make_rc<MockWaker>();
    detail::Context ctx(waker);
    auto s = range(0, 2);
    drain(s, ctx);
    auto r = s.poll_next(ctx);  // one more after exhaustion
    EXPECT_TRUE(r.isReady());
    EXPECT_FALSE(r.value().has_value());
}

// --- co_return value (HasFinalValue = true) ---

TEST(CoroStreamTest, FinalValueEmittedAsLastItem) {
    auto waker = make_rc<MockWaker>();
    detail::Context ctx(waker);
    auto s = range_with_sum(1, 4);  // yields 1, 2, 3 then co_return 6
    auto items = drain(s, ctx);
    // yields: 1, 2, 3; final value: 6 (1+2+3)
    EXPECT_THAT(items, ::testing::ElementsAre(1, 2, 3, 6));
}

TEST(CoroStreamTest, FinalValueFollowedByNullopt) {
    auto waker = make_rc<MockWaker>();
    detail::Context ctx(waker);
    auto s = range_with_sum(0, 1);  // co_yield 0, then co_return 0 (sum)
    s.poll_next(ctx);               // item: 0
    s.poll_next(ctx);               // final value: 0 (the sum)
    auto r = s.poll_next(ctx);      // now exhausted
    EXPECT_TRUE(r.isReady());
    EXPECT_FALSE(r.value().has_value());
}

// --- Exception propagation ---

TEST(CoroStreamTest, ExceptionBeforeYieldStoredAsError) {
    auto waker = make_rc<MockWaker>();
    detail::Context ctx(waker);
    auto s = throws_on_first_yield();
    auto r = s.poll_next(ctx);
    EXPECT_TRUE(r.isError());
    EXPECT_THROW(r.rethrowIfError(), std::runtime_error);
}

TEST(CoroStreamTest, ExceptionAfterYieldStoredAsError) {
    auto waker = make_rc<MockWaker>();
    detail::Context ctx(waker);
    auto s = throws_after_one_yield();

    auto r1 = s.poll_next(ctx);
    EXPECT_TRUE(r1.isReady());
    EXPECT_EQ(r1.value(), 1);

    auto r2 = s.poll_next(ctx);
    EXPECT_TRUE(r2.isError());
    EXPECT_THROW(r2.rethrowIfError(), std::runtime_error);
}

// --- co_await inside generator ---

TEST(CoroStreamTest, AwaitingStreamYieldsCorrectValues) {
    auto waker = make_rc<MockWaker>();
    detail::Context ctx(waker);
    auto s = awaiting_stream();
    auto items = drain(s, ctx);
    EXPECT_THAT(items, ::testing::ElementsAre(10, 20));
}

TEST(CoroStreamTest, InnerFutureErrorStoredAsError) {
    auto waker = make_rc<MockWaker>();
    detail::Context ctx(waker);
    auto s = awaits_failing_future();
    auto r = s.poll_next(ctx);
    EXPECT_TRUE(r.isError());
    EXPECT_THROW(r.rethrowIfError(), std::runtime_error);
}

TEST(CoroStreamTest, AwaitOfDroppedFutureStoredAsTaskCancelled) {
    auto waker = make_rc<MockWaker>();
    detail::Context ctx(waker);
    auto s = awaits_dropped_future();
    auto r = s.poll_next(ctx);
    ASSERT_TRUE(r.isError());
    EXPECT_THROW(r.rethrowIfError(), TaskCancelled);
}

// --- Suspension on a pending inner future ---

TEST(CoroStreamTest, SuspendsOnPendingInnerFutureThenResumes) {
    auto waker = make_rc<MockWaker>();
    detail::Context ctx(waker);
    Gate gate;
    int  destroyed = 0;
    auto s = leaf_then_end(gate, destroyed);

    EXPECT_TRUE(s.poll_next(ctx).isPending());
    gate.open = true;
    auto r = s.poll_next(ctx);
    ASSERT_TRUE(r.isReady());
    EXPECT_EQ(r.value(), 1);

    auto end = s.poll_next(ctx);
    ASSERT_TRUE(end.isReady());
    EXPECT_FALSE(end.value().has_value());
    EXPECT_EQ(destroyed, 1);
}

// A poll while the inner future is still pending (a spurious wake) must not resume the body.
TEST(CoroStreamTest, SpuriousWakeDoesNotResumeGenerator) {
    auto waker = make_rc<MockWaker>();
    detail::Context ctx(waker);
    Gate gate;
    int  destroyed = 0;
    auto s = leaf_then_end(gate, destroyed);

    EXPECT_TRUE(s.poll_next(ctx).isPending());
    EXPECT_TRUE(s.poll_next(ctx).isPending());
    EXPECT_EQ(gate.polls, 2);

    gate.open = true;
    auto r = s.poll_next(ctx);
    ASSERT_TRUE(r.isReady());
    EXPECT_EQ(r.value(), 1);
}

// Once a co_await has resumed, the next poll resumes from the co_yield after it without
// polling the finished future again.
TEST(CoroStreamTest, ResumeFromYieldDoesNotRepollFinishedFuture) {
    auto waker = make_rc<MockWaker>();
    detail::Context ctx(waker);
    Gate gate;
    auto s = leaf_then_two_yields(gate);

    EXPECT_TRUE(s.poll_next(ctx).isPending());
    gate.open = true;
    EXPECT_EQ(s.poll_next(ctx).value(), 1);
    const int polls = gate.polls;
    EXPECT_EQ(s.poll_next(ctx).value(), 2);
    EXPECT_EQ(gate.polls, polls);
}

TEST(CoroStreamTest, MovedWhileSuspendedResumes) {
    auto waker = make_rc<MockWaker>();
    detail::Context ctx(waker);
    Gate gate;
    int  destroyed = 0;
    auto s1 = leaf_then_end(gate, destroyed);

    EXPECT_TRUE(s1.poll_next(ctx).isPending());
    auto s2 = std::move(s1);
    gate.open = true;
    auto r = s2.poll_next(ctx);
    ASSERT_TRUE(r.isReady());
    EXPECT_EQ(r.value(), 1);
    EXPECT_EQ(destroyed, 0);
}

// --- Cancellation ---

TEST(CoroStreamTest, CancelBeforeFirstPollSkipsBody) {
    auto waker = make_rc<MockWaker>();
    detail::Context ctx(waker);
    bool started = false;
    auto s = sets_started(started);

    s.cancel();
    EXPECT_TRUE(s.poll_next(ctx).isDropped());
    EXPECT_FALSE(started);
}

TEST(CoroStreamTest, CancelAtYieldDestroysFrame) {
    auto waker = make_rc<MockWaker>();
    detail::Context ctx(waker);
    Gate gate{.open = true};
    int  destroyed = 0;
    auto s = leaf_then_end(gate, destroyed);

    EXPECT_EQ(s.poll_next(ctx).value(), 1);
    EXPECT_EQ(destroyed, 0);
    s.cancel();
    EXPECT_TRUE(s.poll_next(ctx).isDropped());
    EXPECT_EQ(destroyed, 1);
}

TEST(CoroStreamTest, CancelledStreamStaysDropped) {
    auto waker = make_rc<MockWaker>();
    detail::Context ctx(waker);
    Gate gate;
    int  destroyed = 0;
    auto s = leaf_then_end(gate, destroyed);

    EXPECT_TRUE(s.poll_next(ctx).isPending());
    s.cancel();
    EXPECT_TRUE(s.poll_next(ctx).isDropped());
    EXPECT_TRUE(s.poll_next(ctx).isDropped());
    EXPECT_EQ(destroyed, 1);
}

// A leaf future is dropped with the frame: no drain, and no further poll of it.
TEST(CoroStreamTest, CancelWhileAwaitingLeafDropsAtOnce) {
    auto waker = make_rc<MockWaker>();
    detail::Context ctx(waker);
    Gate gate;
    int  destroyed = 0;
    auto s = leaf_then_end(gate, destroyed);

    EXPECT_TRUE(s.poll_next(ctx).isPending());
    s.cancel();
    EXPECT_TRUE(s.poll_next(ctx).isDropped());
    EXPECT_EQ(gate.polls, 1);
    EXPECT_EQ(destroyed, 1);
}

// A Cancellable future is cancelled once and polled until it has drained, and only then is
// the frame destroyed.
TEST(CoroStreamTest, CancelWhileAwaitingCancellableDrainsItFirst) {
    auto waker = make_rc<MockWaker>();
    detail::Context ctx(waker);
    Gate first, second;
    int  destroyed = 0;
    auto s = cancellable_then_leaf(first, second, destroyed);

    EXPECT_TRUE(s.poll_next(ctx).isPending());
    s.cancel();
    EXPECT_TRUE(s.poll_next(ctx).isPending());
    EXPECT_TRUE(s.poll_next(ctx).isPending());
    EXPECT_EQ(first.cancels, 1);
    EXPECT_EQ(destroyed, 0);

    first.drained = true;
    EXPECT_TRUE(s.poll_next(ctx).isDropped());
    EXPECT_EQ(first.cancels, 1);
    EXPECT_EQ(destroyed, 1);
}

// Regression: resuming from a Cancellable future's co_await left its cancel hook set. A cancel
// at the next suspension point, on a leaf, then called cancel() through the hook on the
// destroyed awaitable and waited for the leaf to complete before dropping, which for a channel
// that never delivers again was forever.
TEST(CoroStreamTest, CancelAtLeafAfterCancellableAwaitDropsAtOnce) {
    auto waker = make_rc<MockWaker>();
    detail::Context ctx(waker);
    Gate first, second;
    int  destroyed = 0;
    auto s = cancellable_then_leaf(first, second, destroyed);

    EXPECT_TRUE(s.poll_next(ctx).isPending());
    first.open = true;
    EXPECT_EQ(s.poll_next(ctx).value(), 1);
    EXPECT_TRUE(s.poll_next(ctx).isPending());

    s.cancel();
    EXPECT_TRUE(s.poll_next(ctx).isDropped());
    EXPECT_EQ(first.cancels, 0);
    EXPECT_EQ(second.polls, 1);
    EXPECT_EQ(destroyed, 1);
}

// The same stale hook, with the cancel at the co_yield after the Cancellable future's await.
TEST(CoroStreamTest, CancelAtYieldAfterCancellableAwaitDoesNotCancelIt) {
    auto waker = make_rc<MockWaker>();
    detail::Context ctx(waker);
    Gate first, second;
    int  destroyed = 0;
    auto s = cancellable_then_leaf(first, second, destroyed);

    EXPECT_TRUE(s.poll_next(ctx).isPending());
    first.open = true;
    EXPECT_EQ(s.poll_next(ctx).value(), 1);

    s.cancel();
    EXPECT_TRUE(s.poll_next(ctx).isDropped());
    EXPECT_EQ(first.cancels, 0);
    EXPECT_EQ(destroyed, 1);
}
