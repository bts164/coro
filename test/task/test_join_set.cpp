#include <gtest/gtest.h>
#include "executor_traits.h"
#include <coro/coro.h>
#include <coro/co_invoke.h>
#include <coro/sync/join.h>
#include <coro/sync/select.h>
#include <coro/sync/when.h>
#include <coro/task/join_set.h>
#include <coro/task/yield_now.h>
#include "co_assert.h"
#include <coro/runtime/runtime.h>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>
#include <algorithm>
#include <atomic>
#include <memory>

#ifndef CORO_PICO
#include <coro/runtime/current_thread_executor.h>
#include <coro/runtime/work_sharing_executor.h>
#include <coro/runtime/work_stealing_executor.h>
#endif

using namespace coro;

namespace {

template<typename T>
struct ReadyFuture {
    using OutputType = T;
    T value;
    PollResult<T> poll(detail::Context&) { return std::move(value); }
};

struct ReadyVoidFuture {
    using OutputType = void;
    PollResult<void> poll(detail::Context&) { return PollReady; }
};

class MockWaker : public detail::Waker {
public:
    void wake() override {}
    detail::Rc<detail::Waker> clone() override { return detail::make_rc<MockWaker>(); }
};

struct ThrowingFuture {
    using OutputType = int;
    PollResult<int> poll(detail::Context&) {
        try { throw std::runtime_error("task failed"); }
        catch (...) { return PollError(std::current_exception()); }
    }
};

struct ThrowingVoidFuture {
    using OutputType = void;
    PollResult<void> poll(detail::Context&) {
        try { throw std::runtime_error("void task failed"); }
        catch (...) { return PollError(std::current_exception()); }
    }
};

template<typename T>
class OneShotFuture {
public:
    using OutputType = T;
    explicit OneShotFuture(T value) : m_value(std::move(value)) {}
    PollResult<T> poll(detail::Context& ctx) {
        if (m_polled) return std::move(m_value);
        m_polled = true;
        ctx.getWaker()->wake();
        return PollPending;
    }
private:
    T    m_value;
    bool m_polled{false};
};

// Ready at its first poll, which it records in `polled`. On CurrentThreadExecutor the
// task has reported to its JoinSet by the time any other task sees the flag.
struct FlaggedReadyFuture {
    using OutputType = std::shared_ptr<int>;
    std::shared_ptr<int> value;
    bool& polled;
    PollResult<std::shared_ptr<int>> poll(detail::Context&) {
        polled = true;
        return std::move(value);
    }
};

// Used only in single-threaded tests — relies on ordering guarantees.
struct CancellationProbeFuture {
    using OutputType = int;
    std::atomic<bool>& completed_naturally;
    bool first_poll{true};
    PollResult<int> poll(detail::Context& ctx) {
        if (!first_poll) { completed_naturally.store(true); return 42; }
        first_poll = false;
        ctx.getWaker()->wake();
        return PollPending;
    }
};

}  // namespace

// ---------------------------------------------------------------------------
// JoinSet<int> — executor-agnostic tests
// ---------------------------------------------------------------------------

template<typename Traits>
class JoinSetTest : public testing::Test {
protected:
    Traits traits;
};
TYPED_TEST_SUITE(JoinSetTest, AllExecutors);

TYPED_TEST(JoinSetTest, SpawnAndDrainInt) {
    int drain_count = 0;
    this->traits.rt.block_on([](int& drain_count) -> Coro<void> {
        JoinSet<int> js;
        js.spawn(ReadyFuture<int>{1});
        js.spawn(ReadyFuture<int>{2});
        js.spawn(ReadyFuture<int>{3});
        co_await js.drain();
        drain_count = 1;
    }(drain_count));
    EXPECT_EQ(drain_count, 1);
}

TYPED_TEST(JoinSetTest, CollectResultsViaNext) {
    std::vector<int> results;
    this->traits.rt.block_on([](std::vector<int>& results) -> Coro<void> {
        JoinSet<int> js;
        js.spawn(ReadyFuture<int>{10});
        js.spawn(ReadyFuture<int>{20});
        js.spawn(ReadyFuture<int>{30});
        while (auto item = co_await next(js))
            results.push_back(*item);
    }(results));
    std::sort(results.begin(), results.end());
    EXPECT_EQ(results, (std::vector<int>{10, 20, 30}));
}

TYPED_TEST(JoinSetTest, EmptyJoinSetDrainCompletesImmediately) {
    bool reached = false;
    this->traits.rt.block_on([](bool& reached) -> Coro<void> {
        JoinSet<int> js;
        co_await js.drain();
        reached = true;
    }(reached));
    EXPECT_TRUE(reached);
}

TYPED_TEST(JoinSetTest, EmptyJoinSetNextReturnsNulloptImmediately) {
    bool got_nullopt = false;
    this->traits.rt.block_on([](bool& got_nullopt) -> Coro<void> {
        JoinSet<int> js;
        auto item = co_await next(js);
        got_nullopt = !item.has_value();
    }(got_nullopt));
    EXPECT_TRUE(got_nullopt);
}

TYPED_TEST(JoinSetTest, EmptyReflectsSpawnAndDrain) {
    bool empty_before = false, empty_after_spawn = true, empty_after_drain = false;
    this->traits.rt.block_on([](bool& before, bool& after_spawn, bool& after_drain) -> Coro<void> {
        JoinSet<int> js;
        before = js.empty();
        js.spawn(ReadyFuture<int>{1});
        after_spawn = js.empty();
        co_await js.drain();
        after_drain = js.empty();
    }(empty_before, empty_after_spawn, empty_after_drain));
    EXPECT_TRUE(empty_before);
    EXPECT_FALSE(empty_after_spawn);
    EXPECT_TRUE(empty_after_drain);
}

TYPED_TEST(JoinSetTest, DrainRethrowsFirstException) {
    bool caught = false;
    this->traits.rt.block_on([](bool& caught) -> Coro<void> {
        JoinSet<int> js;
        js.spawn(ReadyFuture<int>{1});
        js.spawn(ThrowingFuture{});
        js.spawn(ReadyFuture<int>{3});
        try { co_await js.drain(); }
        catch (const std::runtime_error& e) {
            caught = (std::string(e.what()) == "task failed");
        }
    }(caught));
    EXPECT_TRUE(caught);
}

TYPED_TEST(JoinSetTest, NextRethrowsExceptionInline) {
    bool caught = false;
    this->traits.rt.block_on([](bool& caught) -> Coro<void> {
        JoinSet<int> js;
        js.spawn(ThrowingFuture{});
        try { while (co_await next(js)) {} }
        catch (const std::runtime_error& e) {
            caught = (std::string(e.what()) == "task failed");
        }
    }(caught));
    EXPECT_TRUE(caught);
}

TYPED_TEST(JoinSetTest, CancelOnDropDoesNotHang) {
    bool reached_after = false;
    this->traits.rt.block_on([](bool& out) -> Coro<void> {
        {
            JoinSet<int> js;
            js.spawn(ReadyFuture<int>{42});
            co_return;
        }
        out = true;
        co_return;
    }(reached_after));
    EXPECT_FALSE(reached_after);
}

TYPED_TEST(JoinSetTest, CancelPendingTaskOnDrop) {
    bool reached_after = false;
    this->traits.rt.block_on([](bool& out) -> Coro<void> {
        {
            JoinSet<int> js;
            js.spawn(OneShotFuture<int>{42});
        }
        out = true;
        co_return;
    }(reached_after));
    EXPECT_TRUE(reached_after);
}

TYPED_TEST(JoinSetTest, BuildTaskSpawnsTask) {
    std::vector<int> results;
    this->traits.rt.block_on([](std::vector<int>& results) -> Coro<void> {
        JoinSet<int> js;
        js.build_task(ReadyFuture<int>{1}).spawn();
        js.build_task(ReadyFuture<int>{2}).name("second").spawn();
        js.build_task(ReadyFuture<int>{3}).name("third").spawn();
        while (auto item = co_await next(js))
            results.push_back(*item);
    }(results));
    std::sort(results.begin(), results.end());
    EXPECT_EQ(results, (std::vector<int>{1, 2, 3}));
}

TYPED_TEST(JoinSetTest, ComposesWithCoInvoke) {
    std::vector<int> results;
    std::vector<int> inputs = {1, 2, 3, 4, 5};
    this->traits.rt.block_on([](std::vector<int>& inputs, std::vector<int>& results) -> Coro<void> {
        JoinSet<int> js;
        for (int x : inputs)
            js.spawn(co_invoke([x]() -> Coro<int> { co_return x * x; }));
        while (auto item = co_await next(js))
            results.push_back(*item);
    }(inputs, results));
    std::sort(results.begin(), results.end());
    EXPECT_EQ(results, (std::vector<int>{1, 4, 9, 16, 25}));
}

TYPED_TEST(JoinSetTest, NextSuspendsAndIsWokenByAsyncTask) {
    std::vector<int> results;
    this->traits.rt.block_on([](std::vector<int>& results) -> Coro<void> {
        JoinSet<int> js;
        js.spawn(OneShotFuture<int>{10});
        js.spawn(OneShotFuture<int>{20});
        js.spawn(OneShotFuture<int>{30});
        while (auto item = co_await next(js))
            results.push_back(*item);
    }(results));
    std::sort(results.begin(), results.end());
    EXPECT_EQ(results, (std::vector<int>{10, 20, 30}));
}

TYPED_TEST(JoinSetTest, DrainSuspendsAndIsWokenByAsyncTask) {
    int drain_count = 0;
    this->traits.rt.block_on([](int& drain_count) -> Coro<void> {
        JoinSet<int> js;
        js.spawn(OneShotFuture<int>{1});
        js.spawn(OneShotFuture<int>{2});
        co_await js.drain();
        drain_count = 1;
    }(drain_count));
    EXPECT_EQ(drain_count, 1);
}

TYPED_TEST(JoinSetTest, DrainDiscardsAllButFirstException) {
    int caught_count = 0;
    std::string first_message;
    this->traits.rt.block_on([](int& caught_count, std::string& first_message) -> Coro<void> {
        JoinSet<int> js;
        js.spawn(ThrowingFuture{});
        js.spawn(ThrowingFuture{});
        js.spawn(ThrowingFuture{});
        try { co_await js.drain(); }
        catch (const std::runtime_error& e) {
            ++caught_count;
            first_message = e.what();
        }
    }(caught_count, first_message));
    EXPECT_EQ(caught_count, 1);
    EXPECT_EQ(first_message, "task failed");
}

TYPED_TEST(JoinSetTest, NextInSelectLosingBranchLeavesJoinSetUsable) {
    std::vector<int> results;
    this->traits.rt.block_on([](std::vector<int>& results) -> Coro<void> {
        JoinSet<int> js;
        js.spawn(OneShotFuture<int>{10});
        js.spawn(OneShotFuture<int>{20});
        auto sel = co_await select(next(js), ReadyVoidFuture{});
        // On multi-threaded executors the spawned tasks may complete before the
        // select evaluates, so either branch can win. Capture the result if
        // next(js) won so no item is silently dropped.
        if (auto* b = std::get_if<SelectBranch<0, std::optional<int>>>(&sel))
            if (b->value) results.push_back(*b->value);
        // JoinSet must be fully drainable regardless of which branch won.
        while (auto item = co_await next(js))
            results.push_back(*item);
    }(results));
    std::sort(results.begin(), results.end());
    EXPECT_EQ(results, (std::vector<int>{10, 20}));
}

TYPED_TEST(JoinSetTest, NextInSelectWinningBranchWorks) {
    std::optional<int> got;
    this->traits.rt.block_on([](std::optional<int>& got) -> Coro<void> {
        struct NeverFuture { using OutputType = void; PollResult<void> poll(detail::Context&) { return PollPending; } };
        JoinSet<int> js;
        js.spawn(OneShotFuture<int>{42});
        auto sel = co_await select(next(js), NeverFuture{});
        if (std::holds_alternative<SelectBranch<0, std::optional<int>>>(sel))
            got = *std::get<SelectBranch<0, std::optional<int>>>(sel).value;
    }(got));
    EXPECT_TRUE(got.has_value());
    EXPECT_EQ(*got, 42);
}

TYPED_TEST(JoinSetTest, NextInSelectRepeatedRoundsCollectsAll) {
    std::vector<int> results;
    this->traits.rt.block_on([](std::vector<int>& results) -> Coro<void> {
        struct NeverFuture { using OutputType = void; PollResult<void> poll(detail::Context&) { return PollPending; } };
        JoinSet<int> js;
        for (int i = 1; i <= 4; ++i) js.spawn(OneShotFuture<int>{i * 10});
        while (true) {
            auto sel = co_await select(next(js), NeverFuture{});
            if (!std::holds_alternative<SelectBranch<0, std::optional<int>>>(sel)) break;
            auto item = std::get<SelectBranch<0, std::optional<int>>>(sel).value;
            if (!item) break;
            results.push_back(*item);
        }
    }(results));
    std::sort(results.begin(), results.end());
    EXPECT_EQ(results, (std::vector<int>{10, 20, 30, 40}));
}

// --- Gating next() on empty() via when() ---
//
// An empty JoinSet's next() resolves Ready(exhausted) synchronously (see
// EmptyJoinSetNextReturnsNulloptImmediately above) -- so a bare
// select(other, next(js)) on an empty js never suspends: it "wins" on branch 1 every
// single poll, and a caller looping on that select() spins the calling thread instead
// of yielding to the executor. coro::when(!js.empty(), ...) is the fix: it keeps the
// branch genuinely Pending until there's something to actually wait on.

TEST(JoinSetEmptyGatingTest, GatedNextIsPendingWhileJoinSetEmpty) {
    auto waker = detail::make_rc<MockWaker>();
    detail::Context ctx(waker);
    JoinSet<int> js;
    auto gated = when(!js.empty(), [&js] { return next(js); });
    EXPECT_TRUE(gated.poll(ctx).isPending());
}

TYPED_TEST(JoinSetTest, GatedNextDelegatesOnceNonEmpty) {
    std::optional<int> got;
    this->traits.rt.block_on([](std::optional<int>& got) -> Coro<void> {
        JoinSet<int> js;
        js.spawn(ReadyFuture<int>{7});
        auto sel = co_await select(never<void>(), when(!js.empty(), [&js] { return next(js); }));
        if (std::holds_alternative<SelectBranch<1, std::optional<int>>>(sel))
            got = *std::get<SelectBranch<1, std::optional<int>>>(sel).value;
    }(got));
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(*got, 7);
}

// ---------------------------------------------------------------------------
// JoinSet<void> — executor-agnostic tests
// ---------------------------------------------------------------------------

template<typename Traits>
class JoinSetVoidTest : public testing::Test {
protected:
    Traits traits;
};
TYPED_TEST_SUITE(JoinSetVoidTest, AllExecutors);

TYPED_TEST(JoinSetVoidTest, SpawnAndDrain) {
    std::atomic<int> completed = 0;
    this->traits.rt.block_on([](std::atomic<int>& completed) -> Coro<void> {
        JoinSet<void> js;
        js.spawn(co_invoke([&completed]() -> Coro<void> { ++completed; co_return; }));
        js.spawn(co_invoke([&completed]() -> Coro<void> { ++completed; co_return; }));
        co_await js.drain();
    }(completed));
    EXPECT_EQ(completed, 2);
}

TYPED_TEST(JoinSetVoidTest, DrainRethrowsFirstException) {
    std::atomic_bool caught = false;
    this->traits.rt.block_on([](std::atomic_bool& caught) -> Coro<void> {
        JoinSet<void> js;
        js.spawn(ReadyVoidFuture{});
        js.spawn(ThrowingVoidFuture{});
        try { co_await js.drain(); }
        catch (const std::runtime_error& e) {
            caught.store((std::string(e.what()) == "void task failed"));
        }
    }(caught));
    EXPECT_TRUE(caught);
}

TYPED_TEST(JoinSetVoidTest, EmptyDrainCompletesImmediately) {
    bool reached = false;
    this->traits.rt.block_on([](bool& reached) -> Coro<void> {
        JoinSet<void> js;
        co_await js.drain();
        reached = true;
    }(reached));
    EXPECT_TRUE(reached);
}

TYPED_TEST(JoinSetVoidTest, CollectResultsViaNext) {
    int count = 0;
    this->traits.rt.block_on([](int& count) -> Coro<void> {
        JoinSet<void> js;
        js.spawn(ReadyVoidFuture{});
        js.spawn(ReadyVoidFuture{});
        js.spawn(ReadyVoidFuture{});
        while (co_await next(js)) ++count;
    }(count));
    EXPECT_EQ(count, 3);
}

TYPED_TEST(JoinSetVoidTest, EmptyNextReturnsFalseImmediately) {
    bool got_false = false;
    this->traits.rt.block_on([](bool& got_false) -> Coro<void> {
        JoinSet<void> js;
        got_false = !(co_await next(js));
    }(got_false));
    EXPECT_TRUE(got_false);
}

TYPED_TEST(JoinSetVoidTest, NextRethrowsException) {
    bool caught = false;
    this->traits.rt.block_on([](bool& caught) -> Coro<void> {
        JoinSet<void> js;
        js.spawn(ReadyVoidFuture{});
        js.spawn(ThrowingVoidFuture{});
        js.spawn(ReadyVoidFuture{});
        int completed = 0;
        try { while (co_await next(js)) ++completed; }
        catch (const std::runtime_error& e) {
            caught = (std::string(e.what()) == "void task failed");
        }
        EXPECT_GE(completed, 0);
    }(caught));
    EXPECT_TRUE(caught);
}

TYPED_TEST(JoinSetVoidTest, CancelOnDropDoesNotHang) {
    bool reached_after = false;
    this->traits.rt.block_on([](bool& out) -> Coro<void> {
        { JoinSet<void> js; js.spawn(ReadyVoidFuture{}); }
        out = true;
        co_return;
    }(reached_after));
    EXPECT_TRUE(reached_after);
}

TYPED_TEST(JoinSetVoidTest, ComposesWithCoInvokeAndCapture) {
    std::atomic<int> sum = 0;
    std::vector<int> data = {10, 20, 30};
    this->traits.rt.block_on([](std::vector<int>& data, std::atomic<int>& sum) -> Coro<void> {
        JoinSet<void> js;
        for (int x : data)
            js.spawn(co_invoke([x, &sum]() -> Coro<void> {
                sum.fetch_add(x);
                co_return;
            }));
        co_await js.drain();
    }(data, sum));
    EXPECT_EQ(sum, 60);
}

TYPED_TEST(JoinSetVoidTest, NextSuspendsAndIsWokenByAsyncTask) {
    int count = 0;
    this->traits.rt.block_on([](int& count) -> Coro<void> {
        JoinSet<void> js;
        for (int i = 0; i < 3; ++i)
            js.spawn(co_invoke([]() -> Coro<void> {
                co_await OneShotFuture<int>{0};
            }));
        while (co_await next(js)) ++count;
    }(count));
    EXPECT_EQ(count, 3);
}

TYPED_TEST(JoinSetVoidTest, DrainDiscardsAllButFirstException) {
    int caught_count = 0;
    this->traits.rt.block_on([](int& caught_count) -> Coro<void> {
        JoinSet<void> js;
        js.spawn(ThrowingVoidFuture{});
        js.spawn(ThrowingVoidFuture{});
        try { co_await js.drain(); }
        catch (const std::runtime_error&) { ++caught_count; }
    }(caught_count));
    EXPECT_EQ(caught_count, 1);
}

// ---------------------------------------------------------------------------
// cancel_all() and drain_or_cancel()
// ---------------------------------------------------------------------------

static_assert(Cancellable<detail::JoinSetDrainFuture<void, detail::DrainMode::CancelOnError>>);
static_assert(!Cancellable<detail::JoinSetDrainFuture<void, detail::DrainMode::All>>);

namespace {

// Counts the destruction of the coroutine frame it is passed into, whether or not the
// coroutine has started: a task cancelled before its first poll never runs its body.
// A cancelled task's frame is destroyed before the task reports to its JoinSet.
class DropToken {
public:
    explicit DropToken(std::atomic<int>& dropped) : m_dropped(&dropped) {}
    DropToken(DropToken&& other) noexcept : m_dropped(std::exchange(other.m_dropped, nullptr)) {}
    ~DropToken() { if (m_dropped) m_dropped->fetch_add(1); }
private:
    std::atomic<int>* m_dropped;
};

Coro<void> wait_forever([[maybe_unused]] DropToken token) {
    co_await never<void>();
}

}  // namespace

TYPED_TEST(JoinSetVoidTest, CancelAllCancelsEveryPendingTask) {
    std::atomic<int> dropped = 0;
    this->traits.rt.block_on([](std::atomic<int>& dropped) -> Coro<void> {
        JoinSet<void> js;
        for (int i = 0; i < 3; ++i) js.spawn(wait_forever(DropToken{dropped}));
        js.cancel_all();
        co_await js.drain();
    }(dropped));
    EXPECT_EQ(dropped, 3);
}

TYPED_TEST(JoinSetVoidTest, CancelAllLeavesSetUsable) {
    bool got_result = false;
    std::atomic<int> dropped = 0;
    this->traits.rt.block_on([](bool& got_result, std::atomic<int>& dropped) -> Coro<void> {
        JoinSet<void> js;
        js.spawn(wait_forever(DropToken{dropped}));
        js.cancel_all();
        co_await js.drain();
        js.spawn(ReadyVoidFuture{});
        got_result = co_await next(js);
    }(got_result, dropped));
    EXPECT_TRUE(got_result);
    EXPECT_EQ(dropped, 1);
}

TYPED_TEST(JoinSetVoidTest, DrainOrCancelWaitsForAllWithoutError) {
    std::atomic<int> completed = 0;
    this->traits.rt.block_on([](std::atomic<int>& completed) -> Coro<void> {
        JoinSet<void> js;
        for (int i = 0; i < 3; ++i)
            js.spawn(co_invoke([&completed]() -> Coro<void> {
                co_await OneShotFuture<int>{0};
                ++completed;
            }));
        co_await js.drain_or_cancel();
    }(completed));
    EXPECT_EQ(completed, 3);
}

TYPED_TEST(JoinSetVoidTest, DrainOrCancelCancelsRestOnFirstException) {
    bool caught = false;
    int dropped_at_catch = -1;
    std::atomic<int> dropped = 0;
    this->traits.rt.block_on([](bool& caught, int& dropped_at_catch,
                                std::atomic<int>& dropped) -> Coro<void> {
        JoinSet<void> js;
        js.spawn(wait_forever(DropToken{dropped}));
        js.spawn(wait_forever(DropToken{dropped}));
        js.spawn(ThrowingVoidFuture{});
        try { co_await js.drain_or_cancel(); }
        catch (const std::runtime_error& e) {
            caught = (std::string(e.what()) == "void task failed");
            dropped_at_catch = dropped.load();
        }
        CO_ASSERT(js.empty());
    }(caught, dropped_at_catch, dropped));
    EXPECT_TRUE(caught);
    // Both waiting tasks had finished before the exception reached the caller.
    EXPECT_EQ(dropped_at_catch, 2);
}

TYPED_TEST(JoinSetVoidTest, DrainOrCancelLosingSelectBranchCancelsTasks) {
    std::size_t winner = 0;
    int dropped_after_select = -1;
    std::atomic<int> dropped = 0;
    this->traits.rt.block_on([](std::size_t& winner, int& dropped_after_select,
                                std::atomic<int>& dropped) -> Coro<void> {
        JoinSet<void> js;
        js.spawn(wait_forever(DropToken{dropped}));
        js.spawn(wait_forever(DropToken{dropped}));
        auto sel = co_await select(js.drain_or_cancel(), ReadyVoidFuture{});
        winner = sel.index();
        dropped_after_select = dropped.load();
        CO_ASSERT(js.empty());
    }(winner, dropped_after_select, dropped));
    EXPECT_EQ(winner, 1u);
    // select() returned only once the drain had seen every task finish.
    EXPECT_EQ(dropped_after_select, 2);
}

// ---------------------------------------------------------------------------
// Single-threaded-specific tests (rely on deterministic scheduling order)
// ---------------------------------------------------------------------------

#ifndef CORO_PICO

TEST(JoinSetSingleThreadedTest, PendingTaskIsCancelledNotCompleted) {
    Runtime rt(1);
    std::atomic<bool> completed_naturally{false};
    rt.block_on(co_invoke([&completed_naturally]() -> Coro<void> {
        { JoinSet<int> js; js.spawn(CancellationProbeFuture{completed_naturally}); }
        co_return;
    }));
    EXPECT_FALSE(completed_naturally);
}

TEST(JoinSetSingleThreadedTest, MoveAssignmentCancelsPreviousTasks) {
    Runtime rt(1);
    std::atomic<bool> old_completed_naturally{false};
    bool new_task_ran = false;
    rt.block_on([](std::atomic<bool>& old_cn, bool& new_ran) -> Coro<void> {
        JoinSet<int> js;
        js.spawn(CancellationProbeFuture{old_cn});
        JoinSet<int> js2;
        js2.spawn(ReadyFuture<int>{99});
        js = std::move(js2);
        while (auto item = co_await next(js))
            if (*item == 99) new_ran = true;
    }(old_completed_naturally, new_task_ran));
    EXPECT_FALSE(old_completed_naturally);
    EXPECT_TRUE(new_task_ran);
}

// A completed task refers to itself until next() or a drain takes it from the set, so
// destroying the set with results nobody consumed must release them.
TEST(JoinSetSingleThreadedTest, DestroyReleasesUnconsumedResults) {
    Runtime rt(std::in_place_type<CurrentThreadExecutor>);
    std::weak_ptr<int> result;
    bool held_until_destroyed = false;
    rt.block_on([](std::weak_ptr<int>& result, bool& held_until_destroyed) -> Coro<void> {
        bool polled = false;
        {
            JoinSet<std::shared_ptr<int>> js;
            auto value = std::make_shared<int>(42);
            result = value;
            js.spawn(FlaggedReadyFuture{std::move(value), polled});
            while (!polled) co_await yield_now();
            held_until_destroyed = !result.expired();
        }
    }(result, held_until_destroyed));
    EXPECT_TRUE(held_until_destroyed);
    EXPECT_TRUE(result.expired());
}

TEST(JoinSetSingleThreadedTest, DrainReleasesResults) {
    Runtime rt(std::in_place_type<CurrentThreadExecutor>);
    std::weak_ptr<int> result;
    bool released_by_drain = false;
    rt.block_on([](std::weak_ptr<int>& result, bool& released_by_drain) -> Coro<void> {
        bool polled = false;
        JoinSet<std::shared_ptr<int>> js;
        auto value = std::make_shared<int>(42);
        result = value;
        js.spawn(FlaggedReadyFuture{std::move(value), polled});
        co_await js.drain();
        released_by_drain = result.expired();
    }(result, released_by_drain));
    EXPECT_TRUE(released_by_drain);
}

// ---------------------------------------------------------------------------
// Multi-threaded tests (require work-stealing executor)
// ---------------------------------------------------------------------------

TEST(JoinSetMultiThreadedTest, CollectViaNext) {
    Runtime rt(4);
    std::atomic<int> result_sum{0};
    rt.block_on([](std::atomic<int>& result_sum) -> Coro<void> {
        JoinSet<int> js;
        for (int i = 1; i <= 8; ++i)
            js.spawn(co_invoke([i]() -> Coro<int> { co_return i; }));
        while (auto item = co_await next(js))
            result_sum.fetch_add(*item);
    }(result_sum));
    EXPECT_EQ(result_sum.load(), 36);
}

TEST(JoinSetMultiThreadedTest, Drain) {
    Runtime rt(4);
    std::atomic<int> completed{0};
    rt.block_on([](std::atomic<int>& completed) -> Coro<void> {
        JoinSet<int> js;
        for (int i = 0; i < 16; ++i)
            js.spawn(co_invoke([i, &completed]() -> Coro<int> {
                completed.fetch_add(1);
                co_return i;
            }));
        co_await js.drain();
    }(completed));
    EXPECT_EQ(completed.load(), 16);
}

TEST(JoinSetMultiThreadedTest, VoidDrain) {
    Runtime rt(4);
    std::atomic<int> completed{0};
    rt.block_on([](std::atomic<int>& completed) -> Coro<void> {
        JoinSet<void> js;
        for (int i = 0; i < 16; ++i)
            js.spawn(co_invoke([&completed]() -> Coro<void> {
                completed.fetch_add(1);
                co_return;
            }));
        co_await js.drain();
    }(completed));
    EXPECT_EQ(completed.load(), 16);
}

#endif  // !CORO_PICO
