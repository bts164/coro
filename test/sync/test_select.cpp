#include <gtest/gtest.h>
#include "executor_traits.h"
#include <coro/detail/rc.h>
#include <coro/sync/select.h>
#include <coro/coro.h>
#include <coro/runtime/runtime.h>
#include <coro/task/spawn_builder.h>
#include <memory>
#include <variant>

#ifndef CORO_PICO
#include <coro/sync/timeout.h>
#endif

using namespace coro;

static_assert(Future<SelectFuture<Coro<int>, Coro<void>>>);

namespace {

struct ImmediateInt {
    using OutputType = int;
    int m_value;
    PollResult<int> poll(detail::Context&) { return m_value; }
};

struct ImmediateVoid {
    using OutputType = void;
    PollResult<void> poll(detail::Context&) { return PollReady; }
};

struct NeverFuture {
    using OutputType = void;
    PollResult<void> poll(detail::Context&) { return PollPending; }
};

struct ThrowingFuture {
    using OutputType = int;
    PollResult<int> poll(detail::Context&) {
        return PollError(std::make_exception_ptr(std::runtime_error("bang")));
    }
};

class CountdownFuture {
public:
    using OutputType = int;
    explicit CountdownFuture(int polls_until_ready, int value)
        : m_remaining(polls_until_ready), m_value(value) {}
    PollResult<int> poll(detail::Context& ctx) {
        if (m_remaining > 0) {
            --m_remaining;
            m_waker = ctx.getWaker()->clone();
            m_waker->wake();
            return PollPending;
        }
        return m_value;
    }
private:
    int m_remaining;
    int m_value;
    detail::Rc<detail::Waker> m_waker;
};

}  // namespace

// --- SelectBranch concept (no executor needed) ---

TEST(SelectBranchTest, IndexIsCorrect) {
    static_assert(SelectBranch<0, int>::index == 0);
    static_assert(SelectBranch<1, void>::index == 1);
    static_assert(SelectBranch<2, std::string>::index == 2);
}

TEST(SelectBranchTest, ValueIsAccessible) {
    SelectBranch<0, int> b(42);
    EXPECT_EQ(b.value, 42);
}

// --- SelectTest — all four executors ---

template<typename Traits>
class SelectTest : public testing::Test {
protected:
    Traits traits;
};
TYPED_TEST_SUITE(SelectTest, AllExecutors);

TYPED_TEST(SelectTest, FirstBranchWinsImmediately) {
    auto result = this->traits.rt.block_on(select(ImmediateInt{7}, NeverFuture{}));
    EXPECT_TRUE((std::holds_alternative<SelectBranch<0, int>>(result)));
    EXPECT_EQ((std::get<SelectBranch<0, int>>(result).value), 7);
}

TYPED_TEST(SelectTest, SecondBranchWinsWhenFirstNeverReady) {
    auto result = this->traits.rt.block_on(select(NeverFuture{}, ImmediateVoid{}));
    EXPECT_TRUE((std::holds_alternative<SelectBranch<1, void>>(result)));
}

TYPED_TEST(SelectTest, ThreeBranchesFirstWins) {
    auto result = this->traits.rt.block_on(select(ImmediateInt{1}, ImmediateInt{2}, ImmediateInt{3}));
    EXPECT_TRUE((std::holds_alternative<SelectBranch<0, int>>(result)));
    EXPECT_EQ((std::get<SelectBranch<0, int>>(result).value), 1);
}

TYPED_TEST(SelectTest, ErrorFromWinningBranchPropagates) {
    EXPECT_THROW(
        this->traits.rt.block_on(select(ThrowingFuture{}, NeverFuture{})),
        std::runtime_error);
}

TYPED_TEST(SelectTest, ErrorFromFirstBranchWinsOverPendingSecond) {
    EXPECT_THROW(
        this->traits.rt.block_on(select(ThrowingFuture{}, CountdownFuture{3, 99})),
        std::runtime_error);
}

TYPED_TEST(SelectTest, SecondBranchWinsAfterFirstPends) {
    auto result = this->traits.rt.block_on(select(CountdownFuture{3, 10}, ImmediateInt{20}));
    EXPECT_TRUE((std::holds_alternative<SelectBranch<1, int>>(result)));
    EXPECT_EQ((std::get<SelectBranch<1, int>>(result).value), 20);
}

namespace {

struct WritingFuture {
    using OutputType = void;
    detail::Rc<int> dest;
    int value;
    PollResult<void> poll(detail::Context&) { *dest = value; return PollReady; }
};

Coro<void> coro_that_spawns_and_blocks(detail::Rc<int> out) {
    spawn(WritingFuture{out, 99}).cancelOnDestroy(false);
    struct NeverCoro {
        using OutputType = void;
        PollResult<void> poll(detail::Context&) { return PollPending; }
    };
    co_await NeverCoro{};
    co_return;
}

}  // namespace

TYPED_TEST(SelectTest, CancelledCoroBranchDrainsChildrenBeforeSelectCompletes) {
    auto shared = detail::make_rc<int>(0);
    this->traits.rt.block_on([](detail::Rc<int> out) -> Coro<void> {
        auto result = co_await select(
            coro_that_spawns_and_blocks(out),
            ImmediateVoid{});
        EXPECT_TRUE((std::holds_alternative<SelectBranch<1, void>>(result)));
        EXPECT_EQ(*out, 99);
        co_return;
    }(shared));
}

// ---------------------------------------------------------------------------
// Timeout tests — desktop only (libuv-backed sleep_for)
// ---------------------------------------------------------------------------

#ifndef CORO_PICO

TEST(TimeoutTest, FutureCompletesBeforeTimeout) {
    Runtime rt(1);
    using namespace std::chrono_literals;
    auto result = rt.block_on(timeout(1000s, ImmediateInt{42}));
    EXPECT_TRUE((std::holds_alternative<SelectBranch<0, int>>(result)));
    EXPECT_EQ((std::get<SelectBranch<0, int>>(result).value), 42);
}

TEST(TimeoutTest, TimeoutFiresWhenFutureNeverCompletes) {
    Runtime rt(1);
    using namespace std::chrono_literals;
    auto result = rt.block_on(timeout(0ns, NeverFuture{}));
    EXPECT_TRUE((std::holds_alternative<SelectBranch<1, void>>(result)));
}

#endif  // !CORO_PICO
