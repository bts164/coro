#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <coro/context.h>

using namespace coro;

class MockWaker : public Waker {
public:
    MOCK_METHOD(void, wake, (), (override));
    MOCK_METHOD(std::shared_ptr<Waker>, clone, (), (override));
};

TEST(ContextTest, GetWakerReturnsSamePointer) {
    auto waker = std::make_shared<MockWaker>();
    Context ctx(waker);
    EXPECT_EQ(ctx.getWaker(), waker);
}

TEST(ContextTest, AcceptsNullWaker) {
    Context ctx(nullptr);
    EXPECT_EQ(ctx.getWaker(), nullptr);
}

TEST(ContextTest, WakerWakeIsCalled) {
    auto waker = std::make_shared<MockWaker>();
    EXPECT_CALL(*waker, wake()).Times(1);
    Context ctx(waker);
    ctx.getWaker()->wake();
}

TEST(ContextTest, WakerCloneIsCalled) {
    auto waker  = std::make_shared<MockWaker>();
    auto clone  = std::make_shared<MockWaker>();
    EXPECT_CALL(*waker, clone()).WillOnce(::testing::Return(clone));
    Context ctx(waker);
    auto result = ctx.getWaker()->clone();
    EXPECT_EQ(result, clone);
}

TEST(ContextTest, GetCancellationTokenDefaultsToNull) {
    auto waker = std::make_shared<MockWaker>();
    Context ctx(waker);
    EXPECT_EQ(ctx.getCancellationToken(), nullptr);
}

TEST(ContextTest, GetCancellationTokenReturnsSamePointer) {
    auto waker = std::make_shared<MockWaker>();
    auto token = std::make_shared<CancellationToken>();
    Context ctx(waker, token);
    EXPECT_EQ(ctx.getCancellationToken(), token);
}

TEST(ContextTest, SubclassCanAddFields) {
    struct DerivedContext : Context {
        explicit DerivedContext(std::shared_ptr<Waker> w, int extra)
            : Context(std::move(w)), extra(extra) {}
        int extra;
    };

    auto waker = std::make_shared<MockWaker>();
    DerivedContext derived(waker, 42);
    Context& base = derived;
    EXPECT_EQ(base.getWaker(), waker);
    EXPECT_EQ(derived.extra, 42);
}
