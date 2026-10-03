#include <gtest/gtest.h>
#include "executor_traits.h"
#include <coro/io/signal.h>
#include <coro/runtime/runtime.h>
#include <coro/runtime/current_thread_executor.h>
#include <coro/runtime/parker.h>
#include <coro/coro.h>
#include <coro/stream.h>
#include <coro/sync/timeout.h>
#include <signal.h>
#include <unistd.h>
#include <chrono>
#include <csignal>
#include <latch>
#include <memory>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <utility>

using namespace coro;
using namespace std::chrono_literals;

// ---------------------------------------------------------------------------
// Concept checks
// ---------------------------------------------------------------------------

static_assert(Future<SignalFuture>);
static_assert(Stream<SignalStream>);

// NOTE: ::raise() runs the handler on the calling thread before it returns, so the
// delivery is counted and its byte is in the self-pipe by the time the next line runs:
// no extra synchronisation is needed. Watching starts at signal()/signal_stream(), so raising
// between creation and the first co_await is the normal case under test. Every await is
// bounded by a timeout so a lost wake-up fails the test instead of hanging the suite.
// Tests run one at a time, so SIGUSR1/SIGUSR2 have no watchers outside the test body.

// ---------------------------------------------------------------------------
// SignalFuture (coro::signal)
// ---------------------------------------------------------------------------

template<typename Traits>
class SignalFutureTest : public testing::Test {
protected:
    Traits traits;
};
TYPED_TEST_SUITE(SignalFutureTest, AllExecutors);

TYPED_TEST(SignalFutureTest, ResolvesOnDelivery) {
    bool delivered = this->traits.rt.block_on([]() -> Coro<bool> {
        auto sig = coro::signal(SIGUSR1);
        ::raise(SIGUSR1);
        auto got = co_await coro::timeout(5s, std::move(sig));
        co_return got.index() == 0;
    }());
    EXPECT_TRUE(delivered);
}

TYPED_TEST(SignalFutureTest, PendingUntilDelivered) {
    std::size_t index = this->traits.rt.block_on([]() -> Coro<std::size_t> {
        auto got = co_await coro::timeout(20ms, coro::signal(SIGUSR1));
        co_return got.index();
    }());
    EXPECT_EQ(index, 1u);   // timed out
}

// The kernel may run the handler on any thread that doesn't block the signal; the
// dispatch must not care which.
TYPED_TEST(SignalFutureTest, ResolvesOnDeliveryFromAnotherThread) {
    bool delivered = this->traits.rt.block_on([]() -> Coro<bool> {
        auto sig = coro::signal(SIGUSR1);
        std::thread([] { ::kill(::getpid(), SIGUSR1); }).join();
        auto got = co_await coro::timeout(5s, std::move(sig));
        co_return got.index() == 0;
    }());
    EXPECT_TRUE(delivered);
}

// A delivery counted before a watcher existed belongs to the watchers that existed then.
TYPED_TEST(SignalFutureTest, IgnoresDeliveriesBeforeCreation) {
    std::size_t index = this->traits.rt.block_on([]() -> Coro<std::size_t> {
        auto earlier = coro::signal(SIGUSR1);   // keeps the handler installed
        ::raise(SIGUSR1);
        auto got = co_await coro::timeout(20ms, coro::signal(SIGUSR1));
        co_return got.index();
    }());
    EXPECT_EQ(index, 1u);   // timed out
}

TYPED_TEST(SignalFutureTest, DroppingBeforeDeliveryDoesNotHang) {
    this->traits.rt.block_on([]() -> Coro<void> {
        { auto sig = coro::signal(SIGUSR1); }
        co_return;
    }());
}

TYPED_TEST(SignalFutureTest, MultipleIndependentWatchersOfSameSignal) {
    bool both = this->traits.rt.block_on([]() -> Coro<bool> {
        auto sig_a = coro::signal(SIGUSR1);
        auto sig_b = coro::signal(SIGUSR1);
        ::raise(SIGUSR1);
        auto a = co_await coro::timeout(5s, std::move(sig_a));
        auto b = co_await coro::timeout(5s, std::move(sig_b));
        co_return a.index() == 0 && b.index() == 0;
    }());
    EXPECT_TRUE(both);
}

TYPED_TEST(SignalFutureTest, InvalidSignalThrows) {
    this->traits.rt.block_on([]() -> Coro<void> {
        EXPECT_THROW((void)coro::signal(SIGKILL), std::system_error);
        EXPECT_THROW((void)coro::signal(SIGSTOP), std::system_error);
        EXPECT_THROW((void)coro::signal(0), std::system_error);
        EXPECT_THROW((void)coro::signal(-1), std::system_error);
        EXPECT_THROW((void)coro::signal_stream({SIGUSR1, SIGKILL}), std::system_error);
        co_return;
    }());
}

// ---------------------------------------------------------------------------
// SignalStream (coro::signal_stream)
// ---------------------------------------------------------------------------

template<typename Traits>
class SignalStreamTest : public testing::Test {
protected:
    Traits traits;
};
TYPED_TEST_SUITE(SignalStreamTest, AllExecutors);

TYPED_TEST(SignalStreamTest, ResolvesOnDelivery) {
    std::optional<SignalEvent> got = this->traits.rt.block_on(
        []() -> Coro<std::optional<SignalEvent>> {
            auto sigs = coro::signal_stream({SIGUSR1});
            ::raise(SIGUSR1);
            auto r = co_await coro::timeout(5s, next(sigs));
            if (r.index() != 0) co_return std::nullopt;
            co_return std::get<0>(r).value;
        }());
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->signum, SIGUSR1);
    EXPECT_EQ(got->count, 1u);
}

// A burst raised before the consumer polls coalesces into one item. Each ::raise()
// runs the handler to completion before returning, so none of the three is merged by
// the kernel and the count is exact here; in general it is a lower bound.
TYPED_TEST(SignalStreamTest, CoalescesBurstIntoOneEventWithCount) {
    std::optional<SignalEvent> got = this->traits.rt.block_on(
        []() -> Coro<std::optional<SignalEvent>> {
            auto sigs = coro::signal_stream({SIGUSR1});
            ::raise(SIGUSR1);
            ::raise(SIGUSR1);
            ::raise(SIGUSR1);
            auto r = co_await coro::timeout(5s, next(sigs));
            if (r.index() != 0) co_return std::nullopt;
            co_return std::get<0>(r).value;
        }());
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->signum, SIGUSR1);
    EXPECT_EQ(got->count, 3u);
}

TYPED_TEST(SignalStreamTest, DistinctSignalsYieldSeparateEvents) {
    std::pair<std::optional<SignalEvent>, std::optional<SignalEvent>> got =
        this->traits.rt.block_on(
            []() -> Coro<std::pair<std::optional<SignalEvent>, std::optional<SignalEvent>>> {
                auto sigs = coro::signal_stream({SIGUSR1, SIGUSR2});
                ::raise(SIGUSR1);
                ::raise(SIGUSR2);
                std::pair<std::optional<SignalEvent>, std::optional<SignalEvent>> out;
                auto first = co_await coro::timeout(5s, next(sigs));
                if (first.index() == 0) out.first = std::get<0>(first).value;
                auto second = co_await coro::timeout(5s, next(sigs));
                if (second.index() == 0) out.second = std::get<0>(second).value;
                co_return out;
            }());
    ASSERT_TRUE(got.first.has_value());
    ASSERT_TRUE(got.second.has_value());
    EXPECT_NE(got.first->signum, got.second->signum);
    EXPECT_TRUE((got.first->signum == SIGUSR1 && got.second->signum == SIGUSR2) ||
                (got.first->signum == SIGUSR2 && got.second->signum == SIGUSR1));
}

// Items after the first only carry deliveries since the previous item.
TYPED_TEST(SignalStreamTest, CountsResetBetweenItems) {
    std::pair<uint64_t, uint64_t> counts = this->traits.rt.block_on(
        []() -> Coro<std::pair<uint64_t, uint64_t>> {
            auto sigs = coro::signal_stream({SIGUSR1});
            std::pair<uint64_t, uint64_t> out{0, 0};
            ::raise(SIGUSR1);
            ::raise(SIGUSR1);
            auto first = co_await coro::timeout(5s, next(sigs));
            if (first.index() == 0 && std::get<0>(first).value)
                out.first = std::get<0>(first).value->count;
            ::raise(SIGUSR1);
            auto second = co_await coro::timeout(5s, next(sigs));
            if (second.index() == 0 && std::get<0>(second).value)
                out.second = std::get<0>(second).value->count;
            co_return out;
        }());
    EXPECT_EQ(counts.first, 2u);
    EXPECT_EQ(counts.second, 1u);
}

TYPED_TEST(SignalStreamTest, DroppingBeforeDeliveryDoesNotHang) {
    this->traits.rt.block_on([]() -> Coro<void> {
        { auto sigs = coro::signal_stream({SIGUSR1}); }
        co_return;
    }());
}

// ---------------------------------------------------------------------------
// Process-wide behaviour
// ---------------------------------------------------------------------------

namespace {

bool handler_is_default(int signum) {
    struct sigaction current {};
    ::sigaction(signum, nullptr, &current);
    return !(current.sa_flags & SA_SIGINFO) && current.sa_handler == SIG_DFL;
}

// Creates a watcher of SIGUSR2 on this Runtime, signals `registered`, then waits.
Coro<bool> wait_for_sigusr2(std::latch& registered) {
    auto sig = coro::signal(SIGUSR2);
    registered.count_down();
    auto got = co_await coro::timeout(5s, std::move(sig));
    co_return got.index() == 0;
}

} // namespace

// The last watcher to go restores the action that was in place before the first.
TEST(SignalTest, RestoresPreviousActionWhenLastWatcherDrops) {
    ASSERT_TRUE(handler_is_default(SIGUSR2));
    Runtime rt(std::size_t{1});
    rt.block_on([]() -> Coro<void> {
        auto a = coro::signal(SIGUSR2);
        EXPECT_FALSE(handler_is_default(SIGUSR2));
        {
            auto b = coro::signal_stream({SIGUSR2});
        }
        EXPECT_FALSE(handler_is_default(SIGUSR2));   // `a` still watches
        co_return;
    }());
    EXPECT_TRUE(handler_is_default(SIGUSR2));
}

// One delivery reaches watchers on two Runtimes, each woken through its own driver
// or by the other's broadcast, whichever drains the self-pipe first.
TEST(SignalTest, WatchersOnSeparateRuntimesBothResolve) {
    std::latch registered(1);
    bool other_delivered = false;
    std::thread other([&] {
        Runtime rt(std::size_t{1});
        other_delivered = rt.block_on(wait_for_sigusr2(registered));
    });

    Runtime rt(std::size_t{4});
    bool delivered = rt.block_on([](std::latch& registered) -> Coro<bool> {
        auto sig = coro::signal(SIGUSR2);
        registered.wait();   // briefly blocks this worker; the other Runtime is unaffected
        ::raise(SIGUSR2);
        auto got = co_await coro::timeout(5s, std::move(sig));
        co_return got.index() == 0;
    }(registered));
    other.join();

    EXPECT_TRUE(delivered);
    EXPECT_TRUE(other_delivered);
}

// A CurrentThreadExecutor with a caller-supplied parker never turns the IoDriver, so a
// watcher there could never be woken: signal() refuses instead of hanging later.
TEST(SignalTest, ThrowsWithoutDriver) {
    Runtime rt(std::in_place_type<CurrentThreadExecutor>,
               std::make_unique<PollingParker>([] {}));
    rt.block_on([]() -> Coro<void> {
        EXPECT_THROW((void)coro::signal(SIGUSR1), std::logic_error);
        EXPECT_THROW((void)coro::signal_stream({SIGUSR1}), std::logic_error);
        co_return;
    }());
    EXPECT_TRUE(handler_is_default(SIGUSR1));   // nothing was installed
}
