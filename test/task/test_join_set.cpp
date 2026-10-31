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
#include <deque>
#include <memory>
#include <utility>

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

using IntItem = std::optional<JoinResult<int>>;

static_assert(Stream<JoinSet<int>>);
static_assert(std::same_as<JoinSet<int>::ItemType, JoinResult<int>>);
static_assert(std::same_as<JoinSet<void, std::string>::ItemType, JoinResult<void, std::string>>);

// An untagged set takes no tag, a tagged one requires one, and only a tagged result has tag().
template<typename S>
concept SpawnsUntagged = requires(S& js) { js.spawn(ReadyFuture<int>{1}); };
template<typename S>
concept SpawnsTagged = requires(S& js) { js.spawn(1, ReadyFuture<int>{1}); };
template<typename R>
concept HasTag = requires(R& r) { r.tag(); };
static_assert(SpawnsUntagged<JoinSet<int>> && !SpawnsTagged<JoinSet<int>>);
static_assert(!SpawnsUntagged<JoinSet<int, int>> && SpawnsTagged<JoinSet<int, int>>);
static_assert(!HasTag<JoinResult<int>> && HasTag<JoinResult<int, int>>);
static_assert(std::same_as<decltype(std::declval<JoinSet<int>&>().spawn(ReadyFuture<int>{1})), CancelHandle>);

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
            results.push_back(item->value());
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

TYPED_TEST(JoinSetTest, NextYieldsExceptionAsResult) {
    bool has_exception = false, has_value = true, cancelled = true, caught = false;
    this->traits.rt.block_on([](bool& has_exception, bool& has_value, bool& cancelled,
                                bool& caught) -> Coro<void> {
        JoinSet<int> js;
        js.spawn(ThrowingFuture{});
        auto item = co_await next(js);
        CO_ASSERT(item.has_value());
        has_exception = item->exception() != nullptr;
        has_value = item->has_value();
        cancelled = item->is_cancelled();
        try { (void)item->value(); }
        catch (const std::runtime_error& e) {
            caught = (std::string(e.what()) == "task failed");
        }
        auto after = co_await next(js);
        CO_ASSERT(!after.has_value());
    }(has_exception, has_value, cancelled, caught));
    EXPECT_TRUE(has_exception);
    EXPECT_FALSE(has_value);
    EXPECT_FALSE(cancelled);
    EXPECT_TRUE(caught);
}

TYPED_TEST(JoinSetTest, NextYieldsValueAsResult) {
    int value = 0;
    bool has_value = false, no_exception = false, cancelled = true;
    this->traits.rt.block_on([](int& value, bool& has_value, bool& no_exception,
                                bool& cancelled) -> Coro<void> {
        JoinSet<int> js;
        js.spawn(ReadyFuture<int>{5});
        auto item = co_await next(js);
        CO_ASSERT(item.has_value());
        has_value = item->has_value();
        no_exception = item->exception() == nullptr;
        cancelled = item->is_cancelled();
        value = std::move(*item).value();
    }(value, has_value, no_exception, cancelled));
    EXPECT_EQ(value, 5);
    EXPECT_TRUE(has_value);
    EXPECT_TRUE(no_exception);
    EXPECT_FALSE(cancelled);
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
            results.push_back(item->value());
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
            results.push_back(item->value());
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
            results.push_back(item->value());
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
        if (auto* b = std::get_if<SelectBranch<0, IntItem>>(&sel))
            if (b->value) results.push_back(b->value->value());
        // JoinSet must be fully drainable regardless of which branch won.
        while (auto item = co_await next(js))
            results.push_back(item->value());
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
        if (std::holds_alternative<SelectBranch<0, IntItem>>(sel))
            got = std::get<SelectBranch<0, IntItem>>(sel).value->value();
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
            if (!std::holds_alternative<SelectBranch<0, IntItem>>(sel)) break;
            auto item = std::get<SelectBranch<0, IntItem>>(sel).value;
            if (!item) break;
            results.push_back(item->value());
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
        if (std::holds_alternative<SelectBranch<1, IntItem>>(sel))
            got = std::get<SelectBranch<1, IntItem>>(sel).value->value();
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

TYPED_TEST(JoinSetVoidTest, NextYieldsExceptionAsResult) {
    int values = 0, caught = 0;
    this->traits.rt.block_on([](int& values, int& caught) -> Coro<void> {
        JoinSet<void> js;
        js.spawn(ReadyVoidFuture{});
        js.spawn(ThrowingVoidFuture{});
        js.spawn(ReadyVoidFuture{});
        // Every task is reported, the failed one included.
        while (auto item = co_await next(js)) {
            if (item->has_value()) ++values;
            try { item->value(); }
            catch (const std::runtime_error& e) {
                if (std::string(e.what()) == "void task failed") ++caught;
            }
        }
    }(values, caught));
    EXPECT_EQ(values, 2);
    EXPECT_EQ(caught, 1);
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
// cancel_all(), cancelled results, drain(), shutdown() and join_all()
// ---------------------------------------------------------------------------

static_assert(Cancellable<detail::JoinSetDrainFuture<void, void>>);
static_assert(Cancellable<detail::JoinSetShutdownFuture<int, void>>);
static_assert(Cancellable<detail::JoinSetJoinAllFuture<int, int, std::vector<JoinResult<int, int>>>>);
static_assert(std::same_as<decltype(std::declval<JoinSet<int>&>().join_all())::OutputType,
                           std::vector<JoinResult<int>>>);
static_assert(std::same_as<
    decltype(std::declval<JoinSet<int>&>().join_all(std::deque<JoinResult<int>>{}))::OutputType,
    std::deque<JoinResult<int>>>);
static_assert(std::same_as<decltype(std::declval<JoinSet<int>&>().drain())::OutputType, void>);

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

TYPED_TEST(JoinSetVoidTest, CancelAllReportsEachTaskAsCancelled) {
    int cancelled = 0;
    bool value_threw = false, no_exception = true;
    std::atomic<int> dropped = 0;
    this->traits.rt.block_on([](int& cancelled, bool& value_threw, bool& no_exception,
                                std::atomic<int>& dropped) -> Coro<void> {
        JoinSet<void> js;
        for (int i = 0; i < 3; ++i) js.spawn(wait_forever(DropToken{dropped}));
        js.cancel_all();
        while (auto item = co_await next(js)) {
            if (item->is_cancelled()) ++cancelled;
            if (item->exception()) no_exception = false;
            try { item->value(); }
            catch (const JoinCancelled&) { value_threw = true; }
        }
    }(cancelled, value_threw, no_exception, dropped));
    EXPECT_EQ(cancelled, 3);
    EXPECT_TRUE(value_threw);
    EXPECT_TRUE(no_exception);
    EXPECT_EQ(dropped, 3);
}

TYPED_TEST(JoinSetVoidTest, DrainCountsCancelledTaskAsFailure) {
    bool caught = false;
    std::atomic<int> dropped = 0;
    this->traits.rt.block_on([](bool& caught, std::atomic<int>& dropped) -> Coro<void> {
        JoinSet<void> js;
        js.spawn(wait_forever(DropToken{dropped}));
        js.cancel_all();
        try { co_await js.drain(); }
        catch (const JoinCancelled&) { caught = true; }
    }(caught, dropped));
    EXPECT_TRUE(caught);
    EXPECT_EQ(dropped, 1);
}

TYPED_TEST(JoinSetVoidTest, ShutdownCancelsEveryTaskAndThrowsNothing) {
    bool returned = false;
    std::atomic<int> dropped = 0;
    this->traits.rt.block_on([](bool& returned, std::atomic<int>& dropped) -> Coro<void> {
        JoinSet<void> js;
        for (int i = 0; i < 3; ++i) js.spawn(wait_forever(DropToken{dropped}));
        js.spawn(ThrowingVoidFuture{});
        co_await js.shutdown();
        returned = true;
        CO_ASSERT(js.empty());
    }(returned, dropped));
    EXPECT_TRUE(returned);
    EXPECT_EQ(dropped, 3);
}

TYPED_TEST(JoinSetVoidTest, ShutdownLeavesSetUsable) {
    bool got_result = false;
    std::atomic<int> dropped = 0;
    this->traits.rt.block_on([](bool& got_result, std::atomic<int>& dropped) -> Coro<void> {
        JoinSet<void> js;
        js.spawn(wait_forever(DropToken{dropped}));
        co_await js.shutdown();
        js.spawn(ReadyVoidFuture{});
        auto item = co_await next(js);
        got_result = item.has_value() && item->has_value();
    }(got_result, dropped));
    EXPECT_TRUE(got_result);
    EXPECT_EQ(dropped, 1);
}

TYPED_TEST(JoinSetVoidTest, ShutdownOfEmptySetCompletesImmediately) {
    bool reached = false;
    this->traits.rt.block_on([](bool& reached) -> Coro<void> {
        JoinSet<void> js;
        co_await js.shutdown();
        reached = true;
    }(reached));
    EXPECT_TRUE(reached);
}

TYPED_TEST(JoinSetVoidTest, DrainWaitsForAllWithoutError) {
    std::atomic<int> completed = 0;
    this->traits.rt.block_on([](std::atomic<int>& completed) -> Coro<void> {
        JoinSet<void> js;
        for (int i = 0; i < 3; ++i)
            js.spawn(co_invoke([&completed]() -> Coro<void> {
                co_await OneShotFuture<int>{0};
                ++completed;
            }));
        co_await js.drain();
    }(completed));
    EXPECT_EQ(completed, 3);
}

TYPED_TEST(JoinSetVoidTest, DrainCancelsRestOnFirstException) {
    bool caught = false;
    int dropped_at_catch = -1;
    std::atomic<int> dropped = 0;
    this->traits.rt.block_on([](bool& caught, int& dropped_at_catch,
                                std::atomic<int>& dropped) -> Coro<void> {
        JoinSet<void> js;
        js.spawn(wait_forever(DropToken{dropped}));
        js.spawn(wait_forever(DropToken{dropped}));
        js.spawn(ThrowingVoidFuture{});
        try { co_await js.drain(); }
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

TYPED_TEST(JoinSetVoidTest, DrainWaitLetsEveryTaskFinish) {
    bool caught = false;
    std::atomic<int> completed = 0;
    this->traits.rt.block_on([](bool& caught, std::atomic<int>& completed) -> Coro<void> {
        JoinSet<void> js;
        js.spawn(ThrowingVoidFuture{});
        for (int i = 0; i < 3; ++i)
            js.spawn(co_invoke([&completed]() -> Coro<void> {
                co_await OneShotFuture<int>{0};
                co_await OneShotFuture<int>{0};
                ++completed;
            }));
        try { co_await js.drain(OnError::Wait); }
        catch (const std::runtime_error& e) {
            caught = (std::string(e.what()) == "void task failed");
        }
    }(caught, completed));
    EXPECT_TRUE(caught);
    EXPECT_EQ(completed, 3);
}

TYPED_TEST(JoinSetVoidTest, DrainLosingSelectBranchCancelsTasks) {
    std::size_t winner = 0;
    int dropped_after_select = -1;
    std::atomic<int> dropped = 0;
    this->traits.rt.block_on([](std::size_t& winner, int& dropped_after_select,
                                std::atomic<int>& dropped) -> Coro<void> {
        JoinSet<void> js;
        js.spawn(wait_forever(DropToken{dropped}));
        js.spawn(wait_forever(DropToken{dropped}));
        auto sel = co_await select(js.drain(), ReadyVoidFuture{});
        winner = sel.index();
        dropped_after_select = dropped.load();
        CO_ASSERT(js.empty());
    }(winner, dropped_after_select, dropped));
    EXPECT_EQ(winner, 1u);
    // select() returned only once the drain had seen every task finish.
    EXPECT_EQ(dropped_after_select, 2);
}

TYPED_TEST(JoinSetVoidTest, ShutdownLosingSelectBranchStillWaitsForTasks) {
    std::size_t winner = 0;
    int dropped_after_select = -1;
    std::atomic<int> dropped = 0;
    this->traits.rt.block_on([](std::size_t& winner, int& dropped_after_select,
                                std::atomic<int>& dropped) -> Coro<void> {
        JoinSet<void> js;
        js.spawn(wait_forever(DropToken{dropped}));
        auto sel = co_await select(js.shutdown(), ReadyVoidFuture{});
        winner = sel.index();
        dropped_after_select = dropped.load();
        CO_ASSERT(js.empty());
    }(winner, dropped_after_select, dropped));
    EXPECT_EQ(winner, 1u);
    EXPECT_EQ(dropped_after_select, 1);
}

TYPED_TEST(JoinSetVoidTest, JoinAllLosingSelectBranchCancelsTasks) {
    std::size_t winner = 0;
    int dropped_after_select = -1;
    std::atomic<int> dropped = 0;
    this->traits.rt.block_on([](std::size_t& winner, int& dropped_after_select,
                                std::atomic<int>& dropped) -> Coro<void> {
        JoinSet<void> js;
        js.spawn(wait_forever(DropToken{dropped}));
        js.spawn(wait_forever(DropToken{dropped}));
        auto sel = co_await select(js.join_all(), ReadyVoidFuture{});
        winner = sel.index();
        dropped_after_select = dropped.load();
        CO_ASSERT(js.empty());
    }(winner, dropped_after_select, dropped));
    EXPECT_EQ(winner, 1u);
    EXPECT_EQ(dropped_after_select, 2);
}

namespace {

Coro<int> wait_forever_int([[maybe_unused]] DropToken token) {
    co_await never<void>();
    co_return 0;
}

}  // namespace

TYPED_TEST(JoinSetTest, JoinAllReturnsEveryValue) {
    std::vector<int> results;
    this->traits.rt.block_on([](std::vector<int>& results) -> Coro<void> {
        JoinSet<int> js;
        js.spawn(ReadyFuture<int>{1});
        js.spawn(OneShotFuture<int>{2});
        js.spawn(ReadyFuture<int>{3});
        for (auto& r : co_await js.join_all()) results.push_back(r.value());
        CO_ASSERT(js.empty());
    }(results));
    std::sort(results.begin(), results.end());
    EXPECT_EQ(results, (std::vector<int>{1, 2, 3}));
}

TYPED_TEST(JoinSetTest, JoinAllOfEmptySetReturnsNothing) {
    std::size_t size = 1;
    this->traits.rt.block_on([](std::size_t& size) -> Coro<void> {
        JoinSet<int> js;
        auto values = co_await js.join_all();
        size = values.size();
    }(size));
    EXPECT_EQ(size, 0u);
}

TYPED_TEST(JoinSetTest, JoinAllIntoGivenContainer) {
    std::size_t capacity = 0, size = 0, more_size = 1;
    int sum = 0;
    this->traits.rt.block_on([](std::size_t& capacity, std::size_t& size, std::size_t& more_size,
                                int& sum) -> Coro<void> {
        JoinSet<int> js;
        for (int i = 1; i <= 3; ++i) js.spawn(OneShotFuture<int>{i});
        std::vector<JoinResult<int>> into;
        into.reserve(16);
        auto results = co_await js.join_all(std::move(into));
        capacity = results.capacity();
        size = results.size();
        for (auto& r : results) sum += r.value();
        js.spawn(ReadyFuture<int>{4});
        std::deque<JoinResult<int>> more = co_await js.join_all<std::deque<JoinResult<int>>>();
        more_size = more.size();
    }(capacity, size, more_size, sum));
    EXPECT_GE(capacity, 16u);
    EXPECT_EQ(size, 3u);
    EXPECT_EQ(sum, 6);
    EXPECT_EQ(more_size, 1u);
}

TYPED_TEST(JoinSetTest, JoinAllCancelOnErrorCollectsTheRestAsCancelled) {
    int exceptions = 0, cancelled = 0, values = 0;
    std::atomic<int> dropped = 0;
    this->traits.rt.block_on([](int& exceptions, int& cancelled, int& values,
                                std::atomic<int>& dropped) -> Coro<void> {
        JoinSet<int> js;
        js.spawn(ThrowingFuture{});
        for (int i = 0; i < 2; ++i) js.spawn(wait_forever_int(DropToken{dropped}));
        // Never throws: the failure is in its result.
        for (auto& r : co_await js.join_all()) {
            if (r.exception()) ++exceptions;
            if (r.is_cancelled()) ++cancelled;
            if (r.has_value()) ++values;
        }
    }(exceptions, cancelled, values, dropped));
    EXPECT_EQ(exceptions, 1);
    EXPECT_EQ(cancelled, 2);
    EXPECT_EQ(values, 0);
    EXPECT_EQ(dropped, 2);
}

TYPED_TEST(JoinSetTest, JoinAllWaitLetsEveryTaskFinish) {
    int exceptions = 0, values = 0;
    this->traits.rt.block_on([](int& exceptions, int& values) -> Coro<void> {
        JoinSet<int> js;
        js.spawn(ThrowingFuture{});
        for (int i = 0; i < 3; ++i) js.spawn(OneShotFuture<int>{i});
        for (auto& r : co_await js.join_all(OnError::Wait)) {
            if (r.exception()) ++exceptions;
            if (r.has_value()) ++values;
        }
    }(exceptions, values));
    EXPECT_EQ(exceptions, 1);
    EXPECT_EQ(values, 3);
}

// ---------------------------------------------------------------------------
// Tags
// ---------------------------------------------------------------------------

TYPED_TEST(JoinSetTest, TagsComeBackWithTheirResults) {
    std::vector<std::pair<std::string, int>> results;
    this->traits.rt.block_on([](std::vector<std::pair<std::string, int>>& results) -> Coro<void> {
        JoinSet<int, std::string> js;
        js.spawn("one", ReadyFuture<int>{1});
        js.spawn("two", OneShotFuture<int>{2});
        js.build_task("three", ReadyFuture<int>{3}).name("third").spawn();
        while (auto item = co_await next(js))
            results.emplace_back(item->tag(), item->value());
    }(results));
    std::sort(results.begin(), results.end());
    EXPECT_EQ(results, (std::vector<std::pair<std::string, int>>{
        {"one", 1}, {"three", 3}, {"two", 2}}));
}

TYPED_TEST(JoinSetTest, TagsNeedNotBeUnique) {
    int sum = 0, tagged = 0;
    this->traits.rt.block_on([](int& sum, int& tagged) -> Coro<void> {
        JoinSet<int, int> js;
        for (int i = 1; i <= 3; ++i) js.spawn(7, ReadyFuture<int>{i});
        for (auto& r : co_await js.join_all()) {
            sum += r.value();
            if (r.tag() == 7) ++tagged;
        }
    }(sum, tagged));
    EXPECT_EQ(sum, 6);
    EXPECT_EQ(tagged, 3);
}

TYPED_TEST(JoinSetVoidTest, TagSurvivesExceptionAndCancellation) {
    std::string failed_tag, cancelled_tag;
    std::atomic<int> dropped = 0;
    this->traits.rt.block_on([](std::string& failed_tag, std::string& cancelled_tag,
                                std::atomic<int>& dropped) -> Coro<void> {
        JoinSet<void, std::string> js;
        js.spawn("throws", ThrowingVoidFuture{});
        js.spawn("waits", wait_forever(DropToken{dropped}));
        for (auto& r : co_await js.join_all()) {
            if (r.exception()) failed_tag = r.tag();
            if (r.is_cancelled()) cancelled_tag = std::move(r).tag();
        }
    }(failed_tag, cancelled_tag, dropped));
    EXPECT_EQ(failed_tag, "throws");
    EXPECT_EQ(cancelled_tag, "waits");
}

namespace {

Coro<std::unique_ptr<int>> make_unique_int(int v) {
    co_return std::make_unique<int>(v);
}

}  // namespace

TYPED_TEST(JoinSetTest, MoveOnlyTagAndValue) {
    int sum = 0;
    this->traits.rt.block_on([](int& sum) -> Coro<void> {
        JoinSet<std::unique_ptr<int>, std::unique_ptr<int>> js;
        js.spawn(std::make_unique<int>(10), make_unique_int(5));
        while (auto item = co_await next(js)) {
            std::unique_ptr<int> tag = std::move(item->tag());
            std::unique_ptr<int> value = std::move(*item).value();
            sum = *tag + *value;
        }
    }(sum));
    EXPECT_EQ(sum, 15);
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
            if (item->value() == 99) new_ran = true;
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

// On CurrentThreadExecutor the order is fixed: the pending task finishes second.
TEST(JoinSetSingleThreadedTest, JoinAllIsInCompletionOrder) {
    Runtime rt(std::in_place_type<CurrentThreadExecutor>);
    std::vector<int> order;
    rt.block_on([](std::vector<int>& order) -> Coro<void> {
        JoinSet<int> js;
        js.spawn(OneShotFuture<int>{1});
        js.spawn(ReadyFuture<int>{2});
        for (auto& r : co_await js.join_all()) order.push_back(r.value());
    }(order));
    EXPECT_EQ(order, (std::vector<int>{2, 1}));
}

namespace {

Coro<int> throw_after_a_poll() {
    co_await OneShotFuture<int>{0};
    throw std::runtime_error("second");
    co_return 0;
}

Coro<int> throw_at_once() {
    throw std::runtime_error("first");
    co_return 0;
}

}  // namespace

// The first failure in completion order is the one rethrown.
TEST(JoinSetSingleThreadedTest, DrainRethrowsFirstFailureInCompletionOrder) {
    Runtime rt(std::in_place_type<CurrentThreadExecutor>);
    std::string message;
    rt.block_on([](std::string& message) -> Coro<void> {
        JoinSet<int> js;
        js.spawn(throw_after_a_poll());
        js.spawn(throw_at_once());
        try { co_await js.drain(OnError::Wait); }
        catch (const std::runtime_error& e) { message = e.what(); }
    }(message));
    EXPECT_EQ(message, "first");
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
            result_sum.fetch_add(item->value());
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
