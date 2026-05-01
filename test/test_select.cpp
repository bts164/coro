#include <gtest/gtest.h>
#include <coro/sync/select.h>
#include <coro/sync/timeout.h>
#include <coro/coro.h>
#include <coro/runtime/runtime.h>
#include <coro/task/spawn_builder.h>
#include <memory>
#include <variant>

using namespace coro;

// --- Concept check ---
static_assert(Future<SelectFuture<Coro<int>, Coro<void>>>);

// --- Helper futures ---

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

// A future that returns Pending for the first N polls, then Ready.
class CountdownFuture {
public:
    using OutputType = int;
    explicit CountdownFuture(int polls_until_ready, int value)
        : m_remaining(polls_until_ready), m_value(value) {}

    PollResult<int> poll(detail::Context& ctx) {
        if (m_remaining > 0) {
            --m_remaining;
            m_waker = ctx.getWaker()->clone();
            m_waker->wake();  // self-wake so the executor re-polls us
            return PollPending;
        }
        return m_value;
    }
private:
    int m_remaining;
    int m_value;
    std::shared_ptr<detail::Waker> m_waker;
};

// --- SelectBranch concept ---

TEST(SelectBranchTest, IndexIsCorrect) {
    static_assert(SelectBranch<0, int>::index == 0);
    static_assert(SelectBranch<1, void>::index == 1);
    static_assert(SelectBranch<2, std::string>::index == 2);
}

TEST(SelectBranchTest, ValueIsAccessible) {
    SelectBranch<0, int> b(42);
    EXPECT_EQ(b.value, 42);
}

// --- Basic select behaviour ---

TEST(SelectTest, FirstBranchWinsImmediately) {
    Runtime rt(1);
    auto result = rt.block_on(select(ImmediateInt{7}, NeverFuture{}));
    EXPECT_TRUE((std::holds_alternative<SelectBranch<0, int>>(result)));
    EXPECT_EQ((std::get<SelectBranch<0, int>>(result).value), 7);
}

TEST(SelectTest, SecondBranchWinsWhenFirstNeverReady) {
    Runtime rt(1);
    auto result = rt.block_on(select(NeverFuture{}, ImmediateVoid{}));
    EXPECT_TRUE((std::holds_alternative<SelectBranch<1, void>>(result)));
}

TEST(SelectTest, ThreeBranchesFirstWins) {
    Runtime rt(1);
    auto result = rt.block_on(select(ImmediateInt{1}, ImmediateInt{2}, ImmediateInt{3}));
    // First branch polled first — wins immediately.
    EXPECT_TRUE((std::holds_alternative<SelectBranch<0, int>>(result)));
    EXPECT_EQ((std::get<SelectBranch<0, int>>(result).value), 1);
}

TEST(SelectTest, ErrorFromWinningBranchPropagates) {
    Runtime rt(1);
    EXPECT_THROW(
        rt.block_on(select(ThrowingFuture{}, NeverFuture{})),
        std::runtime_error
    );
}

TEST(SelectTest, ErrorFromFirstBranchWinsOverPendingSecond) {
    Runtime rt(1);
    EXPECT_THROW(
        rt.block_on(select(ThrowingFuture{}, CountdownFuture{3, 99})),
        std::runtime_error
    );
}

TEST(SelectTest, SecondBranchWinsAfterFirstPends) {
    Runtime rt(1);
    auto result = rt.block_on(select(CountdownFuture{3, 10}, ImmediateInt{20}));
    // First poll: CountdownFuture returns Pending, ImmediateInt returns Ready → second wins.
    EXPECT_TRUE((std::holds_alternative<SelectBranch<1, int>>(result)));
    EXPECT_EQ((std::get<SelectBranch<1, int>>(result).value), 20);
}

// --- Cancellation and drain ---

// A future that writes to shared state only after 2 polls (simulating slow I/O).
// Used to verify that the cancelled branch fully executes before select returns.
struct WritingFuture {
    using OutputType = void;
    std::shared_ptr<int> dest;
    int value;
    PollResult<void> poll(detail::Context&) { *dest = value; return PollReady; }
};

Coro<void> coro_that_spawns_and_blocks(std::shared_ptr<int> out) {
    spawn(WritingFuture{out, 99}).cancelOnDestroy(false);
    // Block forever — this branch will be cancelled by select.
    struct NeverCoro {
        using OutputType = void;
        PollResult<void> poll(detail::Context&) { return PollPending; }
    };
    co_await NeverCoro{};
    co_return;
}

// select cancels the slow coroutine branch; that branch's spawned children must drain
// before select delivers the winning result.
TEST(SelectTest, CancelledCoroBranchDrainsChildrenBeforeSelectCompletes) {
    Runtime rt(1);
    auto shared = std::make_shared<int>(0);

    rt.block_on([](std::shared_ptr<int> out) -> Coro<void> {
        auto result = co_await select(
            coro_that_spawns_and_blocks(out),
            ImmediateVoid{}
        );
        // ImmediateVoid won (branch 1). The cancelled Coro branch must have drained
        // its spawned child (which sets *out = 99) before select returned.
        EXPECT_TRUE((std::holds_alternative<SelectBranch<1, void>>(result)));
        EXPECT_EQ(*out, 99);
        co_return;
    }(shared));
}

// --- timeout ---

TEST(TimeoutTest, FutureCompletesBeforeTimeout) {
    Runtime rt(1);
    using namespace std::chrono_literals;
    auto result = rt.block_on(timeout(1000s, ImmediateInt{42}));
    // Branch 0 (the future) wins.
    EXPECT_TRUE((std::holds_alternative<SelectBranch<0, int>>(result)));
    EXPECT_EQ((std::get<SelectBranch<0, int>>(result).value), 42);
}

TEST(TimeoutTest, TimeoutFiresWhenFutureNeverCompletes) {
    Runtime rt(1);
    using namespace std::chrono_literals;
    // duration = 0 means the deadline is already in the past on first poll.
    auto result = rt.block_on(timeout(0ns, NeverFuture{}));
    // Branch 1 (the sleep) wins — timeout elapsed.
    EXPECT_TRUE((std::holds_alternative<SelectBranch<1, void>>(result)));
}
