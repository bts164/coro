#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <coro/stream.h>
#include <coro/coro.h>
#include <coro/co_invoke.h>
#include <coro/sync/select.h>
#include <coro/runtime/runtime.h>

#include <optional>
#include <variant>
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

// Stream that suspends once per item (stores waker, re-wakes itself, returns PollPending),
// then delivers the item on the next poll. Used to exercise waker registration and
// NextFuture drop paths.
template<typename T>
class SuspendingStream {
public:
    using ItemType = T;
    explicit SuspendingStream(std::vector<T> items) : m_items(std::move(items)) {}

    PollResult<std::optional<T>> poll_next(detail::Context& ctx) {
        if (m_index >= m_items.size())
            return std::optional<T>(std::nullopt);
        if (!m_suspended) {
            m_suspended = true;
            m_waker = ctx.getWaker()->clone();
            m_waker->wake();  // re-schedule immediately; simulates async I/O callback
            return PollPending;
        }
        m_suspended = false;
        m_waker.reset();
        return std::optional<T>(std::move(m_items[m_index++]));
    }

private:
    std::vector<T>                  m_items;
    std::size_t                     m_index     = 0;
    bool                            m_suspended = false;
    std::shared_ptr<detail::Waker>  m_waker;
};

static_assert(Stream<SuspendingStream<int>>);

// -----------------------------------------------------------------------
// NextFuture drop — low-level unit test
//
// Dropping a NextFuture after it has suspended (stored a waker and returned
// PollPending) must leave the stream in a usable state: the next NextFuture
// created from the same stream must deliver the item correctly.
// -----------------------------------------------------------------------

TEST(NextFutureTest, DroppedAfterSuspendLeavesStreamUsable) {
    // Use a concrete waker that records whether wake() was called.
    class CountingWaker : public detail::Waker {
    public:
        int count = 0;
        void wake() override { ++count; }
        std::shared_ptr<Waker> clone() override {
            return std::make_shared<CountingWaker>();
        }
    };

    SuspendingStream<int> s({42});

    // First NextFuture: poll once → PollPending (waker stored in stream).
    {
        auto waker = std::make_shared<CountingWaker>();
        detail::Context ctx(waker);
        auto n = next(s);
        auto r = n.poll(ctx);
        EXPECT_TRUE(r.isPending());
        // n destroyed here — stream still holds (now stale) waker
    }

    // Second NextFuture: stream should deliver the item on the second poll
    // (SuspendingStream advances past the suspend on re-poll).
    auto waker2 = std::make_shared<CountingWaker>();
    detail::Context ctx2(waker2);
    auto n2 = next(s);
    auto r = n2.poll(ctx2);
    ASSERT_TRUE(r.isReady());
    EXPECT_EQ(r.value(), 42);
}

// -----------------------------------------------------------------------
// NextFuture in select — integration tests
//
// These verify that a NextFuture used as a select branch works correctly
// when the branch loses (NextFuture dropped mid-suspension) and that the
// stream is still usable in subsequent rounds.
// -----------------------------------------------------------------------

// Immediately-ready void future used as the "other branch" in select.
struct SelectVoidBranch {
    using OutputType = void;
    PollResult<void> poll(detail::Context&) { return PollReady; }
};

// Never-completes future used as the "other branch" when we want next() to win.
struct SelectNeverBranch {
    using OutputType = void;
    PollResult<void> poll(detail::Context&) { return PollPending; }
};

TEST(NextFutureTest, SelectLosingBranchLeavesStreamUsable) {
    // select(next(s), ImmediateVoid) — ImmediateVoid wins; next(s) was pending.
    // After select returns, the stream must still deliver all its items.
    Runtime rt(1);
    std::vector<int> results;

    rt.block_on(co_invoke([&results]() -> Coro<void> {
        SuspendingStream<int> s({10, 20, 30});

        // next(s) suspends on first poll; ImmediateVoid wins immediately.
        auto sel = co_await select(next(s), SelectVoidBranch{});
        EXPECT_TRUE((std::holds_alternative<SelectBranch<1, void>>(sel)));

        // Stream still usable — collect remaining items.
        while (auto item = co_await next(s))
            results.push_back(*item);
    }));

    std::sort(results.begin(), results.end());
    EXPECT_EQ(results, (std::vector<int>{10, 20, 30}));
}

TEST(NextFutureTest, SelectWinningBranchDeliversItem) {
    // select(next(s), NeverFuture) — next(s) wins once the item is ready.
    Runtime rt(1);
    std::optional<int> got;

    rt.block_on(co_invoke([&got]() -> Coro<void> {
        SuspendingStream<int> s({42});
        auto sel = co_await select(next(s), SelectNeverBranch{});
        if (std::holds_alternative<SelectBranch<0, std::optional<int>>>(sel))
            got = std::get<SelectBranch<0, std::optional<int>>>(sel).value;
    }));

    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(*got, 42);
}

TEST(NextFutureTest, SelectRepeatedRoundsCollectsAllItems) {
    // Each iteration wraps next(s) in select. Verifies waker is refreshed correctly
    // each round and all items are delivered in order.
    Runtime rt(1);
    std::vector<int> results;

    rt.block_on(co_invoke([&results]() -> Coro<void> {
        SuspendingStream<int> s({1, 2, 3, 4});
        while (true) {
            auto sel = co_await select(next(s), SelectNeverBranch{});
            if (!std::holds_alternative<SelectBranch<0, std::optional<int>>>(sel))
                break;
            auto item = std::get<SelectBranch<0, std::optional<int>>>(sel).value;
            if (!item) break;
            results.push_back(*item);
        }
    }));

    EXPECT_EQ(results, (std::vector<int>{1, 2, 3, 4}));
}
