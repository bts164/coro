#include <gtest/gtest.h>
#include "executor_traits.h"
#include <coro/detail/rc.h>
#include <coro/sync/select.h>
#include <coro/sync/when.h>
#include <coro/coro.h>
#include <coro/runtime/runtime.h>
#include <variant>

using namespace coro;

namespace {

class MockWaker : public detail::Waker {
public:
    void wake() override {}
    detail::Rc<detail::Waker> clone() override { return detail::make_rc<MockWaker>(); }
};

// Deliberately not default-constructible: proves WhenFuture never needs to default-
// construct F, and that when(false, ...) never invokes make_future to build one.
struct NoDefaultFuture {
    using OutputType = int;
    int value;
    explicit NoDefaultFuture(int v) : value(v) {}
    PollResult<int> poll(detail::Context&) { return value; }
};
static_assert(!std::is_default_constructible_v<NoDefaultFuture>);
static_assert(Future<NoDefaultFuture>);
static_assert(std::is_default_constructible_v<WhenFuture<NoDefaultFuture>>);

struct NeverFuture {
    using OutputType = void;
    PollResult<void> poll(detail::Context&) { return PollPending; }
};

struct ImmediateInt {
    using OutputType = int;
    int m_value;
    PollResult<int> poll(detail::Context&) { return m_value; }
};

struct CancellableFuture {
    using OutputType = int;
    bool cancelled = false;
    PollResult<int> poll(detail::Context&) {
        if (cancelled) return PollDropped;
        return PollPending;
    }
    void cancel() noexcept { cancelled = true; }
};

struct NonCancellableFuture {
    using OutputType = int;
    PollResult<int> poll(detail::Context&) { return PollPending; }
};

static_assert(Future<WhenFuture<NoDefaultFuture>>);
static_assert(Cancellable<WhenFuture<CancellableFuture>>);
static_assert(!Cancellable<WhenFuture<NonCancellableFuture>>);

}  // namespace

TEST(WhenFutureTest, DisengagedNeverConstructsInnerFuture) {
    bool called = false;
    auto w = when(false, [&] { called = true; return NoDefaultFuture(42); });
    EXPECT_FALSE(called);
}

TEST(WhenFutureTest, DisengagedAlwaysPollsPending) {
    auto waker = detail::make_rc<MockWaker>();
    detail::Context ctx(waker);
    auto w = when(false, [&] { return NoDefaultFuture(42); });
    EXPECT_TRUE(w.poll(ctx).isPending());
    EXPECT_TRUE(w.poll(ctx).isPending());  // repeated polls stay Pending
}

TEST(WhenFutureTest, EngagedConstructsExactlyOnce) {
    int calls = 0;
    auto w = when(true, [&] { ++calls; return NoDefaultFuture(42); });
    EXPECT_EQ(calls, 1);
}

TEST(WhenFutureTest, EngagedPollDelegatesToInner) {
    auto waker = detail::make_rc<MockWaker>();
    detail::Context ctx(waker);
    auto w = when(true, [&] { return NoDefaultFuture(42); });
    auto r = w.poll(ctx);
    ASSERT_TRUE(r.isReady());
    EXPECT_EQ(r.value(), 42);
}

// Regression: select()'s drain pass cancels every losing Cancellable branch, then polls
// it until PollDropped. A disengaged WhenFuture that stayed PollPending forever after
// cancel() would hang select() -- this is the case that motivated tracking cancellation
// separately from engagement.
TEST(WhenFutureTest, DisengagedCancelThenPollReturnsDropped) {
    auto waker = detail::make_rc<MockWaker>();
    detail::Context ctx(waker);
    WhenFuture<CancellableFuture> w;  // disengaged
    w.cancel();
    EXPECT_TRUE(w.poll(ctx).isDropped());
}

// Same external contract as the disengaged case above, but exercised via the real
// forwarding path: cancel() reaches the wrapped future, which is what actually reports
// PollDropped.
TEST(WhenFutureTest, EngagedCancelForwardsToInner) {
    auto waker = detail::make_rc<MockWaker>();
    detail::Context ctx(waker);
    WhenFuture<CancellableFuture> w{CancellableFuture{}};
    w.cancel();
    EXPECT_TRUE(w.poll(ctx).isDropped());
}

// --- Integration with select() — the pattern that motivated when() ---

template<typename Traits>
class WhenSelectTest : public testing::Test {
protected:
    Traits traits;
};
TYPED_TEST_SUITE(WhenSelectTest, AllExecutors);

TYPED_TEST(WhenSelectTest, DisabledBranchNeverWinsAndIsNeverConstructed) {
    bool called = false;
    this->traits.rt.block_on([](bool& called) -> Coro<void> {
        auto sel = co_await select(
            when(false, [&called] { called = true; return NoDefaultFuture(7); }),
            ImmediateInt{99});
        EXPECT_TRUE((std::holds_alternative<SelectBranch<1, int>>(sel)));
        EXPECT_EQ((std::get<SelectBranch<1, int>>(sel).value), 99);
    }(called));
    EXPECT_FALSE(called);
}

TYPED_TEST(WhenSelectTest, EnabledBranchCanWin) {
    std::optional<int> got;
    this->traits.rt.block_on([](std::optional<int>& got) -> Coro<void> {
        auto sel = co_await select(
            when(true, [] { return NoDefaultFuture(7); }),
            NeverFuture{});
        if (std::holds_alternative<SelectBranch<0, int>>(sel))
            got = std::get<SelectBranch<0, int>>(sel).value;
    }(got));
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(*got, 7);
}
