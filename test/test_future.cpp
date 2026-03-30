#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <coro/future.h>

#include <string>

using namespace coro;

class MockWaker : public detail::Waker {
public:
    MOCK_METHOD(void, wake, (), (override));
    MOCK_METHOD(std::shared_ptr<Waker>, clone, (), (override));
};

// Stub: always returns Pending and stores the waker for inspection.
template<typename T>
class NeverFuture {
public:
    using OutputType = T;
    PollResult<T> poll(detail::Context& ctx) {
        m_waker = ctx.getWaker();
        return PollPending;
    }
    std::shared_ptr<detail::Waker> storedWaker() const { return m_waker; }
private:
    std::shared_ptr<detail::Waker> m_waker;
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

static_assert(Future<NeverFuture<int>>);
static_assert(Future<NeverFuture<std::string>>);
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

TEST(NeverFutureTest, PollReturnsPending) {
    auto waker = std::make_shared<MockWaker>();
    detail::Context ctx(waker);
    NeverFuture<int> f;
    EXPECT_TRUE(f.poll(ctx).isPending());
}

TEST(NeverFutureTest, StoresWakerAfterPoll) {
    auto waker = std::make_shared<MockWaker>();
    detail::Context ctx(waker);
    NeverFuture<int> f;
    f.poll(ctx);
    EXPECT_EQ(f.storedWaker(), waker);
}

// Disabled until Phase 3 implements ImmediateFuture::poll.
TEST(ImmediateFutureTest, PollReturnsReady) {
    auto waker = std::make_shared<MockWaker>();
    detail::Context ctx(waker);
    ImmediateFuture<int> f(42);
    auto result = f.poll(ctx);
    EXPECT_TRUE(result.isReady());
    EXPECT_EQ(result.value(), 42);
}
