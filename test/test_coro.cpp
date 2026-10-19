#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <coro/coro.h>
#include <coro/future.h>
#include <stdexcept>
#include <string>

// The Skynet benchmark at the end needs a multi-threaded runtime.
#ifndef CORO_PICO
#include <coro/sync/join.h>
#include <coro/runtime/runtime.h>
#endif

using namespace coro;
using namespace coro::detail;

namespace {

class MockWaker : public detail::Waker {
public:
    MOCK_METHOD(void, wake, (), (override));
    MOCK_METHOD(Rc<detail::Waker>, clone, (), (override));
};

// --- Helper coroutines ---

Coro<int> returns_int() {
    co_return 42;
}

Coro<void> returns_void() {
    co_return;
}

Coro<int> awaits_immediate(int value) {
    struct ImmediateFuture {
        using OutputType = int;
        int m_value;
        PollResult<int> poll(detail::Context&) { return m_value; }
    };
    co_return co_await ImmediateFuture{value};
}

Coro<void> awaits_immediate_void() {
    struct ImmediateFutureVoid {
        using OutputType = void;
        PollResult<void> poll(detail::Context&) { return PollReady; }
    };
    co_await ImmediateFutureVoid{};
}

Coro<int> throws_immediately() {
    throw std::runtime_error("boom");
    co_return 0;
}

Coro<int> awaits_throwing() {
    struct ThrowingFuture {
        using OutputType = int;
        PollResult<int> poll(detail::Context&) {
            return PollError(std::make_exception_ptr(std::runtime_error("inner boom")));
        }
    };
    co_return co_await ThrowingFuture{};
}

Coro<std::string> chain_two() {
    struct StringFuture {
        using OutputType = std::string;
        PollResult<std::string> poll(detail::Context&) { return std::string("hello"); }
    };
    auto s = co_await StringFuture{};
    co_return s + " world";
}

// --- Concept checks (compile-time) ---

static_assert(Future<Coro<int>>);
static_assert(Future<Coro<void>>);
static_assert(Future<Coro<std::string>>);

}  // namespace

// --- Construction and move tests ---

TEST(CoroTest, IntCoroIsConstructible) {
    auto c = returns_int();
    (void)c;
}

TEST(CoroTest, VoidCoroIsConstructible) {
    auto c = returns_void();
    (void)c;
}

TEST(CoroTest, IntCoroIsMovable) {
    auto c1 = returns_int();
    auto c2 = std::move(c1);
    (void)c2;
}

TEST(CoroTest, VoidCoroIsMovable) {
    auto c1 = returns_void();
    auto c2 = std::move(c1);
    (void)c2;
}

// --- co_return value ---

TEST(CoroTest, IntPollReturnsReady) {
    auto waker = make_rc<MockWaker>();
    detail::Context ctx(waker);
    auto c = returns_int();
    auto result = c.poll(ctx);
    EXPECT_TRUE(result.isReady());
    EXPECT_EQ(result.value(), 42);
}

TEST(CoroTest, VoidPollReturnsReady) {
    auto waker = make_rc<MockWaker>();
    detail::Context ctx(waker);
    auto c = returns_void();
    EXPECT_TRUE(c.poll(ctx).isReady());
}

// --- co_await an inner Future ---

TEST(CoroTest, AwaitsImmediateFuture) {
    auto waker = make_rc<MockWaker>();
    detail::Context ctx(waker);
    auto c = awaits_immediate(99);
    auto result = c.poll(ctx);
    EXPECT_TRUE(result.isReady());
    EXPECT_EQ(result.value(), 99);
}

TEST(CoroTest, AwaitsImmediateVoidFuture) {
    auto waker = make_rc<MockWaker>();
    detail::Context ctx(waker);
    auto c = awaits_immediate_void();
    EXPECT_TRUE(c.poll(ctx).isReady());
}

TEST(CoroTest, ChainsStringFutures) {
    auto waker = make_rc<MockWaker>();
    detail::Context ctx(waker);
    auto c = chain_two();
    auto result = c.poll(ctx);
    EXPECT_TRUE(result.isReady());
    EXPECT_EQ(result.value(), "hello world");
}

// --- Exception propagation ---

TEST(CoroTest, ThrowInBodyStoredAsError) {
    auto waker = make_rc<MockWaker>();
    detail::Context ctx(waker);
    auto c = throws_immediately();
    auto result = c.poll(ctx);
    EXPECT_TRUE(result.isError());
    EXPECT_THROW(result.rethrowIfError(), std::runtime_error);
}

TEST(CoroTest, InnerFutureErrorRethrown) {
    auto waker = make_rc<MockWaker>();
    detail::Context ctx(waker);
    auto c = awaits_throwing();
    auto result = c.poll(ctx);
    EXPECT_TRUE(result.isError());
    EXPECT_THROW(result.rethrowIfError(), std::runtime_error);
}

// --- co_await of a dropped future ---

namespace {

// Reports PollDropped, as a handle to a cancelled task does.
template<typename T>
struct DroppedFuture {
    using OutputType = T;
    PollResult<T> poll(detail::Context&) { return PollDropped; }
};

Coro<int> awaits_dropped_int() {
    co_return co_await DroppedFuture<int>{};
}

Coro<void> awaits_dropped_void(bool& carried_on) {
    co_await DroppedFuture<void>{};
    carried_on = true;
}

}  // namespace

TEST(CoroTest, AwaitOfDroppedFutureThrowsTaskCancelled) {
    auto waker = make_rc<MockWaker>();
    detail::Context ctx(waker);
    auto c = awaits_dropped_int();
    auto result = c.poll(ctx);
    ASSERT_TRUE(result.isError());
    EXPECT_THROW(result.rethrowIfError(), TaskCancelled);
}

// A void await has no value to miss, but carrying on would act as if the work had been done.
TEST(CoroTest, VoidAwaitOfDroppedFutureThrowsTaskCancelled) {
    auto waker = make_rc<MockWaker>();
    detail::Context ctx(waker);
    bool carried_on = false;
    auto c = awaits_dropped_void(carried_on);
    auto result = c.poll(ctx);
    ASSERT_TRUE(result.isError());
    EXPECT_THROW(result.rethrowIfError(), TaskCancelled);
    EXPECT_FALSE(carried_on);
}

// --- Suspension (two-poll Future) ---

namespace {

// A Future that returns Pending on the first poll and Ready(value) on the second.
class TwoPollFuture {
public:
    using OutputType = int;
    explicit TwoPollFuture(int value) : m_value(value) {}

    PollResult<int> poll(detail::Context& ctx) {
        if (!m_polled_once) {
            m_polled_once = true;
            m_waker = ctx.getWaker();
            return PollPending;
        }
        return m_value;
    }

    Rc<detail::Waker> storedWaker() const { return m_waker; }

private:
    int                    m_value;
    bool                   m_polled_once = false;
    Rc<detail::Waker> m_waker;
};

Coro<int> awaits_two_poll(TwoPollFuture f) {
    co_return co_await std::move(f);
}

}  // namespace

TEST(CoroTest, SuspendsOnPendingInnerFuture) {
    auto waker = make_rc<MockWaker>();
    EXPECT_CALL(*waker, wake()).Times(0);  // waker not called by the test
    detail::Context ctx(waker);

    TwoPollFuture f(7);
    auto c = awaits_two_poll(std::move(f));

    // First poll: inner future returns Pending → outer coroutine suspends
    auto r1 = c.poll(ctx);
    EXPECT_TRUE(r1.isPending());
}

TEST(CoroTest, ResumesAfterInnerFutureBecomesReady) {
    auto waker = make_rc<MockWaker>();
    EXPECT_CALL(*waker, wake()).Times(::testing::AnyNumber());
    detail::Context ctx(waker);

    TwoPollFuture f(7);
    auto c = awaits_two_poll(std::move(f));

    // First poll: Pending
    EXPECT_TRUE(c.poll(ctx).isPending());

    // Second poll: inner future returns Ready(7) → outer coroutine completes
    auto r2 = c.poll(ctx);
    EXPECT_TRUE(r2.isReady());
    EXPECT_EQ(r2.value(), 7);
}

// --- Spurious-wake correctness ---

namespace {

// A Future that returns Pending for the first N polls, then Ready.
// Models a future that fires its waker before it is actually done (spurious wake).
class SpuriousWakeFuture {
public:
    using OutputType = int;
    explicit SpuriousWakeFuture(int value, int pending_polls)
        : m_value(value), m_remaining(pending_polls) {}

    PollResult<int> poll(detail::Context& ctx) {
        if (m_remaining > 0) {
            --m_remaining;
            m_waker = ctx.getWaker();  // re-register waker each time
            return PollPending;
        }
        return m_value;
    }

private:
    int                    m_value;
    int                    m_remaining;
    Rc<detail::Waker> m_waker;
};

Coro<int> awaits_spurious(SpuriousWakeFuture f) {
    co_return co_await std::move(f);
}

}  // namespace

// The outer Coro must absorb spurious wakes without resuming the coroutine
// until the inner future is genuinely ready.
TEST(CoroTest, SpuriousWakeDoesNotResumeCoroutine) {
    auto waker = make_rc<MockWaker>();
    EXPECT_CALL(*waker, wake()).Times(::testing::AnyNumber());
    detail::Context ctx(waker);

    // Inner future is pending for 2 polls before becoming ready.
    auto c = awaits_spurious(SpuriousWakeFuture{55, 2});

    // Poll 1: inner future Pending → outer Pending
    EXPECT_TRUE(c.poll(ctx).isPending());

    // Poll 2: spurious wake — inner future still Pending → outer Pending
    // (coroutine must NOT be resumed here)
    EXPECT_TRUE(c.poll(ctx).isPending());

    // Poll 3: inner future Ready(55) → outer Ready(55)
    auto r = c.poll(ctx);
    EXPECT_TRUE(r.isReady());
    EXPECT_EQ(r.value(), 55);
}

// --- Cancellation ---

namespace {

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

Coro<int> awaits_leaf(Gate& gate, int& destroyed) {
    FrameMarker marker{&destroyed};
    co_return co_await GateFuture(gate, 1);
}

Coro<int> awaits_cancellable(Gate& gate, int& destroyed) {
    FrameMarker marker{&destroyed};
    co_return co_await CancellableFuture(gate, 1);
}

// Awaits a Cancellable future, then a leaf: the shape whose stale cancel hook hung CoroStream.
Coro<int> cancellable_then_leaf(Gate& first, Gate& second, int& destroyed) {
    FrameMarker marker{&destroyed};
    int a = co_await CancellableFuture(first, 1);
    int b = co_await GateFuture(second, 2);
    co_return a + b;
}

Coro<int> sets_started(bool& started) {
    started = true;
    co_return 1;
}

}  // namespace

TEST(CoroTest, CancelBeforeFirstPollSkipsBody) {
    auto waker = make_rc<MockWaker>();
    detail::Context ctx(waker);
    bool started = false;
    auto c = sets_started(started);

    c.cancel();
    EXPECT_TRUE(c.poll(ctx).isDropped());
    EXPECT_FALSE(started);
}

TEST(CoroTest, CancelledCoroStaysDropped) {
    auto waker = make_rc<MockWaker>();
    detail::Context ctx(waker);
    Gate gate;
    int  destroyed = 0;
    auto c = awaits_leaf(gate, destroyed);

    EXPECT_TRUE(c.poll(ctx).isPending());
    c.cancel();
    EXPECT_TRUE(c.poll(ctx).isDropped());
    EXPECT_TRUE(c.poll(ctx).isDropped());
    EXPECT_EQ(destroyed, 1);
}

// A leaf future is dropped with the frame: no drain, and no further poll of it.
TEST(CoroTest, CancelWhileAwaitingLeafDropsAtOnce) {
    auto waker = make_rc<MockWaker>();
    detail::Context ctx(waker);
    Gate gate;
    int  destroyed = 0;
    auto c = awaits_leaf(gate, destroyed);

    EXPECT_TRUE(c.poll(ctx).isPending());
    c.cancel();
    EXPECT_TRUE(c.poll(ctx).isDropped());
    EXPECT_EQ(gate.polls, 1);
    EXPECT_EQ(destroyed, 1);
}

// A Cancellable future is cancelled once and polled until it has drained, and only then is
// the frame destroyed.
TEST(CoroTest, CancelWhileAwaitingCancellableDrainsItFirst) {
    auto waker = make_rc<MockWaker>();
    detail::Context ctx(waker);
    Gate gate;
    int  destroyed = 0;
    auto c = awaits_cancellable(gate, destroyed);

    EXPECT_TRUE(c.poll(ctx).isPending());
    c.cancel();
    EXPECT_TRUE(c.poll(ctx).isPending());
    EXPECT_TRUE(c.poll(ctx).isPending());
    EXPECT_EQ(gate.cancels, 1);
    EXPECT_EQ(destroyed, 0);

    gate.drained = true;
    EXPECT_TRUE(c.poll(ctx).isDropped());
    EXPECT_EQ(gate.cancels, 1);
    EXPECT_EQ(destroyed, 1);
}

// Once a Cancellable future's co_await has resumed, a cancel at the next co_await, on a leaf,
// must not reach the finished future or wait for the leaf (CoroStream once did both).
TEST(CoroTest, CancelAtLeafAfterCancellableAwaitDropsAtOnce) {
    auto waker = make_rc<MockWaker>();
    detail::Context ctx(waker);
    Gate first, second;
    int  destroyed = 0;
    auto c = cancellable_then_leaf(first, second, destroyed);

    EXPECT_TRUE(c.poll(ctx).isPending());
    first.open = true;
    EXPECT_TRUE(c.poll(ctx).isPending());  // resumed past the first await, now on the leaf

    c.cancel();
    EXPECT_TRUE(c.poll(ctx).isDropped());
    EXPECT_EQ(first.cancels, 0);
    EXPECT_EQ(second.polls, 1);
    EXPECT_EQ(destroyed, 1);
}

TEST(CoroTest, MovedWhileSuspendedResumes) {
    auto waker = make_rc<MockWaker>();
    detail::Context ctx(waker);
    Gate gate;
    int  destroyed = 0;
    auto c1 = awaits_leaf(gate, destroyed);

    EXPECT_TRUE(c1.poll(ctx).isPending());
    auto c2 = std::move(c1);
    gate.open = true;
    auto r = c2.poll(ctx);
    ASSERT_TRUE(r.isReady());
    EXPECT_EQ(r.value(), 1);
    EXPECT_EQ(destroyed, 1);
}

#ifndef CORO_PICO

namespace {

template<std::size_t... Is>
Coro<size_t> skynet(size_t my_num, size_t remaining, std::index_sequence<Is...> seq) {
    if (remaining == 1) {
        co_return my_num;
    }
    auto results = co_await join(skynet(my_num + Is*(remaining/10), remaining/10, seq)...);
    co_return ((std::get<Is>(results)) + ...);
}

}  // namespace

TEST(CoroTest, DISABLED_Skynet) {
    size_t result = Runtime(4).block_on(skynet(0, 1000000, std::make_index_sequence<10>{}));
    EXPECT_EQ(result, 499999500000);
}

#endif  // CORO_PICO
