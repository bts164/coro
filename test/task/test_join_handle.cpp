#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <coro/task/join_handle.h>
#include <coro/coro.h>
#include <coro/future.h>
#include <coro/runtime/runtime.h>
#include "executor_traits.h"
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>

using namespace coro;
using namespace coro::detail;

namespace {

class MockWaker : public detail::Waker {
public:
    MOCK_METHOD(void, wake, (), (override));
    MOCK_METHOD(Rc<detail::Waker>, clone, (), (override));
};

Rc<detail::TaskState<int>> make_int_state() {
    return make_rc<detail::TaskState<int>>();
}

Rc<detail::TaskState<void>> make_void_state() {
    return make_rc<detail::TaskState<void>>();
}

// --- Concept checks (compile-time) ---

static_assert(Future<JoinHandle<int>>);
static_assert(Future<JoinHandle<void>>);
static_assert(Future<JoinHandle<std::string>>);

static_assert(std::is_same_v<CancelAndJoin<JoinHandle<int>>::OutputType, std::optional<int>>);
static_assert(std::is_same_v<CancelAndJoin<JoinHandle<void>>::OutputType, bool>);
static_assert(Cancellable<CancelAndJoin<JoinHandle<int>>>);

}  // namespace

// --- Construction and move ---

TEST(JoinHandleTest, IntIsConstructible) {
    JoinHandle<int> h(make_int_state());
    (void)h;
}

TEST(JoinHandleTest, VoidIsConstructible) {
    JoinHandle<void> h(make_void_state());
    (void)h;
}

TEST(JoinHandleTest, IntIsMovable) {
    JoinHandle<int> h1(make_int_state());
    JoinHandle<int> h2(std::move(h1));
    (void)h2;
}

TEST(JoinHandleTest, VoidIsMovable) {
    JoinHandle<void> h1(make_void_state());
    JoinHandle<void> h2(std::move(h1));
    (void)h2;
}

// --- Destructor cancels, detach does not ---

TEST(JoinHandleTest, DestructorSetsCancelledFlag) {
    auto state = make_int_state();
    { JoinHandle<int> h(state); }
    EXPECT_TRUE(state->cancelled.load());
}

TEST(JoinHandleTest, VoidDestructorSetsCancelledFlag) {
    auto state = make_void_state();
    { JoinHandle<void> h(state); }
    EXPECT_TRUE(state->cancelled.load());
}

// Move-assigning into a live JoinHandle must cancel the OLD task (running the
// destructor's cancel()-and-register-with-scope protocol on it), not just
// silently drop the old state and let it run unbounded. Regression test for
// the same defaulted-move-assignment bug found in MpscSender/OneshotSender/
// WatchSender/WatchReceiver/MpscReceiver.
TEST(JoinHandleTest, MoveAssignmentCancelsDisplacedTask) {
    auto state_a = make_int_state();
    auto state_b = make_int_state();
    JoinHandle<int> ha(state_a);
    JoinHandle<int> hb(state_b);
    ha = std::move(hb);
    EXPECT_TRUE(state_a->cancelled.load());
    EXPECT_FALSE(state_b->cancelled.load());
}

TEST(JoinHandleTest, DetachDoesNotCancel) {
    auto state = make_int_state();
    JoinHandle<int> h(state);
    std::move(h).detach();
    EXPECT_FALSE(state->cancelled.load());
}

TEST(JoinHandleTest, VoidDetachDoesNotCancel) {
    auto state = make_void_state();
    JoinHandle<void> h(state);
    std::move(h).detach();
    EXPECT_FALSE(state->cancelled.load());
}

// --- poll: Pending when no result yet, waker stored ---

TEST(JoinHandleTest, PollReturnsPendingWhenNoResult) {
    auto waker = make_rc<MockWaker>();
    detail::Context ctx(waker);
    JoinHandle<int> h(make_int_state());
    EXPECT_TRUE(h.poll(ctx).isPending());
}

TEST(JoinHandleTest, PollStoresWakerInState) {
    auto waker = make_rc<MockWaker>();
    detail::Context ctx(waker);
    auto state = make_int_state();
    JoinHandle<int> h(state);
    h.poll(ctx);
    EXPECT_EQ(state->waker.lock(), waker);
}

TEST(JoinHandleTest, VoidPollReturnsPendingWhenNoResult) {
    auto waker = make_rc<MockWaker>();
    detail::Context ctx(waker);
    JoinHandle<void> h(make_void_state());
    EXPECT_TRUE(h.poll(ctx).isPending());
}

// --- poll: Ready when result has been set ---

TEST(JoinHandleTest, PollReturnsReadyAfterSetResult) {
    auto waker = make_rc<MockWaker>();
    detail::Context ctx(waker);
    auto state = make_int_state();
    state->setResult(42);
    JoinHandle<int> h(state);
    auto r = h.poll(ctx);
    EXPECT_TRUE(r.isReady());
    EXPECT_EQ(r.value(), 42);
}

TEST(JoinHandleTest, VoidPollReturnsReadyAfterSetDone) {
    auto waker = make_rc<MockWaker>();
    detail::Context ctx(waker);
    auto state = make_void_state();
    state->setResult();
    JoinHandle<void> h(state);
    EXPECT_TRUE(h.poll(ctx).isReady());
}

// --- poll: Error when exception has been set ---

TEST(JoinHandleTest, PollReturnsErrorAfterSetException) {
    auto waker = make_rc<MockWaker>();
    detail::Context ctx(waker);
    auto state = make_int_state();
    state->setException(std::make_exception_ptr(std::runtime_error("oops")));
    JoinHandle<int> h(state);
    auto r = h.poll(ctx);
    EXPECT_TRUE(r.isError());
    EXPECT_THROW(r.rethrowIfError(), std::runtime_error);
}

TEST(JoinHandleTest, VoidPollReturnsErrorAfterSetException) {
    auto waker = make_rc<MockWaker>();
    detail::Context ctx(waker);
    auto state = make_void_state();
    state->setException(std::make_exception_ptr(std::runtime_error("oops")));
    JoinHandle<void> h(state);
    auto r = h.poll(ctx);
    EXPECT_TRUE(r.isError());
    EXPECT_THROW(r.rethrowIfError(), std::runtime_error);
}

// --- setResult wakes the stored waker ---

TEST(JoinHandleTest, SetResultCallsWaker) {
    auto waker = make_rc<MockWaker>();
    EXPECT_CALL(*waker, wake()).Times(1);
    detail::Context ctx(waker);
    auto state = make_int_state();
    JoinHandle<int> h(state);
    h.poll(ctx);              // stores waker
    state->setResult(1);      // should call wake()
}

TEST(JoinHandleTest, SetExceptionCallsWaker) {
    auto waker = make_rc<MockWaker>();
    EXPECT_CALL(*waker, wake()).Times(1);
    detail::Context ctx(waker);
    auto state = make_int_state();
    JoinHandle<int> h(state);
    h.poll(ctx);
    state->setException(std::make_exception_ptr(std::runtime_error("x")));
}

// --- cancel_and_join: cancels, then reports how the task ended ---

TEST(JoinHandleTest, CancelAndJoinCancelsTheTask) {
    auto state = make_int_state();
    auto f = JoinHandle<int>(state).cancel_and_join();
    EXPECT_TRUE(state->cancelled.load());
}

TEST(JoinHandleTest, CancelAndJoinPendingUntilTaskEnds) {
    auto waker = make_rc<MockWaker>();
    EXPECT_CALL(*waker, wake()).Times(1);
    detail::Context ctx(waker);
    auto state = make_int_state();
    auto f = JoinHandle<int>(state).cancel_and_join();
    EXPECT_TRUE(f.poll(ctx).isPending());
    state->mark_done();
    auto r = f.poll(ctx);
    ASSERT_TRUE(r.isReady());
    EXPECT_EQ(r.value(), std::nullopt);
}

// The cancel stopped the task before it produced a value.
TEST(JoinHandleTest, CancelAndJoinYieldsNulloptWhenCancelled) {
    auto waker = make_rc<MockWaker>();
    detail::Context ctx(waker);
    auto state = make_int_state();
    state->mark_done();
    auto f = JoinHandle<int>(state).cancel_and_join();
    auto r = f.poll(ctx);
    ASSERT_TRUE(r.isReady());
    EXPECT_EQ(r.value(), std::nullopt);
}

// The task finished before the cancel reached it.
TEST(JoinHandleTest, CancelAndJoinYieldsValueWhenFinishedFirst) {
    auto waker = make_rc<MockWaker>();
    detail::Context ctx(waker);
    auto state = make_int_state();
    state->setResult(42);
    auto f = JoinHandle<int>(state).cancel_and_join();
    auto r = f.poll(ctx);
    ASSERT_TRUE(r.isReady());
    EXPECT_EQ(r.value(), std::optional<int>(42));
}

TEST(JoinHandleTest, CancelAndJoinPropagatesError) {
    auto waker = make_rc<MockWaker>();
    detail::Context ctx(waker);
    auto state = make_int_state();
    state->setException(std::make_exception_ptr(std::runtime_error("oops")));
    auto f = JoinHandle<int>(state).cancel_and_join();
    auto r = f.poll(ctx);
    EXPECT_TRUE(r.isError());
    EXPECT_THROW(r.rethrowIfError(), std::runtime_error);
}

TEST(JoinHandleTest, VoidCancelAndJoinYieldsFalseWhenCancelled) {
    auto waker = make_rc<MockWaker>();
    detail::Context ctx(waker);
    auto state = make_void_state();
    state->mark_done();
    auto f = JoinHandle<void>(state).cancel_and_join();
    auto r = f.poll(ctx);
    ASSERT_TRUE(r.isReady());
    EXPECT_FALSE(r.value());
}

TEST(JoinHandleTest, VoidCancelAndJoinYieldsTrueWhenFinishedFirst) {
    auto waker = make_rc<MockWaker>();
    detail::Context ctx(waker);
    auto state = make_void_state();
    state->setResult();
    auto f = JoinHandle<void>(state).cancel_and_join();
    auto r = f.poll(ctx);
    ASSERT_TRUE(r.isReady());
    EXPECT_TRUE(r.value());
}

// Awaited from a coroutine: before CancelAndJoin, a cancelled JoinHandle<int> made the
// await throw while reading a value that was never produced.
TEST(JoinHandleTest, CancelAndJoinOfParkedTaskYieldsNullopt) {
    SingleThreadRuntime rt;
    std::optional<int> result = rt.block_on([]() -> Coro<std::optional<int>> {
        JoinHandle<int> h = spawn(never<int>());
        co_return co_await std::move(h).cancel_and_join();
    }());
    EXPECT_EQ(result, std::nullopt);
}
