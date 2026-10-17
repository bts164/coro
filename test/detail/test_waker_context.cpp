#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <coro/detail/context.h>

using namespace coro;
using namespace coro::detail;

namespace {

class MockWaker : public detail::Waker {
public:
    MOCK_METHOD(void, wake, (), (override));
    MOCK_METHOD(Rc<Waker>, clone, (), (override));
};

class MockWeakCloneWaker : public detail::Waker {
public:
    MOCK_METHOD(void, wake, (), (override));
    MOCK_METHOD(Rc<Waker>, clone, (), (override));
    MOCK_METHOD(Weak<Waker>, weak_clone, (), (override));
};

}  // namespace

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

TEST(ContextTest, OwningGetWeakWakerDoesNotClone) {
    auto waker = make_rc<MockWeakCloneWaker>();
    EXPECT_CALL(*waker, clone()).Times(0);
    EXPECT_CALL(*waker, weak_clone()).Times(0);
    detail::Context ctx(waker);
    EXPECT_EQ(ctx.get_weak_waker().lock(), waker);
}

TEST(ContextTest, NonOwningClonesOnlyWhenAsked) {
    auto waker = make_rc<MockWaker>();
    EXPECT_CALL(*waker, clone()).Times(0);
    EXPECT_CALL(*waker, wake()).Times(1);
    detail::Context ctx(*waker);
    ctx.yield_to_executor();
    EXPECT_TRUE(ctx.yielded());
}

TEST(ContextTest, NonOwningGetWakerCallsClone) {
    auto waker = make_rc<MockWaker>();
    EXPECT_CALL(*waker, clone()).WillOnce(::testing::Return(waker));
    detail::Context ctx(*waker);
    EXPECT_EQ(ctx.getWaker(), waker);
}

TEST(ContextTest, NonOwningGetWeakWakerCallsWeakClone) {
    auto waker = make_rc<MockWeakCloneWaker>();
    EXPECT_CALL(*waker, clone()).Times(0);
    EXPECT_CALL(*waker, weak_clone()).WillOnce(::testing::Return(Weak<Waker>(waker)));
    detail::Context ctx(*waker);
    EXPECT_EQ(ctx.get_weak_waker().lock(), waker);
}

TEST(ContextTest, DefaultWeakCloneGoesThroughClone) {
    auto waker = make_rc<MockWaker>();
    EXPECT_CALL(*waker, clone()).WillOnce(::testing::Return(waker));
    detail::Context ctx(*waker);
    EXPECT_EQ(ctx.get_weak_waker().lock(), waker);
}

TEST(ContextTest, NonOwningKeepsBudget) {
    auto waker = make_rc<MockWaker>();
    detail::Context ctx(*waker, 1);
    EXPECT_TRUE(ctx.try_consume_budget());
    EXPECT_FALSE(ctx.try_consume_budget());
    EXPECT_EQ(ctx.remaining_budget(), 0u);
}
