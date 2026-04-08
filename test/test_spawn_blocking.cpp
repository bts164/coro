#include <coro/coro.h>
#include <coro/co_invoke.h>
#include <coro/runtime/runtime.h>
#include <coro/sync/sleep.h>
#include <coro/task/join_set.h>
#include <coro/task/spawn_blocking.h>
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>

using namespace std::chrono_literals;

// ---------------------------------------------------------------------------
// Basic result retrieval
// ---------------------------------------------------------------------------

TEST(SpawnBlocking, ReturnsValue) {
    coro::Runtime rt(1);
    int result = rt.block_on([]() -> coro::Coro<int> {
        co_return co_await coro::spawn_blocking([] { return 42; });
    }());
    EXPECT_EQ(result, 42);
}

TEST(SpawnBlocking, ReturnsVoid) {
    coro::Runtime rt(1);
    std::atomic<bool> ran{false};
    rt.block_on([&]() -> coro::Coro<void> {
        co_await coro::spawn_blocking([&] { ran.store(true); });
    }());
    EXPECT_TRUE(ran.load());
}

TEST(SpawnBlocking, ReturnsString) {
    coro::Runtime rt(1);
    std::string result = rt.block_on([]() -> coro::Coro<std::string> {
        co_return co_await coro::spawn_blocking([] { return std::string("hello"); });
    }());
    EXPECT_EQ(result, "hello");
}

// ---------------------------------------------------------------------------
// T = std::exception_ptr (must not be confused with a captured exception)
// ---------------------------------------------------------------------------

TEST(SpawnBlocking, ReturnsExceptionPtr) {
    coro::Runtime rt(1);
    std::exception_ptr ep = rt.block_on([]() -> coro::Coro<std::exception_ptr> {
        co_return co_await coro::spawn_blocking([] {
            try { throw std::runtime_error("inner"); }
            catch (...) { return std::current_exception(); }
        });
    }());
    ASSERT_NE(ep, nullptr);
    EXPECT_THROW(std::rethrow_exception(ep), std::runtime_error);
}

// ---------------------------------------------------------------------------
// Exception propagation
// ---------------------------------------------------------------------------

TEST(SpawnBlocking, PropagatesException) {
    coro::Runtime rt(1);
    EXPECT_THROW(
        rt.block_on([]() -> coro::Coro<int> {
            co_return co_await coro::spawn_blocking([]() -> int {
                throw std::runtime_error("blocking error");
            });
        }()),
        std::runtime_error);
}

TEST(SpawnBlocking, PropagatesExceptionVoid) {
    coro::Runtime rt(1);
    EXPECT_THROW(
        rt.block_on([]() -> coro::Coro<void> {
            co_await coro::spawn_blocking([]() {
                throw std::runtime_error("blocking void error");
            });
        }()),
        std::runtime_error);
}

// ---------------------------------------------------------------------------
// Concurrency — executor thread is freed while blocking work runs
// ---------------------------------------------------------------------------

TEST(SpawnBlocking, ExecutorThreadIsFreeDuringBlockingWork) {
    coro::Runtime rt(1);

    // Spawn two tasks. The blocking work in the first sleeps briefly.
    // If the executor thread were blocked, the second task could never run.
    // We verify both complete.
    std::atomic<int> count{0};

    rt.block_on([&]() -> coro::Coro<void> {
        auto h1 = coro::spawn(coro::co_invoke([&]() -> coro::Coro<void> {
            co_await coro::spawn_blocking([&] {
                std::this_thread::sleep_for(10ms);
                count.fetch_add(1);
            });
            co_return;
        })).submit();

        auto h2 = coro::spawn(coro::co_invoke([&]() -> coro::Coro<void> {
            count.fetch_add(1);
            co_return;
        })).submit();

        co_await h1;
        co_await h2;
    }());

    EXPECT_EQ(count.load(), 2);
}

// ---------------------------------------------------------------------------
// Multiple concurrent blocking tasks
// ---------------------------------------------------------------------------

TEST(SpawnBlocking, MultipleConcurrentTasks) {
    coro::Runtime rt(4);
    constexpr int N = 10;
    std::atomic<int> sum{0};

    rt.block_on([&]() -> coro::Coro<void> {
        coro::JoinSet<void> js;
        for (int i = 0; i < N; ++i) {
            js.spawn(coro::co_invoke([&, i]() -> coro::Coro<void> {
                int v = co_await coro::spawn_blocking([i] { return i * i; });
                sum.fetch_add(v);
            }));
        }
        co_await js.drain();
    }());

    // sum of squares 0..9
    int expected = 0;
    for (int i = 0; i < N; ++i) expected += i * i;
    EXPECT_EQ(sum.load(), expected);
}

// ---------------------------------------------------------------------------
// Detach on drop — handle dropped without awaiting, blocking work still runs
// ---------------------------------------------------------------------------

TEST(SpawnBlocking, DetachOnDrop) {
    coro::Runtime rt(1);
    std::atomic<bool> ran{false};

    rt.block_on([&]() -> coro::Coro<void> {
        {
            // Drop the handle immediately — detaches the thread.
            [[maybe_unused]] auto h = coro::spawn_blocking([&] {
                std::this_thread::sleep_for(5ms);
                ran.store(true);
            });
        }
        // Give the detached thread time to finish.
        co_await coro::sleep_for(50ms);
    }());

    EXPECT_TRUE(ran.load());
}

// ---------------------------------------------------------------------------
// blocking_get() — synchronous wait from a blocking thread (recursive)
// ---------------------------------------------------------------------------

TEST(SpawnBlocking, BlockingGetRecursive) {
    coro::Runtime rt(1);
    int result = rt.block_on([]() -> coro::Coro<int> {
        co_return co_await coro::spawn_blocking([] {
            // From a blocking thread, submit sub-work and wait synchronously.
            auto h = coro::spawn_blocking([] { return 99; });
            return h.blocking_get();
        });
    }());
    EXPECT_EQ(result, 99);
}

TEST(SpawnBlocking, BlockingGetVoid) {
    coro::Runtime rt(1);
    std::atomic<bool> ran{false};
    rt.block_on([&]() -> coro::Coro<void> {
        co_await coro::spawn_blocking([&] {
            auto h = coro::spawn_blocking([&] { ran.store(true); });
            h.blocking_get();
        });
    }());
    EXPECT_TRUE(ran.load());
}

// ---------------------------------------------------------------------------
// Moved-from handle throws on use
// ---------------------------------------------------------------------------

TEST(SpawnBlocking, MovedFromHandleThrows) {
    coro::Runtime rt(1);
    rt.block_on([]() -> coro::Coro<void> {
        auto h = coro::spawn_blocking([] { return 1; });
        auto h2 = std::move(h);
        // h is now moved-from — blocking_get() should throw.
        EXPECT_THROW(h.blocking_get(), std::logic_error);
        co_await h2;  // consume h2 so the blocking thread completes cleanly
    }());
}
