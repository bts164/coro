#include <gtest/gtest.h>
#include "executor_traits.h"
#include <coro/future.h>
#include <coro/coro.h>
#include <coro/co_invoke.h>
#include <coro/sync/select.h>
#include <coro/sync/oneshot.h>
#include <coro/task/join_handle.h>
#include <coro/runtime/runtime.h>
#include <variant>

using namespace coro;
using namespace coro::detail;

namespace {

class MockWaker : public detail::Waker {
public:
    void wake() override {}
    Rc<Waker> clone() override { return make_rc<MockWaker>(); }
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
};

static_assert(Future<FutureRef<ReadyFuture<int>>>);
static_assert(Future<FutureRef<NeverFuture<void>>>);
static_assert(Future<FutureRef<CancellableFuture>>);
static_assert(!Cancellable<FutureRef<CancellableFuture>>);
static_assert(!Cancellable<FutureRef<NonCancellableFuture>>);
static_assert(!std::is_copy_constructible_v<FutureRef<ReadyFuture<int>>>);
static_assert(!std::is_copy_assignable_v<FutureRef<ReadyFuture<int>>>);
static_assert(std::is_move_constructible_v<FutureRef<ReadyFuture<int>>>);

}  // namespace

template<typename Traits>
class FutureRefTest : public testing::Test {
protected:
    Traits traits;
};
TYPED_TEST_SUITE(FutureRefTest, AllExecutors);

TYPED_TEST(FutureRefTest, PollDelegatesToUnderlying) {
    auto waker = make_rc<MockWaker>();
    detail::Context ctx(waker);
    ReadyFuture<int> f{42};
    auto r = ref(f).poll(ctx);
    ASSERT_TRUE(r.isReady());
    EXPECT_EQ(r.value(), 42);
}

TYPED_TEST(FutureRefTest, PendingDelegatesToUnderlying) {
    auto waker = make_rc<MockWaker>();
    detail::Context ctx(waker);
    NeverFuture<int> f;
    EXPECT_TRUE(ref(f).poll(ctx).isPending());
}

TYPED_TEST(FutureRefTest, LosingBranchLeavesUnderlyingFutureUsable) {
    int got = -1;
    this->traits.rt.block_on([](int& got) -> Coro<void> {
        struct ImmediateVoid {
            using OutputType = void;
            PollResult<void> poll(detail::Context&) { return PollReady; }
        };
        NeverFuture<int> f;
        auto sel = co_await select(coro::ref(f), ImmediateVoid{});
        EXPECT_TRUE((std::holds_alternative<SelectBranch<1, void>>(sel)));
        auto waker = make_rc<MockWaker>();
        detail::Context ctx(waker);
        EXPECT_TRUE(ref(f).poll(ctx).isPending());
        got = 0;
    }(got));
    EXPECT_EQ(got, 0);
}

TYPED_TEST(FutureRefTest, WinningBranchDeliversResult) {
    std::optional<int> got;
    this->traits.rt.block_on([](std::optional<int>& got) -> Coro<void> {
        struct NeverVoid {
            using OutputType = void;
            PollResult<void> poll(detail::Context&) { return PollPending; }
        };
        ReadyFuture<int> f{99};
        auto sel = co_await select(coro::ref(f), NeverVoid{});
        if (std::holds_alternative<SelectBranch<0, int>>(sel))
            got = std::get<SelectBranch<0, int>>(sel).value;
    }(got));
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(*got, 99);
}

TYPED_TEST(FutureRefTest, LosingBranchDoesNotCancelTask) {
    int result = -1;
    this->traits.rt.block_on([](int& result) -> Coro<void> {
        auto [tx, rx] = oneshot_channel<int>();
        JoinHandle<int> task = coro::spawn(
            [](auto rx) -> Coro<int> {
                auto r = co_await rx.recv();
                co_return r.value();
            }(std::move(rx)));
        struct ImmediateVoid {
            using OutputType = void;
            PollResult<void> poll(detail::Context&) { return PollReady; }
        };
        auto sel = co_await select(coro::ref(task), ImmediateVoid{});
        EXPECT_TRUE((std::holds_alternative<SelectBranch<1, void>>(sel)));
        tx.send(77);
        result = co_await task;
    }(result));
    EXPECT_EQ(result, 77);
}
