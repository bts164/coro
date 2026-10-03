#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <coro/future.h>

#include <chrono>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

using namespace coro;
using namespace coro::detail;

class MockWaker : public detail::Waker {
public:
    MOCK_METHOD(void, wake, (), (override));
    MOCK_METHOD(Rc<Waker>, clone, (), (override));
};

// Stub: always returns Pending and stores the waker for inspection.
template<typename T>
class WakerStoringFuture {
public:
    using OutputType = T;
    PollResult<T> poll(detail::Context& ctx) {
        m_waker = ctx.getWaker();
        return PollPending;
    }
    Rc<detail::Waker> storedWaker() const { return m_waker; }
private:
    Rc<detail::Waker> m_waker;
};

// Stub: should return Ready(value) — body left as PollPending until Phase 3.
template<typename T>
class ImmediateFuture {
public:
    using OutputType = T;
    explicit ImmediateFuture(T value) : m_value(std::move(value)) {}
    PollResult<T> poll(detail::Context&) {
        return std::move(m_value);
    }
private:
    T m_value;
};

// --- Concept checks (compile-time) ---

static_assert(Future<WakerStoringFuture<int>>);
static_assert(Future<WakerStoringFuture<std::string>>);
static_assert(Future<ImmediateFuture<int>>);

struct NoOutputType {
    PollResult<int> poll(detail::Context&) { return PollPending; }
};
static_assert(!Future<NoOutputType>);

struct WrongReturnType {
    using OutputType = int;
    int poll(detail::Context&) { return 0; }
};
static_assert(!Future<WrongReturnType>);

// --- Runtime tests ---

TEST(WakerStoringFutureTest, PollReturnsPending) {
    auto waker = make_rc<MockWaker>();
    detail::Context ctx(waker);
    WakerStoringFuture<int> f;
    EXPECT_TRUE(f.poll(ctx).isPending());
}

TEST(WakerStoringFutureTest, StoresWakerAfterPoll) {
    auto waker = make_rc<MockWaker>();
    detail::Context ctx(waker);
    WakerStoringFuture<int> f;
    f.poll(ctx);
    EXPECT_EQ(f.storedWaker(), waker);
}

// Disabled until Phase 3 implements ImmediateFuture::poll.
TEST(ImmediateFutureTest, PollReturnsReady) {
    auto waker = make_rc<MockWaker>();
    detail::Context ctx(waker);
    ImmediateFuture<int> f(42);
    auto result = f.poll(ctx);
    EXPECT_TRUE(result.isReady());
    EXPECT_EQ(result.value(), 42);
}

// --- coro::never<T>() / coro::NeverFuture<T> ---
//
// Qualified as coro::NeverFuture for clarity next to this file's WakerStoringFuture stub.

static_assert(Future<coro::NeverFuture<int>>);
static_assert(Future<coro::NeverFuture<void>>);
static_assert(!Cancellable<coro::NeverFuture<int>>);

TEST(CoroNeverFutureTest, PollAlwaysReturnsPending) {
    auto waker = make_rc<MockWaker>();
    detail::Context ctx(waker);
    coro::NeverFuture<int> f;
    EXPECT_TRUE(f.poll(ctx).isPending());
    EXPECT_TRUE(f.poll(ctx).isPending());
}

TEST(CoroNeverFutureTest, NeverFactoryReturnsPending) {
    auto waker = make_rc<MockWaker>();
    detail::Context ctx(waker);
    auto f = coro::never<int>();
    EXPECT_TRUE(f.poll(ctx).isPending());
}

TEST(CoroNeverFutureTest, VoidOutputTypeCompiles) {
    auto waker = make_rc<MockWaker>();
    detail::Context ctx(waker);
    auto f = coro::never<void>();
    EXPECT_TRUE(f.poll(ctx).isPending());
}

// --- blocking_wait ---

namespace {

struct ImmediateVoidFuture {
    using OutputType = void;
    PollResult<void> poll(detail::Context&) { return PollReady; }
};

struct ErrorFuture {
    using OutputType = int;
    PollResult<int> poll(detail::Context&) {
        return PollError(std::make_exception_ptr(std::runtime_error("boom")));
    }
};

// Pends on the first poll, handing the caller's waker out through `promise` so a
// test thread can wake it later; ready on the second poll.
class DelayedFuture {
public:
    using OutputType = int;
    DelayedFuture(std::shared_ptr<std::promise<Rc<detail::Waker>>> promise, int value)
        : m_promise(std::move(promise)), m_value(value) {}
    PollResult<int> poll(detail::Context& ctx) {
        if (!m_polled) {
            m_polled = true;
            m_promise->set_value(ctx.getWaker());
            return PollPending;
        }
        return m_value;
    }
private:
    std::shared_ptr<std::promise<Rc<detail::Waker>>> m_promise;
    int                                               m_value;
    bool                                              m_polled = false;
};

} // namespace

TEST(BlockingWaitTest, ReturnsReadyValueImmediately) {
    EXPECT_EQ(blocking_wait(ImmediateFuture<int>(42)), 42);
}

TEST(BlockingWaitTest, HandlesVoidOutput) {
    blocking_wait(ImmediateVoidFuture{});
    SUCCEED();
}

TEST(BlockingWaitTest, RethrowsException) {
    EXPECT_THROW(blocking_wait(ErrorFuture{}), std::runtime_error);
}

TEST(BlockingWaitTest, BlocksUntilWokenFromAnotherThread) {
    auto promise = std::make_shared<std::promise<Rc<detail::Waker>>>();
    auto waker_future = promise->get_future();

    std::thread waker_thread([waker_future = std::move(waker_future)]() mutable {
        auto waker = waker_future.get();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        waker->wake();
    });

    auto result = blocking_wait(DelayedFuture(std::move(promise), 99));
    EXPECT_EQ(result, 99);
    waker_thread.join();
}
