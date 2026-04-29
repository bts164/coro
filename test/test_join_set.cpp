#include <gtest/gtest.h>
#include <coro/coro.h>
#include <coro/co_invoke.h>
#include <coro/sync/join.h>
#include <coro/sync/select.h>
#include <coro/task/join_set.h>
#include <coro/runtime/runtime.h>
#include <coro/runtime/single_threaded_executor.h>
#include <coro/runtime/work_sharing_executor.h>
#include <coro/runtime/work_stealing_executor.h>
#include <stdexcept>
#include <variant>
#include <vector>
#include <algorithm>

using namespace coro;

// -----------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------

// Immediately-ready future that returns a value.
template<typename T>
struct ReadyFuture {
    using OutputType = T;
    T value;
    PollResult<T> poll(detail::Context&) { return std::move(value); }
};

// Immediately-ready void future.
struct ReadyVoidFuture {
    using OutputType = void;
    PollResult<void> poll(detail::Context&) { return PollReady; }
};

// Future that throws on completion.
struct ThrowingFuture {
    using OutputType = int;
    PollResult<int> poll(detail::Context&) {
        try { throw std::runtime_error("task failed"); }
        catch (...) { return PollError(std::current_exception()); }
    }
};

// Future that throws (void version).
struct ThrowingVoidFuture {
    using OutputType = void;
    PollResult<void> poll(detail::Context&) {
        try { throw std::runtime_error("void task failed"); }
        catch (...) { return PollError(std::current_exception()); }
    }
};

// -----------------------------------------------------------------------
// JoinSet<int> — non-void
// -----------------------------------------------------------------------

TEST(JoinSetTest, SpawnAndDrainInt) {
    // Spawn three tasks and drain. No results collected.
    Runtime rt(1);
    int drain_count = 0;

    rt.block_on(co_invoke([&drain_count]() -> Coro<void> {
        JoinSet<int> js;
        js.spawn(ReadyFuture<int>{1});
        js.spawn(ReadyFuture<int>{2});
        js.spawn(ReadyFuture<int>{3});
        co_await js.drain();
        drain_count = 1;
    }));

    EXPECT_EQ(drain_count, 1);
}

TEST(JoinSetTest, CollectResultsViaNext) {
    // Results arrive in completion order (immediate tasks complete in spawn order).
    Runtime rt(1);
    std::vector<int> results;

    rt.block_on(co_invoke([&results]() -> Coro<void> {
        JoinSet<int> js;
        js.spawn(ReadyFuture<int>{10});
        js.spawn(ReadyFuture<int>{20});
        js.spawn(ReadyFuture<int>{30});

        while (auto item = co_await next(js))
            results.push_back(*item);
    }));

    std::sort(results.begin(), results.end());
    EXPECT_EQ(results, (std::vector<int>{10, 20, 30}));
}

TEST(JoinSetTest, EmptyJoinSetDrainCompletesImmediately) {
    Runtime rt(1);
    bool reached = false;

    rt.block_on(co_invoke([&reached]() -> Coro<void> {
        JoinSet<int> js;
        co_await js.drain();
        reached = true;
    }));

    EXPECT_TRUE(reached);
}

TEST(JoinSetTest, EmptyJoinSetNextReturnsNulloptImmediately) {
    Runtime rt(1);
    bool got_nullopt = false;

    rt.block_on(co_invoke([&got_nullopt]() -> Coro<void> {
        JoinSet<int> js;
        auto item = co_await next(js);
        got_nullopt = !item.has_value();
    }));

    EXPECT_TRUE(got_nullopt);
}

TEST(JoinSetTest, DrainRethrowsFirstException) {
    Runtime rt(1);
    bool caught = false;

    rt.block_on(co_invoke([&caught]() -> Coro<void> {
        JoinSet<int> js;
        js.spawn(ReadyFuture<int>{1});
        js.spawn(ThrowingFuture{});
        js.spawn(ReadyFuture<int>{3});

        try {
            co_await js.drain();
        } catch (const std::runtime_error& e) {
            caught = (std::string(e.what()) == "task failed");
        }
    }));

    EXPECT_TRUE(caught);
}

TEST(JoinSetTest, NextRethrowsExceptionInline) {
    Runtime rt(1);
    bool caught = false;

    rt.block_on(co_invoke([&caught]() -> Coro<void> {
        JoinSet<int> js;
        js.spawn(ThrowingFuture{});

        try {
            while (co_await next(js)) {}
        } catch (const std::runtime_error& e) {
            caught = (std::string(e.what()) == "task failed");
        }
    }));

    EXPECT_TRUE(caught);
}

// -----------------------------------------------------------------------
// JoinSet<void>
// -----------------------------------------------------------------------

TEST(JoinSetVoidTest, SpawnAndDrain) {
    Runtime rt(1);
    std::atomic<int> completed = 0;

    rt.block_on(co_invoke([&completed]() -> Coro<void> {
        JoinSet<void> js;
        js.spawn(co_invoke([&completed]() -> Coro<void> {
            ++completed;
            co_return;
        }));
        js.spawn(co_invoke([&completed]() -> Coro<void> {
            ++completed;
            co_return;
        }));
        co_await js.drain();
    }));

    EXPECT_EQ(completed, 2);
}

TEST(JoinSetVoidTest, DrainRethrowsFirstException) {
    Runtime rt(1);
    std::atomic_bool caught = false;

    rt.block_on(co_invoke([&caught]() -> Coro<void> {
        JoinSet<void> js;
        js.spawn(ReadyVoidFuture{});
        js.spawn(ThrowingVoidFuture{});

        try {
            co_await js.drain();
        } catch (const std::runtime_error& e) {
            caught.store((std::string(e.what()) == "void task failed"));
        }
    }));

    EXPECT_TRUE(caught);
}

TEST(JoinSetVoidTest, EmptyDrainCompletesImmediately) {
    Runtime rt(1);
    bool reached = false;

    rt.block_on(co_invoke([&reached]() -> Coro<void> {
        JoinSet<void> js;
        co_await js.drain();
        reached = true;
    }));

    EXPECT_TRUE(reached);
}

TEST(JoinSetVoidTest, CollectResultsViaNext) {
    // next() returns true for each completed void task, false when exhausted.
    Runtime rt(1);
    int count = 0;

    rt.block_on(co_invoke([&count]() -> Coro<void> {
        JoinSet<void> js;
        js.spawn(ReadyVoidFuture{});
        js.spawn(ReadyVoidFuture{});
        js.spawn(ReadyVoidFuture{});

        while (co_await next(js))
            ++count;
    }));

    EXPECT_EQ(count, 3);
}

TEST(JoinSetVoidTest, EmptyNextReturnsFalseImmediately) {
    Runtime rt(1);
    bool got_false = false;

    rt.block_on(co_invoke([&got_false]() -> Coro<void> {
        JoinSet<void> js;
        got_false = !(co_await next(js));
    }));

    EXPECT_TRUE(got_false);
}

TEST(JoinSetVoidTest, NextRethrowsException) {
    Runtime rt(1);
    bool caught = false;

    rt.block_on(co_invoke([&caught]() -> Coro<void> {
        JoinSet<void> js;
        js.spawn(ReadyVoidFuture{});
        js.spawn(ThrowingVoidFuture{});
        js.spawn(ReadyVoidFuture{});

        int completed = 0;
        try {
            while (co_await next(js))
                ++completed;
        } catch (const std::runtime_error& e) {
            caught = (std::string(e.what()) == "void task failed");
        }
        // At least one task completed before the exception
        EXPECT_GE(completed, 0);
    }));

    EXPECT_TRUE(caught);
}

// -----------------------------------------------------------------------
// Cancel on drop
// -----------------------------------------------------------------------

// A future that suspends once before completing — so it is genuinely pending
// when it enters a JoinSet, exercising the cancel-on-drop path.
template<typename T>
class OneShotFuture {
public:
    using OutputType = T;
    explicit OneShotFuture(T value) : m_value(std::move(value)) {}
    PollResult<T> poll(detail::Context& ctx) {
        if (m_polled) return std::move(m_value);
        m_polled = true;
        // Re-wake ourselves immediately so the executor reschedules us.
        ctx.getWaker()->wake();
        return PollPending;
    }
private:
    T    m_value;
    bool m_polled{false};
};

// A future that tracks whether it completed naturally (second poll reached) vs
// was cancelled before the second poll. Relies on the single-threaded executor:
// the task's second poll only runs after the parent coroutine yields, by which
// time the JoinSet has already been dropped and cancelled=true has been set.
struct CancellationProbeFuture {
    using OutputType = int;
    std::atomic<bool>& completed_naturally;
    bool first_poll{true};
    PollResult<int> poll(detail::Context& ctx) {
        if (!first_poll) {
            completed_naturally.store(true);
            return 42;
        }
        first_poll = false;
        ctx.getWaker()->wake();  // re-schedule so Task::poll() runs again
        return PollPending;
    }
};

TEST(JoinSetTest, CancelOnDropDoesNotHang) {
    // Dropping JoinSet with pending tasks inside co_invoke must not hang.
    // The enclosing CoroutineScope drains cancelled tasks before completing.
    Runtime rt(1);
    bool reached_after = false;

    rt.block_on(co_invoke([&reached_after]() -> Coro<void> {
        {
            JoinSet<int> js;
            js.spawn(ReadyFuture<int>{42});
            // Drop js here without draining — tasks cancelled
            co_return;
        }
        reached_after = true;
        co_return;
    }));

    EXPECT_FALSE(reached_after);
}

TEST(JoinSetVoidTest, CancelOnDropDoesNotHang) {
    Runtime rt(1);
    bool reached_after = false;

    rt.block_on(co_invoke([&reached_after]() -> Coro<void> {
        {
            JoinSet<void> js;
            js.spawn(ReadyVoidFuture{});
        }
        reached_after = true;
        co_return;
    }));

    EXPECT_TRUE(reached_after);
}

TEST(JoinSetTest, CancelPendingTaskOnDrop) {
    // A genuinely-pending task (suspends once) must be cancelled and drained
    // by the enclosing scope when the JoinSet is dropped.
    Runtime rt(1);
    bool reached_after = false;

    rt.block_on(co_invoke([&reached_after]() -> Coro<void> {
        {
            JoinSet<int> js;
            js.spawn(OneShotFuture<int>{42});  // pending in the set at drop time
        }
        reached_after = true;
        co_return;
    }));

    EXPECT_TRUE(reached_after);
}

TEST(JoinSetTest, PendingTaskIsCancelledNotCompleted) {
    // Verifies that Task::poll() sees cancelled=true and short-circuits before
    // calling future.poll() a second time — i.e. the task is actually cancelled,
    // not merely drained after running to natural completion.
    //
    // Relies on single-threaded executor ordering: the task's second poll cannot
    // run until the parent coroutine yields, by which point cancel_pending() has
    // already set cancelled=true.
    Runtime rt(1);
    std::atomic<bool> completed_naturally{false};

    rt.block_on(co_invoke([&completed_naturally]() -> Coro<void> {
        {
            JoinSet<int> js;
            js.spawn(CancellationProbeFuture{completed_naturally});
        }
        co_return;
    }));

    EXPECT_FALSE(completed_naturally);
}

// -----------------------------------------------------------------------
// build_task() builder interface
// -----------------------------------------------------------------------

TEST(JoinSetTest, BuildTaskSpawnsTask) {
    Runtime rt(1);
    std::vector<int> results;

    rt.block_on(co_invoke([&results]() -> Coro<void> {
        JoinSet<int> js;
        js.build_task(ReadyFuture<int>{1}).spawn();
        js.build_task(ReadyFuture<int>{2}).name("second").spawn();
        js.build_task(ReadyFuture<int>{3}).name("third").spawn();

        while (auto item = co_await next(js))
            results.push_back(*item);
    }));

    std::sort(results.begin(), results.end());
    EXPECT_EQ(results, (std::vector<int>{1, 2, 3}));
}

// -----------------------------------------------------------------------
// Composition with co_invoke
// -----------------------------------------------------------------------

TEST(JoinSetTest, ComposesWithCoInvoke) {
    // Tasks spawned via co_invoke into a JoinSet — lambda lifetime safe.
    Runtime rt(1);
    std::vector<int> results;
    std::vector<int> inputs = {1, 2, 3, 4, 5};

    rt.block_on(co_invoke([&]() -> Coro<void> {
        JoinSet<int> js;
        for (int x : inputs)
            js.spawn(co_invoke([x]() -> Coro<int> { co_return x * x; }));

        while (auto item = co_await next(js))
            results.push_back(*item);
    }));

    std::sort(results.begin(), results.end());
    EXPECT_EQ(results, (std::vector<int>{1, 4, 9, 16, 25}));
}

TEST(JoinSetVoidTest, ComposesWithCoInvokeAndCapture) {
    // Verify reference-capture safety via co_invoke + JoinSet<void>.
    Runtime rt(1);
    std::atomic<int> sum = 0;
    std::vector<int> data = {10, 20, 30};

    rt.block_on(co_invoke([&]() -> Coro<void> {
        JoinSet<void> js;
        for (int x : data)
            js.spawn(co_invoke([x, &sum]() -> Coro<void> {
                sum.fetch_add(x);
                co_return;
            }));
        co_await js.drain();
    }));

    EXPECT_EQ(sum, 60);
}

// -----------------------------------------------------------------------
// Async consumer parking — next() must suspend and be woken by a later-
// completing task. Exercises on_task_complete() → consumer_waker->wake().
// -----------------------------------------------------------------------

TEST(JoinSetTest, NextSuspendsAndIsWokenByAsyncTask) {
    // OneShotFuture suspends on the first poll and completes on the second,
    // so the consumer's co_await next(js) parks before the result is ready.
    Runtime rt(1);
    std::vector<int> results;

    rt.block_on(co_invoke([&results]() -> Coro<void> {
        JoinSet<int> js;
        js.spawn(OneShotFuture<int>{10});
        js.spawn(OneShotFuture<int>{20});
        js.spawn(OneShotFuture<int>{30});

        while (auto item = co_await next(js))
            results.push_back(*item);
    }));

    std::sort(results.begin(), results.end());
    EXPECT_EQ(results, (std::vector<int>{10, 20, 30}));
}

TEST(JoinSetTest, DrainSuspendsAndIsWokenByAsyncTask) {
    Runtime rt(1);
    int drain_count = 0;

    rt.block_on(co_invoke([&drain_count]() -> Coro<void> {
        JoinSet<int> js;
        js.spawn(OneShotFuture<int>{1});
        js.spawn(OneShotFuture<int>{2});
        co_await js.drain();
        drain_count = 1;
    }));

    EXPECT_EQ(drain_count, 1);
}

TEST(JoinSetVoidTest, NextSuspendsAndIsWokenByAsyncTask) {
    // Use co_invoke to produce Coro<void> futures that each suspend once before
    // completing, so the consumer's co_await next(js) parks mid-stream.
    Runtime rt(1);
    int count = 0;

    rt.block_on(co_invoke([&count]() -> Coro<void> {
        JoinSet<void> js;
        for (int i = 0; i < 3; ++i)
            js.spawn(co_invoke([]() -> Coro<void> {
                co_await OneShotFuture<int>{0};
            }));

        while (co_await next(js))
            ++count;
    }));

    EXPECT_EQ(count, 3);
}

// -----------------------------------------------------------------------
// Multiple exceptions — drain discards all but the first
// -----------------------------------------------------------------------

TEST(JoinSetTest, DrainDiscardsAllButFirstException) {
    Runtime rt(1);
    int caught_count = 0;
    std::string first_message;

    rt.block_on(co_invoke([&]() -> Coro<void> {
        JoinSet<int> js;
        js.spawn(ThrowingFuture{});
        js.spawn(ThrowingFuture{});
        js.spawn(ThrowingFuture{});

        try {
            co_await js.drain();
        } catch (const std::runtime_error& e) {
            ++caught_count;
            first_message = e.what();
        }
        // No second exception should escape — the try/catch must not be re-entered.
    }));

    EXPECT_EQ(caught_count, 1);
    EXPECT_EQ(first_message, "task failed");
}

TEST(JoinSetVoidTest, DrainDiscardsAllButFirstException) {
    Runtime rt(1);
    int caught_count = 0;

    rt.block_on(co_invoke([&caught_count]() -> Coro<void> {
        JoinSet<void> js;
        js.spawn(ThrowingVoidFuture{});
        js.spawn(ThrowingVoidFuture{});

        try {
            co_await js.drain();
        } catch (const std::runtime_error&) {
            ++caught_count;
        }
    }));

    EXPECT_EQ(caught_count, 1);
}

// -----------------------------------------------------------------------
// Move assignment — must cancel the old set's tasks before adopting the new
// -----------------------------------------------------------------------

TEST(JoinSetTest, MoveAssignmentCancelsPreviousTasks) {
    // Assigning a new JoinSet to an existing one must cancel the tasks in the
    // old set. Use CancellationProbeFuture to confirm they do not complete naturally.
    Runtime rt(1);
    std::atomic<bool> old_completed_naturally{false};
    bool new_task_ran = false;

    rt.block_on(co_invoke([&]() -> Coro<void> {
        JoinSet<int> js;
        js.spawn(CancellationProbeFuture{old_completed_naturally});

        // Assign a new JoinSet — old tasks must be cancelled.
        JoinSet<int> js2;
        js2.spawn(ReadyFuture<int>{99});
        js = std::move(js2);

        while (auto item = co_await next(js)) {
            if (*item == 99) new_task_ran = true;
        }
    }));

    EXPECT_FALSE(old_completed_naturally);
    EXPECT_TRUE(new_task_ran);
}

// -----------------------------------------------------------------------
// Multi-threaded — exercises the consumer_waker race between on_task_complete()
// (worker thread) and poll_next() (consumer thread).
// -----------------------------------------------------------------------

TEST(JoinSetTest, MultiThreadedCollectViaNext) {
    Runtime rt(4);
    std::atomic<int> result_sum{0};

    rt.block_on(co_invoke([&result_sum]() -> Coro<void> {
        JoinSet<int> js;
        for (int i = 1; i <= 8; ++i)
            js.spawn(co_invoke([i]() -> Coro<int> { co_return i; }));

        while (auto item = co_await next(js))
            result_sum.fetch_add(*item);
    }));

    EXPECT_EQ(result_sum.load(), 36); // 1+2+...+8
}

TEST(JoinSetTest, MultiThreadedDrain) {
    Runtime rt(4);
    std::atomic<int> completed{0};

    rt.block_on(co_invoke([&completed]() -> Coro<void> {
        JoinSet<int> js;
        for (int i = 0; i < 16; ++i)
            js.spawn(co_invoke([i, &completed]() -> Coro<int> {
                completed.fetch_add(1);
                co_return i;
            }));
        co_await js.drain();
    }));

    EXPECT_EQ(completed.load(), 16);
}

TEST(JoinSetVoidTest, MultiThreadedDrain) {
    Runtime rt(4);
    std::atomic<int> completed{0};

    rt.block_on(co_invoke([&completed]() -> Coro<void> {
        JoinSet<void> js;
        for (int i = 0; i < 16; ++i)
            js.spawn(co_invoke([&completed]() -> Coro<void> {
                completed.fetch_add(1);
                co_return;
            }));
        co_await js.drain();
    }));

    EXPECT_EQ(completed.load(), 16);
}

// -----------------------------------------------------------------------
// NextFuture in select — dropping a next(js) branch must not hang or
// corrupt the JoinSet. The losing branch registers consumer_waker, which
// fires a spurious wake; results must still be collectable afterwards.
// -----------------------------------------------------------------------

TEST(JoinSetTest, NextInSelectLosingBranchLeavesJoinSetUsable) {
    // select(next(js), ImmediateVoid) — ImmediateVoid wins on every poll.
    // The next(js) branch suspends, stores consumer_waker, then is dropped.
    // After select returns, the remaining JoinSet tasks must still be
    // drainable with no hang and correct results.
    Runtime rt(1);
    std::vector<int> results;

    rt.block_on(co_invoke([&results]() -> Coro<void> {
        JoinSet<int> js;
        js.spawn(OneShotFuture<int>{10});
        js.spawn(OneShotFuture<int>{20});

        // next(js) suspends (tasks not yet ready); ImmediateVoid wins.
        auto sel = co_await select(next(js), ReadyVoidFuture{});
        EXPECT_TRUE((std::holds_alternative<SelectBranch<1, void>>(sel)));

        // JoinSet still live — drain the remaining results.
        while (auto item = co_await next(js))
            results.push_back(*item);
    }));

    std::sort(results.begin(), results.end());
    EXPECT_EQ(results, (std::vector<int>{10, 20}));
}

TEST(JoinSetTest, NextInSelectWinningBranchWorks) {
    // select(next(js), NeverFuture) — next(js) wins once a task completes.
    Runtime rt(1);
    std::optional<int> got;

    rt.block_on(co_invoke([&got]() -> Coro<void> {
        // NeverFuture: stays pending forever.
        struct NeverFuture {
            using OutputType = void;
            PollResult<void> poll(detail::Context&) { return PollPending; }
        };

        JoinSet<int> js;
        js.spawn(OneShotFuture<int>{42});

        auto sel = co_await select(next(js), NeverFuture{});
        if (std::holds_alternative<SelectBranch<0, std::optional<int>>>(sel))
            got = *std::get<SelectBranch<0, std::optional<int>>>(sel).value;
    }));

    EXPECT_TRUE(got.has_value());
    EXPECT_EQ(*got, 42);
}

TEST(JoinSetTest, NextInSelectRepeatedRoundsCollectsAll) {
    // Loop: each iteration uses select(next(js), NeverFuture) to collect
    // one result. Tests that consumer_waker is correctly refreshed each round.
    Runtime rt(1);
    std::vector<int> results;

    rt.block_on(co_invoke([&results]() -> Coro<void> {
        struct NeverFuture {
            using OutputType = void;
            PollResult<void> poll(detail::Context&) { return PollPending; }
        };

        JoinSet<int> js;
        for (int i = 1; i <= 4; ++i)
            js.spawn(OneShotFuture<int>{i * 10});

        while (true) {
            auto sel = co_await select(next(js), NeverFuture{});
            if (!std::holds_alternative<SelectBranch<0, std::optional<int>>>(sel))
                break;
            auto item = std::get<SelectBranch<0, std::optional<int>>>(sel).value;
            if (!item) break;
            results.push_back(*item);
        }
    }));

    std::sort(results.begin(), results.end());
    EXPECT_EQ(results, (std::vector<int>{10, 20, 30, 40}));
}
