#include <gtest/gtest.h>
#include <coro/detail/coro_scope.h>
#include <coro/detail/task_state.h>
#include <coro/detail/waker.h>
#include <coro/coro.h>
#include <coro/task/join_handle.h>
#include <coro/runtime/runtime.h>
#include <memory>

using namespace coro;
using namespace coro::detail;

// --- CoroutineScope unit tests ---

TEST(CoroutineScopeTest, EmptyScopeHasNoPending) {
    CoroutineScope scope;
    EXPECT_FALSE(scope.has_pending());
}

TEST(CoroutineScopeTest, AddCompletedChildShowsNoPending) {
    CoroutineScope scope;
    auto state = std::make_shared<TaskState<int>>();
    state->setResult(42);  // already done

    scope.add_child(
        [state]() { return state->is_complete(); },
        [state](std::shared_ptr<Waker> w) {
            std::lock_guard lock(state->mutex);
            state->scope_waker = std::move(w);
        }
    );

    EXPECT_FALSE(scope.has_pending());  // swept immediately
}

TEST(CoroutineScopeTest, AddPendingChildShowsPending) {
    CoroutineScope scope;
    auto state = std::make_shared<TaskState<int>>();
    // Not completed yet

    scope.add_child(
        [state]() { return state->is_complete(); },
        [state](std::shared_ptr<Waker> w) {
            std::lock_guard lock(state->mutex);
            state->scope_waker = std::move(w);
        }
    );

    EXPECT_TRUE(scope.has_pending());
}

TEST(CoroutineScopeTest, PendingChildClearsAfterCompletion) {
    CoroutineScope scope;
    auto state = std::make_shared<TaskState<int>>();

    scope.add_child(
        [state]() { return state->is_complete(); },
        [state](std::shared_ptr<Waker> w) {
            std::lock_guard lock(state->mutex);
            state->scope_waker = std::move(w);
        }
    );

    EXPECT_TRUE(scope.has_pending());
    state->setResult(99);
    EXPECT_FALSE(scope.has_pending());
}

// --- JoinHandle destructor registration ---

TEST(CoroutineScopeTest, JoinHandleDropRegistersWithCurrentScope) {
    CoroutineScope scope;
    auto state = std::make_shared<TaskState<int>>();

    {
        // Simulate being inside a poll() call by setting t_current_coro directly.
        t_current_coro = &scope;
        JoinHandle<int> handle(state);
        // handle goes out of scope here — destructor fires with t_current_coro set
    }
    t_current_coro = nullptr;

    // The task is not yet complete, so it should be in the pending list.
    EXPECT_TRUE(scope.has_pending());
}

TEST(CoroutineScopeTest, DetachedJoinHandleDoesNotRegister) {
    CoroutineScope scope;
    auto state = std::make_shared<TaskState<int>>();

    {
        t_current_coro = &scope;
        JoinHandle<int> handle(state);
        std::move(handle).detach();  // detach clears m_state before destructor runs
    }
    t_current_coro = nullptr;

    EXPECT_FALSE(scope.has_pending());
}

TEST(CoroutineScopeTest, JoinHandleDropWithNullCurrentCoroDoesNotRegister) {
    CoroutineScope scope;
    auto state = std::make_shared<TaskState<int>>();

    {
        // t_current_coro is null — no scope to register with
        ASSERT_EQ(t_current_coro, nullptr);
        JoinHandle<int> handle(state);
    }

    EXPECT_FALSE(scope.has_pending());
}

// --- TaskState::mark_done and scope_waker ---

TEST(TaskStateTest, MarkDoneFiresScopeWaker) {
    class TestWaker : public Waker {
    public:
        bool woken = false;
        void wake() override { woken = true; }
        std::shared_ptr<Waker> clone() override {
            return std::make_shared<TestWaker>();
        }
    };

    auto state = std::make_shared<TaskState<int>>();
    auto waker = std::make_shared<TestWaker>();
    {
        std::lock_guard lock(state->mutex);
        state->scope_waker = waker;
    }
    state->mark_done();
    EXPECT_TRUE(waker->woken);
    EXPECT_TRUE(state->is_complete());
}

TEST(TaskStateTest, SetResultMarksDoneAndFiresScopeWaker) {
    class TestWaker : public Waker {
    public:
        bool woken = false;
        void wake() override { woken = true; }
        std::shared_ptr<Waker> clone() override {
            return std::make_shared<TestWaker>();
        }
    };

    auto state = std::make_shared<TaskState<int>>();
    auto waker = std::make_shared<TestWaker>();
    {
        std::lock_guard lock(state->mutex);
        state->scope_waker = waker;
    }
    state->setResult(42);
    EXPECT_TRUE(waker->woken);
    EXPECT_TRUE(state->is_complete());
}

// --- PollDropped on Coro<T> ---

TEST(CoroScopeTest, CancelledCoroReturnsPollDropped) {
    class MockWaker : public Waker {
    public:
        void wake() override {}
        std::shared_ptr<Waker> clone() override { return std::make_shared<MockWaker>(); }
    };

    auto waker = std::make_shared<MockWaker>();
    Context ctx(waker);

    Coro<void> c = []() -> Coro<void> { co_return; }();
    c.cancel();
    auto result = c.poll(ctx);
    EXPECT_TRUE(result.isDropped());
}

// --- Runtime integration: implicit scope drain ---

// A future that completes immediately on the first poll, writing to a shared counter.
struct WritingFuture {
    using OutputType = void;
    std::shared_ptr<int> dest;
    int value;
    PollResult<void> poll(Context&) { *dest = value; return PollReady; }
};

// Coroutine spawns a child task, drops the JoinHandle, then co_returns.
// The implicit scope must hold completion until the child finishes.
Coro<void> spawns_and_returns(std::shared_ptr<int> out, bool cancel = false) {
    (void)spawn(WritingFuture{out, 42}).submit().cancelOnDestroy(cancel);
    co_return;
}

TEST(CoroutineScopeIntegration, CoroutineWaitsForDroppedChildBeforeCompleting) {
    Runtime rt;
    auto shared = std::make_shared<int>(0);
    rt.block_on(spawns_and_returns(shared, false));
    EXPECT_EQ(*shared, 42);
}

TEST(CoroutineScopeIntegration, CoroutineCancelsDroppedChildBeforeCompleting) {
    Runtime rt;
    auto shared = std::make_shared<int>(0);
    rt.block_on(spawns_and_returns(shared, true));
    EXPECT_EQ(*shared, 0);
}

// A future that completes immediately on the first poll, adding to a shared counter.
struct AddingFuture {
    using OutputType = void;
    std::shared_ptr<int> dest;
    int value;
    PollResult<void> poll(Context&) { *dest += value; return PollReady; }
};

// Multiple dropped children — all must complete before the coroutine finishes.
Coro<void> spawns_multiple(std::shared_ptr<int> counter) {
    (void)spawn(AddingFuture{counter, 1}).submit().cancelOnDestroy(false);
    (void)spawn(AddingFuture{counter, 2}).submit().cancelOnDestroy(false);
    (void)spawn(AddingFuture{counter, 3}).submit().cancelOnDestroy(false);
    co_return;
}

TEST(CoroutineScopeIntegration, CoroutineWaitsForAllDroppedChildren) {
    Runtime rt;
    auto counter = std::make_shared<int>(0);
    // Each WritingFuture overwrites counter; last one to run wins.
    // We just verify all three ran (counter was touched), not the final value.
    rt.block_on(spawns_multiple(counter));
    // All three children ran; counter was written at least once.
    EXPECT_EQ(*counter, 6);
}

// JoinHandle dropped inside a nested scope (child coroutine) — registers with the
// child's scope, not the parent's.
Coro<void> inner_spawn(std::shared_ptr<int> out) {
    (void)spawn(WritingFuture{out, 99}).submit().cancelOnDestroy(false);
    co_return;
}

Coro<void> outer_calls_inner(std::shared_ptr<int> out) {
    co_await inner_spawn(out);
}

TEST(CoroutineScopeIntegration, NestedCoroutineDrainsItsOwnChildren) {
    Runtime rt;
    auto shared = std::make_shared<int>(0);
    rt.block_on(outer_calls_inner(shared));
    EXPECT_EQ(*shared, 99);
}

// NOTE: End-to-end cancellation propagation tests (cancelled coroutine drains
// spawned children before returning PollDropped, and children receive cancellation
// via select/timeout combinators) are deferred until those combinators are
// implemented. See doc/coroutine_scope.md for the full design.
