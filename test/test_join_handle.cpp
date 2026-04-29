#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <coro/task/join_handle.h>
#include <coro/future.h>
#include <stdexcept>

using namespace coro;

class MockWaker : public detail::Waker {
public:
    MOCK_METHOD(void, wake, (), (override));
    MOCK_METHOD(std::shared_ptr<detail::Waker>, clone, (), (override));
};

static std::shared_ptr<detail::TaskState<int>> make_int_state() {
    return std::make_shared<detail::TaskState<int>>();
}

static std::shared_ptr<detail::TaskState<void>> make_void_state() {
    return std::make_shared<detail::TaskState<void>>();
}

// --- Concept checks (compile-time) ---

static_assert(Future<JoinHandle<int>>);
static_assert(Future<JoinHandle<void>>);
static_assert(Future<JoinHandle<std::string>>);

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
    auto waker = std::make_shared<MockWaker>();
    detail::Context ctx(waker);
    JoinHandle<int> h(make_int_state());
    EXPECT_TRUE(h.poll(ctx).isPending());
}

TEST(JoinHandleTest, PollStoresWakerInState) {
    auto waker = std::make_shared<MockWaker>();
    detail::Context ctx(waker);
    auto state = make_int_state();
    JoinHandle<int> h(state);
    h.poll(ctx);
    EXPECT_EQ(state->join_waker.lock(), waker);
}

TEST(JoinHandleTest, VoidPollReturnsPendingWhenNoResult) {
    auto waker = std::make_shared<MockWaker>();
    detail::Context ctx(waker);
    JoinHandle<void> h(make_void_state());
    EXPECT_TRUE(h.poll(ctx).isPending());
}

// --- poll: Ready when result has been set ---

TEST(JoinHandleTest, PollReturnsReadyAfterSetResult) {
    auto waker = std::make_shared<MockWaker>();
    detail::Context ctx(waker);
    auto state = make_int_state();
    state->setResult(42);
    JoinHandle<int> h(state);
    auto r = h.poll(ctx);
    EXPECT_TRUE(r.isReady());
    EXPECT_EQ(r.value(), 42);
}

TEST(JoinHandleTest, VoidPollReturnsReadyAfterSetDone) {
    auto waker = std::make_shared<MockWaker>();
    detail::Context ctx(waker);
    auto state = make_void_state();
    state->setResult();
    JoinHandle<void> h(state);
    EXPECT_TRUE(h.poll(ctx).isReady());
}

// --- poll: Error when exception has been set ---

TEST(JoinHandleTest, PollReturnsErrorAfterSetException) {
    auto waker = std::make_shared<MockWaker>();
    detail::Context ctx(waker);
    auto state = make_int_state();
    state->setException(std::make_exception_ptr(std::runtime_error("oops")));
    JoinHandle<int> h(state);
    auto r = h.poll(ctx);
    EXPECT_TRUE(r.isError());
    EXPECT_THROW(r.rethrowIfError(), std::runtime_error);
}

TEST(JoinHandleTest, VoidPollReturnsErrorAfterSetException) {
    auto waker = std::make_shared<MockWaker>();
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
    auto waker = std::make_shared<MockWaker>();
    EXPECT_CALL(*waker, wake()).Times(1);
    detail::Context ctx(waker);
    auto state = make_int_state();
    JoinHandle<int> h(state);
    h.poll(ctx);              // stores waker
    state->setResult(1);      // should call wake()
}

TEST(JoinHandleTest, SetExceptionCallsWaker) {
    auto waker = std::make_shared<MockWaker>();
    EXPECT_CALL(*waker, wake()).Times(1);
    detail::Context ctx(waker);
    auto state = make_int_state();
    JoinHandle<int> h(state);
    h.poll(ctx);
    state->setException(std::make_exception_ptr(std::runtime_error("x")));
}
