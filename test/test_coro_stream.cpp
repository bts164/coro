#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <coro/coro_stream.h>
#include <coro/stream.h>
#include <stdexcept>
#include <vector>

using namespace coro;

class MockWaker : public detail::Waker {
public:
    MOCK_METHOD(void, wake, (), (override));
    MOCK_METHOD(std::shared_ptr<detail::Waker>, clone, (), (override));
};

// Helper: drain a CoroStream synchronously into a vector, asserting no errors.
template<typename T, bool HFV>
std::vector<T> drain(CoroStream<T, HFV>& s, detail::Context& ctx) {
    std::vector<T> out;
    while (true) {
        auto r = s.poll_next(ctx);
        EXPECT_FALSE(r.isError()) << "unexpected error in drain()";
        if (r.isPending()) break;
        if (!r.value()) break;
        out.push_back(std::move(*r.value()));
    }
    return out;
}

// --- Helper generators ---

CoroStream<int> empty_stream() {
    co_return;
}

CoroStream<int> range(int from, int to) {
    for (int i = from; i < to; ++i)
        co_yield i;
}

CoroStream<int, true> range_with_sum(int from, int to) {
    int sum = 0;
    for (int i = from; i < to; ++i) {
        sum += i;
        co_yield i;
    }
    co_return sum;
}

CoroStream<int> throws_on_first_yield() {
    throw std::runtime_error("stream boom");
    co_yield 0;
}

CoroStream<int> throws_after_one_yield() {
    co_yield 1;
    throw std::runtime_error("mid-stream boom");
}

// Generator that co_awaits an inner Future between yields.
CoroStream<int> awaiting_stream() {
    struct ImmediateFuture {
        using OutputType = int;
        int m_value;
        PollResult<int> poll(detail::Context&) { return m_value; }
    };
    int a = co_await ImmediateFuture{10};
    co_yield a;
    int b = co_await ImmediateFuture{20};
    co_yield b;
}

// --- Concept checks (compile-time) ---

static_assert(Stream<CoroStream<int>>);
static_assert(Stream<CoroStream<int, false>>);
static_assert(Stream<CoroStream<int, true>>);
static_assert(Stream<CoroStream<std::string>>);
static_assert(std::same_as<CoroStream<int>::ItemType, int>);
static_assert(std::same_as<CoroStream<int, true>::ItemType, int>);

// --- Construction and move tests ---

TEST(CoroStreamTest, DefaultIsConstructible) {
    auto s = range(0, 5);
    (void)s;
}

TEST(CoroStreamTest, WithFinalValueIsConstructible) {
    auto s = range_with_sum(0, 5);
    (void)s;
}

TEST(CoroStreamTest, IsMovable) {
    auto s1 = range(0, 3);
    auto s2 = std::move(s1);
    (void)s2;
}

// --- Empty stream ---

TEST(CoroStreamTest, EmptyStreamReturnsNullopt) {
    auto waker = std::make_shared<MockWaker>();
    detail::Context ctx(waker);
    auto s = empty_stream();
    auto r = s.poll_next(ctx);
    EXPECT_TRUE(r.isReady());
    EXPECT_FALSE(r.value().has_value());
}

TEST(CoroStreamTest, ExhaustedStreamKeepsReturningNullopt) {
    auto waker = std::make_shared<MockWaker>();
    detail::Context ctx(waker);
    auto s = empty_stream();
    s.poll_next(ctx);  // exhaust
    auto r = s.poll_next(ctx);
    EXPECT_TRUE(r.isReady());
    EXPECT_FALSE(r.value().has_value());
}

// --- co_yield items ---

TEST(CoroStreamTest, RangeYieldsExpectedValues) {
    auto waker = std::make_shared<MockWaker>();
    detail::Context ctx(waker);
    auto s = range(0, 4);
    auto items = drain(s, ctx);
    EXPECT_THAT(items, ::testing::ElementsAre(0, 1, 2, 3));
}

TEST(CoroStreamTest, ExhaustionAfterLastItem) {
    auto waker = std::make_shared<MockWaker>();
    detail::Context ctx(waker);
    auto s = range(0, 2);
    drain(s, ctx);
    auto r = s.poll_next(ctx);  // one more after exhaustion
    EXPECT_TRUE(r.isReady());
    EXPECT_FALSE(r.value().has_value());
}

// --- co_return value (HasFinalValue = true) ---

TEST(CoroStreamTest, FinalValueEmittedAsLastItem) {
    auto waker = std::make_shared<MockWaker>();
    detail::Context ctx(waker);
    auto s = range_with_sum(1, 4);  // yields 1, 2, 3 then co_return 6
    auto items = drain(s, ctx);
    // yields: 1, 2, 3; final value: 6 (1+2+3)
    EXPECT_THAT(items, ::testing::ElementsAre(1, 2, 3, 6));
}

TEST(CoroStreamTest, FinalValueFollowedByNullopt) {
    auto waker = std::make_shared<MockWaker>();
    detail::Context ctx(waker);
    auto s = range_with_sum(0, 1);  // co_yield 0, then co_return 0 (sum)
    s.poll_next(ctx);               // item: 0
    s.poll_next(ctx);               // final value: 0 (the sum)
    auto r = s.poll_next(ctx);      // now exhausted
    EXPECT_TRUE(r.isReady());
    EXPECT_FALSE(r.value().has_value());
}

// --- Exception propagation ---

TEST(CoroStreamTest, ExceptionBeforeYieldStoredAsError) {
    auto waker = std::make_shared<MockWaker>();
    detail::Context ctx(waker);
    auto s = throws_on_first_yield();
    auto r = s.poll_next(ctx);
    EXPECT_TRUE(r.isError());
    EXPECT_THROW(r.rethrowIfError(), std::runtime_error);
}

TEST(CoroStreamTest, ExceptionAfterYieldStoredAsError) {
    auto waker = std::make_shared<MockWaker>();
    detail::Context ctx(waker);
    auto s = throws_after_one_yield();

    auto r1 = s.poll_next(ctx);
    EXPECT_TRUE(r1.isReady());
    EXPECT_EQ(r1.value(), 1);

    auto r2 = s.poll_next(ctx);
    EXPECT_TRUE(r2.isError());
    EXPECT_THROW(r2.rethrowIfError(), std::runtime_error);
}

// --- co_await inside generator ---

TEST(CoroStreamTest, AwaitingStreamYieldsCorrectValues) {
    auto waker = std::make_shared<MockWaker>();
    detail::Context ctx(waker);
    auto s = awaiting_stream();
    auto items = drain(s, ctx);
    EXPECT_THAT(items, ::testing::ElementsAre(10, 20));
}
