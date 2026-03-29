#include <gtest/gtest.h>
#include <coro/synchronize.h>
#include <coro/future.h>
#include <coro/runtime.h>
#include <stdexcept>

using namespace coro;

// --- Concept check (compile-time) ---

static_assert(Future<Synchronize>);

// --- Construction ---

TEST(SynchronizeTest, IsConstructibleWithLambda) {
    Synchronize s([](Synchronize&) -> Coro<void> { co_return; });
    (void)s;
}

TEST(SynchronizeTest, IsMovable) {
    Synchronize s1([](Synchronize&) -> Coro<void> { co_return; });
    Synchronize s2(std::move(s1));
    (void)s2;
}

// --- Phase 3 integration tests (require block_on) ---

// Empty body — no children, completes immediately.
TEST(SynchronizeTest, EmptyBodyCompletes) {
    Runtime rt;
    rt.block_on(Synchronize([](Synchronize&) -> Coro<void> {
        co_return;
    }));
}

// Body produces a value; Synchronize completes after body finishes.
TEST(SynchronizeTest, BodyExecutes) {
    Runtime rt;
    int value = 0;
    rt.block_on(Synchronize([&value](Synchronize&) -> Coro<void> {
        value = 42;
        co_return;
    }));
    EXPECT_EQ(value, 42);
}

// Single child spawned; Synchronize waits for it to complete.
TEST(SynchronizeTest, WaitsForSingleChild) {
    struct IntFuture {
        using OutputType = int;
        int m_value;
        PollResult<int> poll(Context&) { return m_value; }
    };

    Runtime rt;
    int result = 0;
    rt.block_on(Synchronize([&result](Synchronize& sync) -> Coro<void> {
        auto state = std::make_shared<detail::TaskState<int>>();
        // Use spawn + a local state so we can read the result after sync completes.
        // Simpler: use a shared int that the future writes to directly.
        // We use a shared_ptr so the lambda can capture it.
        auto shared = std::make_shared<int>(0);
        struct WritingFuture {
            using OutputType = int;
            std::shared_ptr<int> dest;
            int value;
            PollResult<int> poll(Context&) { *dest = value; return value; }
        };
        sync.spawn(WritingFuture{shared, 99}).submit();
        co_return;
        // suppress unused warning
        (void)result;
        // set result after sync finishes via shared pointer
        result = *shared;  // won't reach here — see below
    }));
    // Can't easily read child result here without JoinHandle.
    // This test just verifies no crash/hang.
}

// Cleaner test: child writes to a shared value; after block_on returns, value is set.
TEST(SynchronizeTest, ChildWritesToSharedValue) {
    Runtime rt;
    auto shared = std::make_shared<int>(0);

    struct WritingFuture {
        using OutputType = int;
        std::shared_ptr<int> dest;
        int value;
        PollResult<int> poll(Context&) { *dest = value; return value; }
    };

    rt.block_on(Synchronize([&shared](Synchronize& sync) -> Coro<void> {
        sync.spawn(WritingFuture{shared, 7}).submit();
        co_return;
    }));

    EXPECT_EQ(*shared, 7);
}

// Multiple children — all must complete before block_on returns.
TEST(SynchronizeTest, WaitsForAllChildren) {
    Runtime rt;
    auto counter = std::make_shared<int>(0);

    struct IncrFuture {
        using OutputType = int;
        std::shared_ptr<int> counter;
        PollResult<int> poll(Context&) { ++(*counter); return *counter; }
    };

    rt.block_on(Synchronize([&counter](Synchronize& sync) -> Coro<void> {
        sync.spawn(IncrFuture{counter}).submit();
        sync.spawn(IncrFuture{counter}).submit();
        sync.spawn(IncrFuture{counter}).submit();
        co_return;
    }));

    EXPECT_EQ(*counter, 3);
}

// Exception thrown in the body is re-thrown by block_on.
TEST(SynchronizeTest, BodyExceptionPropagates) {
    Runtime rt;
    EXPECT_THROW(
        rt.block_on(Synchronize([](Synchronize&) -> Coro<void> {
            throw std::runtime_error("body error");
            co_return;
        })),
        std::runtime_error
    );
}

// Exception thrown by a child task is re-thrown after all children complete.
TEST(SynchronizeTest, ChildExceptionPropagates) {
    Runtime rt;

    struct ThrowingFuture {
        using OutputType = int;
        PollResult<int> poll(Context&) {
            return PollError(std::make_exception_ptr(std::runtime_error("child error")));
        }
    };

    EXPECT_THROW(
        rt.block_on(Synchronize([](Synchronize& sync) -> Coro<void> {
            sync.spawn(ThrowingFuture{}).submit();
            co_return;
        })),
        std::runtime_error
    );
}

// Body can safely reference parent-owned data (key correctness guarantee of Synchronize).
TEST(SynchronizeTest, BodyCanReferenceParentData) {
    Runtime rt;
    int local = 100;

    rt.block_on(Synchronize([&local](Synchronize&) -> Coro<void> {
        local += 1;
        co_return;
    }));

    EXPECT_EQ(local, 101);
}
