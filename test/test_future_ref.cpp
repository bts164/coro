#include <gtest/gtest.h>
#include <coro/future.h>
#include <coro/coro.h>
#include <coro/co_invoke.h>
#include <coro/sync/select.h>
#include <coro/sync/oneshot.h>
#include <coro/task/join_handle.h>
#include <coro/runtime/runtime.h>

#include <variant>

using namespace coro;

// ---------------------------------------------------------------------------
// Minimal test futures
// ---------------------------------------------------------------------------

class MockWaker : public detail::Waker {
public:
    void wake() override {}
    std::shared_ptr<Waker> clone() override { return std::make_shared<MockWaker>(); }
};

template<typename T>
struct ReadyFuture {
    using OutputType = T;
    T value;
    PollResult<T> poll(detail::Context&) { return std::move(value); }
};

template<typename T>
struct NeverFuture {
    using OutputType = T;
    PollResult<T> poll(detail::Context&) { return PollPending; }
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
    // no cancel()
};

// ---------------------------------------------------------------------------
// Concept checks
// ---------------------------------------------------------------------------

static_assert(Future<FutureRef<ReadyFuture<int>>>);
static_assert(Future<FutureRef<NeverFuture<void>>>);
static_assert(Future<FutureRef<CancellableFuture>>);

// FutureRef is never Cancellable — it must not cancel the underlying future when dropped.
static_assert(!Cancellable<FutureRef<CancellableFuture>>);
static_assert(!Cancellable<FutureRef<NonCancellableFuture>>);

// FutureRef is not copyable.
static_assert(!std::is_copy_constructible_v<FutureRef<ReadyFuture<int>>>);
static_assert(!std::is_copy_assignable_v<FutureRef<ReadyFuture<int>>>);

// FutureRef is movable.
static_assert(std::is_move_constructible_v<FutureRef<ReadyFuture<int>>>);

// coro::ref() only accepts lvalues — verified at compile time by the lvalue-ref parameter.
// (Passing an rvalue would fail to compile; we don't test that case here.)

// ---------------------------------------------------------------------------
// Unit tests — poll forwarding
// ---------------------------------------------------------------------------

TEST(FutureRefTest, PollDelegatesToUnderlying) {
    auto waker = std::make_shared<MockWaker>();
    detail::Context ctx(waker);

    ReadyFuture<int> f{42};
    auto r = ref(f).poll(ctx);
    ASSERT_TRUE(r.isReady());
    EXPECT_EQ(r.value(), 42);
}

TEST(FutureRefTest, PendingDelegatesToUnderlying) {
    auto waker = std::make_shared<MockWaker>();
    detail::Context ctx(waker);

    NeverFuture<int> f;
    auto r = ref(f).poll(ctx);
    EXPECT_TRUE(r.isPending());
}

// ---------------------------------------------------------------------------
// Integration tests — select with coro::ref()
// ---------------------------------------------------------------------------

// When the other branch wins, the future behind coro::ref() is untouched.
TEST(FutureRefTest, LosingBranchLeavesUnderlyingFutureUsable) {
    Runtime rt(1);
    int got = -1;

    rt.block_on(co_invoke([&got]() -> Coro<void> {
        // An immediately-ready future in the other branch.
        struct ImmediateVoid {
            using OutputType = void;
            PollResult<void> poll(detail::Context&) { return PollReady; }
        };

        NeverFuture<int> f;
        auto sel = co_await select(coro::ref(f), ImmediateVoid{});

        // ImmediateVoid (branch 1) won — f is intact and still a valid NeverFuture.
        EXPECT_TRUE((std::holds_alternative<SelectBranch<1, void>>(sel)));

        // Re-using f: wrap it again and it still returns Pending.
        auto waker = std::make_shared<MockWaker>();
        detail::Context ctx(waker);
        auto r = ref(f).poll(ctx);
        EXPECT_TRUE(r.isPending());

        got = 0;  // reached
    }));

    EXPECT_EQ(got, 0);
}

// When the coro::ref() branch wins, the result is consumed from the underlying future.
TEST(FutureRefTest, WinningBranchDeliversResult) {
    Runtime rt(1);
    std::optional<int> got;

    rt.block_on(co_invoke([&got]() -> Coro<void> {
        struct NeverVoid {
            using OutputType = void;
            PollResult<void> poll(detail::Context&) { return PollPending; }
        };

        ReadyFuture<int> f{99};
        auto sel = co_await select(coro::ref(f), NeverVoid{});
        if (std::holds_alternative<SelectBranch<0, int>>(sel))
            got = std::get<SelectBranch<0, int>>(sel).value;
    }));

    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(*got, 99);
}

// coro::ref(task) loses a select round; the task must survive (not be cancelled)
// and deliver its result when awaited directly afterward.
//
// The task blocks on a oneshot receiver, guaranteeing it cannot be ready during
// the select — no scheduler assumptions needed.
TEST(FutureRefTest, LosingBranchDoesNotCancelTask) {
    Runtime rt(1);
    int result = -1;

    rt.block_on([](int& result) -> Coro<void> {
        auto [tx, rx] = oneshot::channel<int>();

        // Task is blocked waiting for tx — cannot be ready during the select below.
        JoinHandle<int> task = coro::spawn(
            [](auto rx) -> Coro<int> {
                auto r = co_await rx.recv();
                co_return r.value();
            }(std::move(rx)));

        struct ImmediateVoid {
            using OutputType = void;
            PollResult<void> poll(detail::Context&) { return PollReady; }
        };

        // ImmediateVoid must win — task is blocked, so coro::ref(task) cannot.
        auto sel = co_await select(coro::ref(task), ImmediateVoid{});
        EXPECT_TRUE((std::holds_alternative<SelectBranch<1, void>>(sel)));

        // Task is still alive. Signal it to complete, then await directly.
        tx.send(77);
        result = co_await task;
    }(result));

    EXPECT_EQ(result, 77);
}
