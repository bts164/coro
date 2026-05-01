#include <gtest/gtest.h>
#include <coro/detail/coro_scope.h>
#include <coro/detail/task_state.h>
#include <coro/detail/task.h>
#include <coro/detail/waker.h>
#include <coro/coro.h>
#include <coro/task/join_handle.h>
#include <coro/runtime/runtime.h>
#include <atomic>
#include <memory>

using namespace coro;
using namespace coro::detail;

// Minimal futures used in unit tests. NeverFuture<T> stays pending forever;
// it is used to create a TaskImpl that looks pending from the scope's perspective.
template<typename T>
struct NeverFuture {
    using OutputType = T;
    PollResult<T> poll(Context&) { return PollPending; }
};

// Helper: make an OwnedTask wrapping a pending TaskImpl<NeverFuture<int>> and return
// both the OwnedTask (for the scope) and the TaskState (for external completion control).
static std::pair<OwnedTask, std::shared_ptr<TaskState<int>>> make_pending_int_task() {
    auto impl = std::make_shared<TaskImpl<NeverFuture<int>>>(NeverFuture<int>{});
    std::shared_ptr<TaskState<int>> state = impl;  // aliased; same allocation
    OwnedTask owned{std::shared_ptr<TaskBase>(impl)};
    return {std::move(owned), std::move(state)};
}

// Helper: make an already-completed OwnedTask.
static OwnedTask make_done_int_task(int value) {
    auto impl = std::make_shared<TaskImpl<NeverFuture<int>>>(NeverFuture<int>{});
    impl->setResult(value);  // mark terminated immediately
    return OwnedTask(std::shared_ptr<TaskBase>(impl));
}

// --- CoroutineScope unit tests ---

TEST(CoroutineScopeTest, EmptyScopeHasNoPending) {
    CoroutineScope scope;
    EXPECT_FALSE(scope.has_pending());
}

TEST(CoroutineScopeTest, AddCompletedChildShowsNoPending) {
    CoroutineScope scope;
    scope.add_child(make_done_int_task(42));
    EXPECT_FALSE(scope.has_pending());  // swept immediately
}

TEST(CoroutineScopeTest, AddPendingChildShowsPending) {
    CoroutineScope scope;
    auto [owned, state] = make_pending_int_task();
    scope.add_child(std::move(owned));
    EXPECT_TRUE(scope.has_pending());
}

TEST(CoroutineScopeTest, PendingChildClearsAfterCompletion) {
    CoroutineScope scope;
    auto [owned, state] = make_pending_int_task();
    scope.add_child(std::move(owned));

    EXPECT_TRUE(scope.has_pending());
    state->setResult(99);
    EXPECT_FALSE(scope.has_pending());
}

// --- JoinHandle destructor registration ---

TEST(CoroutineScopeTest, JoinHandleDropRegistersWithCurrentScope) {
    CoroutineScope scope;
    auto impl = std::make_shared<TaskImpl<NeverFuture<int>>>(NeverFuture<int>{});
    std::shared_ptr<TaskState<int>> state = impl;
    OwnedTask owned{std::shared_ptr<TaskBase>(impl)};
    impl.reset();  // scope/JoinHandle now hold the only refs

    {
        // Simulate being inside a poll() call by setting t_current_coro directly.
        t_current_coro = &scope;
        JoinHandle<int> handle(std::move(state), std::move(owned));
        // destructor fires with t_current_coro set → transfers OwnedTask to scope
    }
    t_current_coro = nullptr;

    // The task is not yet complete, so it should be in the pending list.
    EXPECT_TRUE(scope.has_pending());
}

TEST(CoroutineScopeTest, DetachedJoinHandleDoesNotRegister) {
    CoroutineScope scope;
    auto impl = std::make_shared<TaskImpl<NeverFuture<int>>>(NeverFuture<int>{});
    std::shared_ptr<TaskState<int>> state = impl;
    OwnedTask owned{std::shared_ptr<TaskBase>(impl)};
    impl.reset();

    {
        t_current_coro = &scope;
        JoinHandle<int> handle(std::move(state), std::move(owned));
        std::move(handle).detach();  // detach sets self_owned on state, clears m_state/m_owned
    }
    t_current_coro = nullptr;

    EXPECT_FALSE(scope.has_pending());
}

TEST(CoroutineScopeTest, JoinHandleDropWithNullCurrentCoroDoesNotRegister) {
    CoroutineScope scope;
    auto impl = std::make_shared<TaskImpl<NeverFuture<int>>>(NeverFuture<int>{});
    std::shared_ptr<TaskState<int>> state = impl;
    OwnedTask owned{std::shared_ptr<TaskBase>(impl)};
    impl.reset();

    {
        // t_current_coro is null — no scope to register with
        ASSERT_EQ(t_current_coro, nullptr);
        JoinHandle<int> handle(std::move(state), std::move(owned));
        // destructor: t_current_coro null + cancelOnDestroy=true → cancel() called
        // (no executor registered, so just sets cancelled=true) → not registered with scope
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
        state->waker = waker;
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
        state->waker = waker;
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
    (void)spawn(WritingFuture{out, 42}).cancelOnDestroy(cancel);
    co_return;
}

class MarkExitSequence {
public:
    static inline std::atomic_int64_t s_next = 0;
    MarkExitSequence(std::atomic_int64_t *i) :
        m_value(i)
    {}
    MarkExitSequence(MarkExitSequence &&other) :
        m_value(std::exchange(other.m_value, nullptr))
    {}
    MarkExitSequence& operator=(MarkExitSequence &&other) {
        m_value = std::exchange(other.m_value, nullptr);
        return *this;
    }
    ~MarkExitSequence() {
        if (nullptr != m_value) {
            m_value->store(s_next.fetch_add(1));
        }
    }
private:
    std::atomic_int64_t *m_value = nullptr;
};

class EventFuture
{
    struct SharedState {
        std::mutex m_mutex;
        bool m_set = false;
        std::shared_ptr<coro::detail::Waker> m_waker;
    };
    std::shared_ptr<SharedState> m_state;
public:
    using OutputType = void;
    EventFuture() :
        m_state(new SharedState())
    {}
    EventFuture(EventFuture const &) = default;
    EventFuture& operator=(EventFuture  const &) = default;
    ~EventFuture() = default;
    coro::PollResult<void> poll(coro::detail::Context ctx) {
        std::unique_lock lk(m_state->m_mutex);
        if (m_state->m_set) {
            return PollReady;
        }
        m_state->m_waker = ctx.getWaker();
        return PollPending;
    }
    void set() {
        std::unique_lock lk(m_state->m_mutex);
        if (!std::exchange(m_state->m_set, true)) {
            if (auto waker = std::exchange(m_state->m_waker, nullptr); waker) {
                lk.unlock();
                waker->wake();
            }
        }
    }
};

std::atomic_int64_t scopeSequenceLocalRoot = -1;
std::atomic_int64_t scopeSequenceArgOuter = -1;
std::atomic_int64_t scopeSequenceArgInner = -1;
std::atomic_int64_t scopeSequenceLocalOuter = -1;
std::atomic_int64_t scopeSequenceLocalInner = -1;
EventFuture rootEvent;
EventFuture outerEvent;
EventFuture innerEvent;

TEST(CoroutineScopeIntegration, ScopeLifetimeSequence) {
    Runtime rt(1);
    rt.block_on([]() -> coro::Coro<void> {
        MarkExitSequence local(&scopeSequenceLocalRoot);
        auto handle = coro::spawn([](MarkExitSequence) -> coro::Coro<void> {
            // if local were passed by ref to the inner coroutine, then it's lifetime
            // needs to extend past when the cancel of inner completes and it it drained
            MarkExitSequence local(&scopeSequenceLocalOuter);
            co_await coro::spawn([](MarkExitSequence) -> coro::Coro<void> {
                MarkExitSequence local(&scopeSequenceLocalInner);
                rootEvent.set();
                co_await innerEvent;
                co_return;
            }(MarkExitSequence(&scopeSequenceArgInner)));
            co_return;
        }(MarkExitSequence(&scopeSequenceArgOuter)));
        co_await rootEvent;
        co_return;
    }());
    EXPECT_EQ((scopeSequenceLocalRoot.load()),  0);
    EXPECT_EQ((scopeSequenceLocalInner.load()), 1);
    EXPECT_EQ((scopeSequenceArgInner.load()),   2);
    EXPECT_EQ((scopeSequenceLocalOuter.load()), 3);
    EXPECT_EQ((scopeSequenceArgOuter.load()),   4);
}

TEST(CoroutineScopeIntegration, CoroutineWaitsForDroppedChildBeforeCompleting) {
    Runtime rt(1);
    auto shared = std::make_shared<int>(0);
    rt.block_on(spawns_and_returns(shared, false));
    EXPECT_EQ(*shared, 42);
}

TEST(CoroutineScopeIntegration, CoroutineCancelsDroppedChildBeforeCompleting) {
    Runtime rt(1);
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
    (void)spawn(AddingFuture{counter, 1}).cancelOnDestroy(false);
    (void)spawn(AddingFuture{counter, 2}).cancelOnDestroy(false);
    (void)spawn(AddingFuture{counter, 3}).cancelOnDestroy(false);
    co_return;
}

TEST(CoroutineScopeIntegration, CoroutineWaitsForAllDroppedChildren) {
    Runtime rt(1);
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
    (void)spawn(WritingFuture{out, 99}).cancelOnDestroy(false);
    co_return;
}

Coro<void> outer_calls_inner(std::shared_ptr<int> out) {
    co_await inner_spawn(out);
}

// Inner coroutine that spawns a fire-and-forget scope child then blocks on a
// non-Cancellable future. Used by CancelledOuterDrainsInnerCoroScopeChildren.
Coro<void> inner_with_scope_child(std::shared_ptr<std::atomic<int>> counter,
                                  EventFuture blocker) {
    (void)spawn([](std::shared_ptr<std::atomic<int>> counter) -> Coro<void> {
        counter->fetch_add(1);
        co_return;
    }(counter)).cancelOnDestroy(false);
    co_await blocker;
}

TEST(CoroutineScopeIntegration, NestedCoroutineDrainsItsOwnChildren) {
    Runtime rt(1);
    auto shared = std::make_shared<int>(0);
    rt.block_on(outer_calls_inner(shared));
    EXPECT_EQ(*shared, 99);
}

// ─────────────────────────────────────────────────────────────────────────────
// Cancellation integration tests
// Tests below this line exercise the new cooperative-cancel protocol described in
// doc/coroutine_scope.md. They require the following changes to be implemented:
//   1. Coro<T>::poll() cancellation path: drain Cancellable awaited future before
//      calling handle.destroy() (non-Cancellable futures are dropped immediately).
//   2. CoroStream::poll_next() cancellation path: same treatment.
//   3. CoroutineScope: value member with custom move (no unique_ptr).
//   4. transfer_to removed — co_awaited inner Coros are fully drained via the
//      cooperative cancel protocol before the outer frame is torn down.
// Tests that pass today are marked [PASSING]. Tests requiring the above changes
// are unmarked and expected to fail until the implementation is complete.
// ─────────────────────────────────────────────────────────────────────────────

// Test 1: A task sleeping at co_await must actually wake and cancel when its
// JoinHandle is destroyed.  Without a waker clone the wake() call is a no-op and
// the scope drain would deadlock.
TEST(CoroutineScopeIntegration, SleepingTaskWakesOnCancel) {
    Runtime rt(1);
    auto counter = std::make_shared<std::atomic<int>>(0);
    EventFuture started;
    EventFuture blocker; // never set

    rt.block_on([](std::shared_ptr<std::atomic<int>> counter,
                   EventFuture started, EventFuture blocker) -> Coro<void> {
        auto handle = spawn([](std::shared_ptr<std::atomic<int>> counter,
                               EventFuture started, EventFuture blocker) -> Coro<void> {
            counter->fetch_add(1);
            started.set();
            co_await blocker;          // stores waker clone, then parks
            counter->fetch_add(1);     // must never execute
        }(counter, started, blocker));
        co_await started;
        // handle destroyed → child cancelled → waker fires → child cleans up
    }(counter, started, blocker));

    EXPECT_EQ(counter->load(), 1);
}

// Test 2: root cancels A (Cancellable via JoinHandle); A cooperatively cancels B
// (also Cancellable); B eagerly destroys (EventFuture is not Cancellable).
// B's locals must be destroyed before A's.
TEST(CoroutineScopeIntegration, ThreeLevelCooperativeCancelOrdering) {
    MarkExitSequence::s_next.store(0);
    std::atomic_int64_t seqA{-1}, seqB{-1};
    EventFuture innerBlocker; // never set
    EventFuture innerStarted;

    Runtime rt(1);
    rt.block_on([](std::atomic_int64_t* seqA, std::atomic_int64_t* seqB,
                   EventFuture innerBlocker, EventFuture innerStarted) -> Coro<void> {
        auto handle = spawn([](std::atomic_int64_t* seqA, std::atomic_int64_t* seqB,
                               EventFuture innerBlocker,
                               EventFuture innerStarted) -> Coro<void> {
            MarkExitSequence localA(seqA);
            co_await spawn([](std::atomic_int64_t* seqB,
                              EventFuture innerBlocker,
                              EventFuture innerStarted) -> Coro<void> {
                MarkExitSequence localB(seqB);
                innerStarted.set();
                co_await innerBlocker;
            }(seqB, innerBlocker, innerStarted));
        }(seqA, seqB, innerBlocker, innerStarted));
        co_await innerStarted;
        // handle destroyed → A cancelled → A cooperative-cancels B → B eagerly destroyed
    }(&seqA, &seqB, innerBlocker, innerStarted));

    EXPECT_GE(seqB.load(), 0);
    EXPECT_GE(seqA.load(), 0);
    EXPECT_LT(seqB.load(), seqA.load()); // B's locals die before A's
}

// Test 3: When a coroutine is cancelled it must complete BOTH the cooperative
// cancel of a co_awaited child AND the scope drain of a fire-and-forget child.
TEST(CoroutineScopeIntegration, CooperativeCancelWithScopeDrain) {
    Runtime rt(1);
    auto coopCounter  = std::make_shared<std::atomic<int>>(0);
    auto scopeCounter = std::make_shared<std::atomic<int>>(0);
    EventFuture coopBlocker; // never set — blocks the co_awaited child
    EventFuture outerStarted;

    rt.block_on([](std::shared_ptr<std::atomic<int>> coopCounter,
                   std::shared_ptr<std::atomic<int>> scopeCounter,
                   EventFuture coopBlocker,
                   EventFuture outerStarted) -> Coro<void> {
        auto handle = spawn([](std::shared_ptr<std::atomic<int>> coopCounter,
                               std::shared_ptr<std::atomic<int>> scopeCounter,
                               EventFuture coopBlocker,
                               EventFuture outerStarted) -> Coro<void> {
            // fire-and-forget child registered in scope (not cancelled when outer is)
            (void)spawn([](std::shared_ptr<std::atomic<int>> scopeCounter) -> Coro<void> {
                scopeCounter->fetch_add(1);
                co_return;
            }(scopeCounter)).cancelOnDestroy(false);

            outerStarted.set();

            // co_awaited child — Cancellable, triggers cooperative cancel path
            co_await spawn([](std::shared_ptr<std::atomic<int>> coopCounter,
                              EventFuture coopBlocker) -> Coro<void> {
                co_await coopBlocker;
                coopCounter->fetch_add(1); // must not execute
            }(coopCounter, coopBlocker));
        }(coopCounter, scopeCounter, coopBlocker, outerStarted));

        co_await outerStarted;
        // handle destroyed → outer cancelled; both children must settle
    }(coopCounter, scopeCounter, coopBlocker, outerStarted));

    EXPECT_EQ(coopCounter->load(),  0); // co_awaited child was cancelled
    EXPECT_EQ(scopeCounter->load(), 1); // fire-and-forget child ran via scope drain
}

// Test 4: A task whose JoinHandle is dropped (cancelled) before the executor
// ever polls it must not execute its body and must not hang.
TEST(CoroutineScopeIntegration, TaskCancelledBeforeFirstPoll) {
    Runtime rt(1);
    auto counter = std::make_shared<std::atomic<int>>(0);

    rt.block_on([](std::shared_ptr<std::atomic<int>> counter) -> Coro<void> {
        {
            // Single-threaded executor won't poll the child until parent yields,
            // so cancelled=true is set before the first poll.
            (void)spawn([](std::shared_ptr<std::atomic<int>> counter) -> Coro<void> {
                counter->fetch_add(1);
                co_return;
            }(counter)); // handle destroyed immediately → cancelled
        }
        co_return;
    }(counter));

    EXPECT_EQ(counter->load(), 0);
}

// Test 5: A child spawned with cancelOnDestroy=false inside a cancelled parent
// must run to completion via scope drain, not be cancelled with the parent.
TEST(CoroutineScopeIntegration, NoCancelChildWithCancelOnDestroyFalse) {
    Runtime rt(1);
    auto counter = std::make_shared<std::atomic<int>>(0);
    EventFuture parentBlocker; // never set
    EventFuture parentStarted;

    rt.block_on([](std::shared_ptr<std::atomic<int>> counter,
                   EventFuture parentBlocker,
                   EventFuture parentStarted) -> Coro<void> {
        auto handle = spawn([](std::shared_ptr<std::atomic<int>> counter,
                               EventFuture parentBlocker,
                               EventFuture parentStarted) -> Coro<void> {
            (void)spawn([](std::shared_ptr<std::atomic<int>> counter) -> Coro<void> {
                counter->fetch_add(1);
                co_return;
            }(counter)).cancelOnDestroy(false);

            parentStarted.set();
            co_await parentBlocker; // blocks here (EventFuture not Cancellable → eager destroy)
        }(counter, parentBlocker, parentStarted));

        co_await parentStarted;
        // handle destroyed → parent cancelled; scope-child must still run
    }(counter, parentBlocker, parentStarted));

    EXPECT_EQ(counter->load(), 1);
}

// Test 6: detach() must prevent the task from being registered in the enclosing
// scope.  A detached task blocking on a never-set EventFuture would deadlock
// the scope drain if incorrectly registered.  Reaching the SUCCEED() proves
// the scope drained without waiting for the detached task.
TEST(CoroutineScopeIntegration, DetachedTaskNotRegisteredInScope) {
    Runtime rt(1);
    EventFuture neverSet; // never set — would cause infinite scope drain if registered

    rt.block_on([](EventFuture neverSet) -> Coro<void> {
        (void)spawn([](EventFuture neverSet) -> Coro<void> {
            co_await neverSet; // would block forever
        }(neverSet)).detach();
        co_return;
    }(neverSet));

    SUCCEED();
}

// Test 7: When the outer coroutine is cancelled while co_awaiting an inner coroutine,
// the inner coroutine is cancelled cooperatively (it is Cancellable), which causes it
// to drain its own scope children before returning PollDropped. The outer receives
// PollDropped only after that drain is complete. This verifies the design invariant
// that makes transfer_to unnecessary: a co_awaited inner Coro is always fully drained
// via the cancel protocol before the outer frame is torn down.
TEST(CoroutineScopeIntegration, CancelledOuterDrainsInnerCoroScopeChildren) {
    Runtime rt(1);
    auto counter = std::make_shared<std::atomic<int>>(0);
    EventFuture started;
    EventFuture blocker; // never set

    rt.block_on([](std::shared_ptr<std::atomic<int>> counter,
                   EventFuture started, EventFuture blocker) -> Coro<void> {
        auto handle = spawn([](std::shared_ptr<std::atomic<int>> counter,
                               EventFuture started,
                               EventFuture blocker) -> Coro<void> {
            started.set();
            co_await inner_with_scope_child(counter, blocker);
        }(counter, started, blocker));
        co_await started;
        // handle destroyed → outer task cancelled → inner_with_scope_child is
        // Cancellable, so it must be drained (including its cancelOnDestroy=false
        // scope child) before PollDropped propagates up.
    }(counter, started, blocker));

    EXPECT_EQ(counter->load(), 1);
}
