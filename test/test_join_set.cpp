#include <gtest/gtest.h>
#include <coro/coro.h>
#include <coro/co_invoke.h>
#include <coro/task/join_set.h>
#include <coro/runtime/runtime.h>
#include <stdexcept>
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

// -----------------------------------------------------------------------
// Cancel on drop
// -----------------------------------------------------------------------

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
