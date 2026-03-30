#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <coro/coro.h>
#include <coro/future.h>
#include <stdexcept>
#include <string>

using namespace coro;

class MockWaker : public detail::Waker {
public:
    MOCK_METHOD(void, wake, (), (override));
    MOCK_METHOD(std::shared_ptr<detail::Waker>, clone, (), (override));
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
    auto waker = std::make_shared<MockWaker>();
    detail::Context ctx(waker);
    auto c = returns_int();
    auto result = c.poll(ctx);
    EXPECT_TRUE(result.isReady());
    EXPECT_EQ(result.value(), 42);
}

TEST(CoroTest, VoidPollReturnsReady) {
    auto waker = std::make_shared<MockWaker>();
    detail::Context ctx(waker);
    auto c = returns_void();
    EXPECT_TRUE(c.poll(ctx).isReady());
}

// --- co_await an inner Future ---

TEST(CoroTest, AwaitsImmediateFuture) {
    auto waker = std::make_shared<MockWaker>();
    detail::Context ctx(waker);
    auto c = awaits_immediate(99);
    auto result = c.poll(ctx);
    EXPECT_TRUE(result.isReady());
    EXPECT_EQ(result.value(), 99);
}

TEST(CoroTest, AwaitsImmediateVoidFuture) {
    auto waker = std::make_shared<MockWaker>();
    detail::Context ctx(waker);
    auto c = awaits_immediate_void();
    EXPECT_TRUE(c.poll(ctx).isReady());
}

TEST(CoroTest, ChainsStringFutures) {
    auto waker = std::make_shared<MockWaker>();
    detail::Context ctx(waker);
    auto c = chain_two();
    auto result = c.poll(ctx);
    EXPECT_TRUE(result.isReady());
    EXPECT_EQ(result.value(), "hello world");
}

// --- Exception propagation ---

TEST(CoroTest, ThrowInBodyStoredAsError) {
    auto waker = std::make_shared<MockWaker>();
    detail::Context ctx(waker);
    auto c = throws_immediately();
    auto result = c.poll(ctx);
    EXPECT_TRUE(result.isError());
    EXPECT_THROW(result.rethrowIfError(), std::runtime_error);
}

TEST(CoroTest, InnerFutureErrorRethrown) {
    auto waker = std::make_shared<MockWaker>();
    detail::Context ctx(waker);
    auto c = awaits_throwing();
    auto result = c.poll(ctx);
    EXPECT_TRUE(result.isError());
    EXPECT_THROW(result.rethrowIfError(), std::runtime_error);
}

// --- Suspension (two-poll Future) ---

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

    std::shared_ptr<detail::Waker> storedWaker() const { return m_waker; }

private:
    int                    m_value;
    bool                   m_polled_once = false;
    std::shared_ptr<detail::Waker> m_waker;
};

Coro<int> awaits_two_poll(TwoPollFuture f) {
    co_return co_await std::move(f);
}

TEST(CoroTest, SuspendsOnPendingInnerFuture) {
    auto waker = std::make_shared<MockWaker>();
    EXPECT_CALL(*waker, wake()).Times(0);  // waker not called by the test
    detail::Context ctx(waker);

    TwoPollFuture f(7);
    auto c = awaits_two_poll(std::move(f));

    // First poll: inner future returns Pending → outer coroutine suspends
    auto r1 = c.poll(ctx);
    EXPECT_TRUE(r1.isPending());
}

TEST(CoroTest, ResumesAfterInnerFutureBecomesReady) {
    auto waker = std::make_shared<MockWaker>();
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
    std::shared_ptr<detail::Waker> m_waker;
};

Coro<int> awaits_spurious(SpuriousWakeFuture f) {
    co_return co_await std::move(f);
}

// The outer Coro must absorb spurious wakes without resuming the coroutine
// until the inner future is genuinely ready.
TEST(CoroTest, SpuriousWakeDoesNotResumeCoroutine) {
    auto waker = std::make_shared<MockWaker>();
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
