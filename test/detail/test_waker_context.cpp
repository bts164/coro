#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <coro/detail/context.h>

using namespace coro;
using namespace coro::detail;

class MockWaker : public detail::Waker {
public:
    MOCK_METHOD(void, wake, (), (override));
    MOCK_METHOD(Rc<Waker>, clone, (), (override));
};

TEST(ContextTest, GetWakerReturnsSamePointer) {
    auto waker = make_rc<MockWaker>();
    detail::Context ctx(waker);
    EXPECT_EQ(ctx.getWaker(), waker);
}

TEST(ContextTest, AcceptsNullWaker) {
    detail::Context ctx(nullptr);
    EXPECT_EQ(ctx.getWaker(), nullptr);
}

TEST(ContextTest, WakerWakeIsCalled) {
    auto waker = make_rc<MockWaker>();
    EXPECT_CALL(*waker, wake()).Times(1);
    detail::Context ctx(waker);
    ctx.getWaker()->wake();
}

TEST(ContextTest, WakerCloneIsCalled) {
    auto waker  = make_rc<MockWaker>();
    auto clone  = make_rc<MockWaker>();
    EXPECT_CALL(*waker, clone()).WillOnce(::testing::Return(clone));
    detail::Context ctx(waker);
    auto result = ctx.getWaker()->clone();
    EXPECT_EQ(result, clone);
}

TEST(ContextTest, SubclassCanAddFields) {
    struct DerivedContext : detail::Context {
        explicit DerivedContext(Rc<detail::Waker> w, int extra)
            : Context(std::move(w)), extra(extra) {}
        int extra;
    };

    auto waker = make_rc<MockWaker>();
    DerivedContext derived(waker, 42);
    detail::Context& base = derived;
    EXPECT_EQ(base.getWaker(), waker);
    EXPECT_EQ(derived.extra, 42);
}
