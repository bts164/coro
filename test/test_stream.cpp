#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <coro/stream.h>

#include <vector>

using namespace coro;

class MockWaker : public detail::Waker {
public:
    MOCK_METHOD(void, wake, (), (override));
    MOCK_METHOD(std::shared_ptr<detail::Waker>, clone, (), (override));
};

// Stub: backed by a vector; should yield items then nullopt — body stubbed until Phase 3.
template<typename T>
class FiniteStream {
public:
    using ItemType = T;
    explicit FiniteStream(std::vector<T> items) : m_items(std::move(items)) {}
    PollResult<std::optional<T>> poll_next(detail::Context&) {
        if (m_index < m_items.size())
            return std::optional<T>(std::move(m_items[m_index++]));
        return std::optional<T>(std::nullopt);
    }
private:
    std::vector<T> m_items;
    std::size_t m_index = 0;
};

// Void stream: yields N completions then false (exhausted).
class VoidStream {
public:
    using ItemType = void;
    explicit VoidStream(int count) : m_remaining(count) {}
    PollResult<bool> poll_next(detail::Context&) {
        if (m_remaining > 0) { --m_remaining; return true; }
        return false;
    }
private:
    int m_remaining;
};

// Stub: should yield one item then an error — body stubbed until Phase 3.
template<typename T>
class ErrorStream {
public:
    using ItemType = T;
    explicit ErrorStream(T first, std::exception_ptr err)
        : m_first(std::move(first)), m_error(std::move(err)) {}
    PollResult<std::optional<T>> poll_next(detail::Context&) {
        if (!m_sent) {
            m_sent = true;
            return std::optional<T>(std::move(m_first));
        }
        return PollError(std::exchange(m_error, nullptr));
    }
private:
    T m_first;
    std::exception_ptr m_error;
    bool m_sent = false;
};

// --- Concept checks (compile-time) ---

static_assert(Stream<FiniteStream<int>>);
static_assert(Stream<ErrorStream<double>>);
static_assert(Stream<VoidStream>);
static_assert(Future<NextFuture<FiniteStream<int>>>);
static_assert(Future<NextFuture<VoidStream>>);

struct NoItemType {
    PollResult<std::optional<int>> poll_next(detail::Context&) { return PollPending; }
};
static_assert(!Stream<NoItemType>);

struct WrongReturnType {
    using ItemType = int;
    int poll_next(detail::Context&) { return 0; }
};
static_assert(!Stream<WrongReturnType>);

// --- Runtime tests ---

TEST(FiniteStreamTest, PollReturnsPendingStub) {
    auto waker = std::make_shared<MockWaker>();
    detail::Context ctx(waker);
    FiniteStream<int> s({1, 2, 3});
    auto r1 = s.poll_next(ctx);
    EXPECT_TRUE(r1.isReady());
    EXPECT_TRUE(r1.value().has_value());
    EXPECT_EQ(r1.value().value(), 1);

    auto r2 = s.poll_next(ctx);
    EXPECT_TRUE(r2.isReady());
    EXPECT_TRUE(r2.value().has_value());
    EXPECT_EQ(r2.value().value(), 2);

    auto r3 = s.poll_next(ctx);
    EXPECT_TRUE(r3.isReady());
    EXPECT_TRUE(r3.value().has_value());
    EXPECT_EQ(r3.value().value(), 3);

    auto r4 = s.poll_next(ctx);
    EXPECT_TRUE(r4.isReady());
    EXPECT_FALSE(r4.value().has_value());
}

TEST(NextFutureTest, SatisfiesFutureConcept) {
    // Verified by static_assert above; documents the intent at runtime too.
    FiniteStream<int> s({1});
    auto n = next(s);
    static_assert(Future<decltype(n)>);
}

TEST(NextFutureTest, DelegatesPollNext) {
    auto waker = std::make_shared<MockWaker>();
    detail::Context ctx(waker);
    FiniteStream<int> s({});
    auto n = next(s);
    // Both should return Pending since the stub always returns Pending.
    auto result = n.poll(ctx);
    EXPECT_TRUE(result.isReady());
    EXPECT_FALSE(result.value().has_value());
}

// Disabled until Phase 3 implements FiniteStream::poll_next.
TEST(FiniteStreamTest, YieldsAllItems) {
    auto waker = std::make_shared<MockWaker>();
    detail::Context ctx(waker);
    FiniteStream<int> s({1, 2, 3});
    std::vector<int> results;
    while (true) {
        auto r = s.poll_next(ctx);
        ASSERT_TRUE(r.isReady());
        if (!r.value()) break;
        results.push_back(*r.value());
    }
    EXPECT_EQ(results, (std::vector<int>{1, 2, 3}));
}

TEST(VoidStreamTest, YieldsCountThenFalse) {
    auto waker = std::make_shared<MockWaker>();
    detail::Context ctx(waker);
    VoidStream s(3);

    for (int i = 0; i < 3; ++i) {
        auto r = s.poll_next(ctx);
        ASSERT_TRUE(r.isReady());
        EXPECT_TRUE(r.value());
    }
    auto last = s.poll_next(ctx);
    ASSERT_TRUE(last.isReady());
    EXPECT_FALSE(last.value());
}

TEST(VoidStreamTest, NextFutureOutputTypeIsBool) {
    VoidStream s(0);
    auto n = next(s);
    static_assert(std::same_as<decltype(n)::OutputType, bool>);
}

TEST(VoidStreamTest, EmptyYieldsFalseImmediately) {
    auto waker = std::make_shared<MockWaker>();
    detail::Context ctx(waker);
    VoidStream s(0);
    auto r = next(s).poll(ctx);
    ASSERT_TRUE(r.isReady());
    EXPECT_FALSE(r.value());
}

TEST(ErrorStreamTest, PropagatesError) {
    auto waker = std::make_shared<MockWaker>();
    detail::Context ctx(waker);
    auto eptr = std::make_exception_ptr(std::runtime_error("stream error"));
    ErrorStream<int> s(42, eptr);

    auto first = s.poll_next(ctx);
    ASSERT_TRUE(first.isReady());
    EXPECT_EQ(first.value(), 42);

    auto second = s.poll_next(ctx);
    EXPECT_TRUE(second.isError());
    EXPECT_THROW(second.rethrowIfError(), std::runtime_error);
}
