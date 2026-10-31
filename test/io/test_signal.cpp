#include <gtest/gtest.h>
#include "executor_traits.h"
#include "signal_test_util.h"
#include <coro/io/signal.h>
#include <coro/runtime/runtime.h>
#include <coro/runtime/current_thread_executor.h>
#include <coro/runtime/parker.h>
#include <coro/coro.h>
#include <coro/future.h>
#include <coro/stream.h>
#include <coro/sync/sleep.h>
#include <coro/sync/timeout.h>

#ifndef _WIN32
#include <signal.h>
#include <unistd.h>
#include <csignal>
#endif

#include <atomic>
#include <chrono>
#include <latch>
#include <memory>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <utility>

using namespace coro;
using namespace std::chrono_literals;
using signal_test::kSignalA;
using signal_test::kSignalB;

// ---------------------------------------------------------------------------
// Concept checks
// ---------------------------------------------------------------------------

static_assert(Future<SignalFuture>);
static_assert(Stream<SignalStream>);

// NOTE: signal_test::raise() runs the handler on the calling thread before it returns
// (see signal_test_util.h), so the delivery is counted and the watchers' fds are
// readable by the time the next line runs: no extra synchronisation is needed.
// Watching starts at creation, so raising between creation and the first co_await is
// the normal case under test. Every await is bounded by a timeout so a lost wake-up
// fails the test instead of hanging the suite.

// ---------------------------------------------------------------------------
// SignalFuture
// ---------------------------------------------------------------------------

template<typename Traits>
class SignalFutureTest : public testing::Test {
protected:
    Traits traits;
};
TYPED_TEST_SUITE(SignalFutureTest, AllExecutors);

TYPED_TEST(SignalFutureTest, ResolvesOnDelivery) {
    bool delivered = this->traits.rt.block_on([]() -> Coro<bool> {
        auto sig = signal_test::watch(kSignalA);
        signal_test::raise(kSignalA);
        auto got = co_await coro::timeout(5s, std::move(sig));
        co_return got.index() == 0;
    }());
    EXPECT_TRUE(delivered);
}

TYPED_TEST(SignalFutureTest, PendingUntilDelivered) {
    std::size_t index = this->traits.rt.block_on([]() -> Coro<std::size_t> {
        auto got = co_await coro::timeout(20ms, signal_test::watch(kSignalA));
        co_return got.index();
    }());
    EXPECT_EQ(index, 1u);   // timed out
}

// The handler may run on any thread: on POSIX any thread that doesn't block the
// signal, on Windows always a thread of the system's own. The dispatch must not care
// which.
TYPED_TEST(SignalFutureTest, ResolvesOnDeliveryFromAnotherThread) {
    bool delivered = this->traits.rt.block_on([]() -> Coro<bool> {
        auto sig = signal_test::watch(kSignalA);
#ifdef _WIN32
        std::thread([] { signal_test::raise(kSignalA); }).join();
#else
        std::thread([] { ::kill(::getpid(), kSignalA); }).join();
#endif
        auto got = co_await coro::timeout(5s, std::move(sig));
        co_return got.index() == 0;
    }());
    EXPECT_TRUE(delivered);
}

// A delivery counted before a watcher existed belongs to the watchers that existed then.
TYPED_TEST(SignalFutureTest, IgnoresDeliveriesBeforeCreation) {
    std::size_t index = this->traits.rt.block_on([]() -> Coro<std::size_t> {
        auto earlier = signal_test::watch(kSignalA);   // keeps the handler installed
        signal_test::raise(kSignalA);
        auto got = co_await coro::timeout(20ms, signal_test::watch(kSignalA));
        co_return got.index();
    }());
    EXPECT_EQ(index, 1u);   // timed out
}

TYPED_TEST(SignalFutureTest, DroppingBeforeDeliveryDoesNotHang) {
    this->traits.rt.block_on([]() -> Coro<void> {
        { auto sig = signal_test::watch(kSignalA); }
        co_return;
    }());
}

TYPED_TEST(SignalFutureTest, MultipleIndependentWatchersOfSameSignal) {
    bool both = this->traits.rt.block_on([]() -> Coro<bool> {
        auto sig_a = signal_test::watch(kSignalA);
        auto sig_b = signal_test::watch(kSignalA);
        signal_test::raise(kSignalA);
        auto a = co_await coro::timeout(5s, std::move(sig_a));
        auto b = co_await coro::timeout(5s, std::move(sig_b));
        co_return a.index() == 0 && b.index() == 0;
    }());
    EXPECT_TRUE(both);
}

TYPED_TEST(SignalFutureTest, InvalidSignalThrows) {
    this->traits.rt.block_on([]() -> Coro<void> {
#ifdef _WIN32
        using coro::windows::ConsoleEvent;
        // 3 and 4 are gaps in Windows' numbering; 7 is past the last event.
        for (int bad : {3, 4, 7, -1})
            EXPECT_THROW((void)coro::windows::console_event(static_cast<ConsoleEvent>(bad)),
                         std::system_error);
        EXPECT_THROW((void)coro::windows::console_event_stream(
                         {ConsoleEvent::CtrlBreak, static_cast<ConsoleEvent>(3)}),
                     std::system_error);
#else
        EXPECT_THROW((void)coro::signal(SIGKILL), std::system_error);
        EXPECT_THROW((void)coro::signal(SIGSTOP), std::system_error);
        EXPECT_THROW((void)coro::signal(0), std::system_error);
        EXPECT_THROW((void)coro::signal(-1), std::system_error);
        EXPECT_THROW((void)coro::signal_stream({SIGUSR1, SIGKILL}), std::system_error);
#endif
        co_return;
    }());
}

// ---------------------------------------------------------------------------
// SignalStream
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
            auto sigs = signal_test::watch_stream({kSignalA});
            signal_test::raise(kSignalA);
            auto r = co_await coro::timeout(5s, next(sigs));
            if (r.index() != 0) co_return std::nullopt;
            co_return std::get<0>(r).value;
        }());
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->signum, signal_test::signum(kSignalA));
    EXPECT_EQ(got->count, 1u);
}

// A burst raised before the consumer polls coalesces into one item. Each raise runs
// the handler to completion before returning, so none of the three is merged by the
// kernel and the count is exact here; in general it is a lower bound.
TYPED_TEST(SignalStreamTest, CoalescesBurstIntoOneEventWithCount) {
    std::optional<SignalEvent> got = this->traits.rt.block_on(
        []() -> Coro<std::optional<SignalEvent>> {
            auto sigs = signal_test::watch_stream({kSignalA});
            signal_test::raise(kSignalA);
            signal_test::raise(kSignalA);
            signal_test::raise(kSignalA);
            auto r = co_await coro::timeout(5s, next(sigs));
            if (r.index() != 0) co_return std::nullopt;
            co_return std::get<0>(r).value;
        }());
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->signum, signal_test::signum(kSignalA));
    EXPECT_EQ(got->count, 3u);
}

TYPED_TEST(SignalStreamTest, DistinctSignalsYieldSeparateEvents) {
    std::pair<std::optional<SignalEvent>, std::optional<SignalEvent>> got =
        this->traits.rt.block_on(
            []() -> Coro<std::pair<std::optional<SignalEvent>, std::optional<SignalEvent>>> {
                auto sigs = signal_test::watch_stream({kSignalA, kSignalB});
                signal_test::raise(kSignalA);
                signal_test::raise(kSignalB);
                std::pair<std::optional<SignalEvent>, std::optional<SignalEvent>> out;
                auto first = co_await coro::timeout(5s, next(sigs));
                if (first.index() == 0) out.first = std::get<0>(first).value;
                auto second = co_await coro::timeout(5s, next(sigs));
                if (second.index() == 0) out.second = std::get<0>(second).value;
                co_return out;
            }());
    ASSERT_TRUE(got.first.has_value());
    ASSERT_TRUE(got.second.has_value());
    const int a = signal_test::signum(kSignalA);
    const int b = signal_test::signum(kSignalB);
    EXPECT_NE(got.first->signum, got.second->signum);
    EXPECT_TRUE((got.first->signum == a && got.second->signum == b) ||
                (got.first->signum == b && got.second->signum == a));
}

// Items after the first only carry deliveries since the previous item.
TYPED_TEST(SignalStreamTest, CountsResetBetweenItems) {
    std::pair<uint64_t, uint64_t> counts = this->traits.rt.block_on(
        []() -> Coro<std::pair<uint64_t, uint64_t>> {
            auto sigs = signal_test::watch_stream({kSignalA});
            std::pair<uint64_t, uint64_t> out{0, 0};
            signal_test::raise(kSignalA);
            signal_test::raise(kSignalA);
            auto first = co_await coro::timeout(5s, next(sigs));
            if (first.index() == 0 && std::get<0>(first).value)
                out.first = std::get<0>(first).value->count;
            signal_test::raise(kSignalA);
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
        { auto sigs = signal_test::watch_stream({kSignalA}); }
        co_return;
    }());
}

// ---------------------------------------------------------------------------
// ctrl_c (every platform)
// ---------------------------------------------------------------------------

namespace {

// Ctrl-C as the platform delivers it: SIGINT, or the console handler with CTRL_C_EVENT.
void raise_ctrl_c() {
#ifdef _WIN32
    signal_test::raise(coro::windows::ConsoleEvent::CtrlC);
#else
    ::raise(SIGINT);
#endif
}

int ctrl_c_signum() {
#ifdef _WIN32
    return static_cast<int>(coro::windows::ConsoleEvent::CtrlC);
#else
    return SIGINT;
#endif
}

} // namespace

template<typename Traits>
class CtrlCTest : public testing::Test {
protected:
    Traits traits;
};
TYPED_TEST_SUITE(CtrlCTest, AllExecutors);

TYPED_TEST(CtrlCTest, FutureResolvesOnCtrlC) {
    bool delivered = this->traits.rt.block_on([]() -> Coro<bool> {
        auto stop = coro::ctrl_c();
        raise_ctrl_c();
        auto got = co_await coro::timeout(5s, std::move(stop));
        co_return got.index() == 0;
    }());
    EXPECT_TRUE(delivered);
}

TYPED_TEST(CtrlCTest, StreamYieldsEachCtrlC) {
    std::pair<std::optional<SignalEvent>, std::optional<SignalEvent>> got =
        this->traits.rt.block_on(
            []() -> Coro<std::pair<std::optional<SignalEvent>, std::optional<SignalEvent>>> {
                auto presses = coro::ctrl_c_stream();
                std::pair<std::optional<SignalEvent>, std::optional<SignalEvent>> out;
                raise_ctrl_c();
                auto first = co_await coro::timeout(5s, next(presses));
                if (first.index() == 0) out.first = std::get<0>(first).value;
                raise_ctrl_c();
                auto second = co_await coro::timeout(5s, next(presses));
                if (second.index() == 0) out.second = std::get<0>(second).value;
                co_return out;
            }());
    ASSERT_TRUE(got.first.has_value());
    ASSERT_TRUE(got.second.has_value());
    EXPECT_EQ(got.first->signum, ctrl_c_signum());
    EXPECT_EQ(got.first->count, 1u);
    EXPECT_EQ(got.second->count, 1u);
}

// ---------------------------------------------------------------------------
// Process-wide behaviour
// ---------------------------------------------------------------------------

namespace {

// Whether kSignalA would get the platform's default behaviour now. On POSIX: its action
// is SIG_DFL. On Windows: coro's handler passes it on (returns FALSE), so Windows goes
// on to the default, ExitProcess(). Raising it there is safe: the handler only reports.
bool default_behaviour(signal_test::Signal s) {
#ifdef _WIN32
    return !signal_test::raise(s);
#else
    struct sigaction current {};
    ::sigaction(s, nullptr, &current);
    return !(current.sa_flags & SA_SIGINFO) && current.sa_handler == SIG_DFL;
#endif
}

// Creates a watcher of kSignalB on this Runtime, signals `registered`, then waits.
Coro<bool> wait_for_signal_b(std::latch& registered) {
    auto sig = signal_test::watch(kSignalB);
    registered.count_down();
    auto got = co_await coro::timeout(5s, std::move(sig));
    co_return got.index() == 0;
}

} // namespace

// The last watcher to go restores the default behaviour.
TEST(SignalTest, RestoresDefaultWhenLastWatcherDrops) {
    ASSERT_TRUE(default_behaviour(kSignalB));
    Runtime rt(std::size_t{1});
    rt.block_on([]() -> Coro<void> {
        auto a = signal_test::watch(kSignalB);
        EXPECT_FALSE(default_behaviour(kSignalB));
        {
            auto b = signal_test::watch_stream({kSignalB});
        }
        EXPECT_FALSE(default_behaviour(kSignalB));   // `a` still watches
        co_return;
    }());
    EXPECT_TRUE(default_behaviour(kSignalB));
}

// One delivery reaches watchers on two Runtimes, each woken through its own driver
// or by the other's broadcast, whichever drains first.
TEST(SignalTest, WatchersOnSeparateRuntimesBothResolve) {
    std::latch registered(1);
    bool other_delivered = false;
    std::thread other([&] {
        Runtime rt(std::size_t{1});
        other_delivered = rt.block_on(wait_for_signal_b(registered));
    });

    Runtime rt(std::size_t{4});
    bool delivered = rt.block_on([](std::latch& registered) -> Coro<bool> {
        auto sig = signal_test::watch(kSignalB);
        registered.wait();   // briefly blocks this worker; the other Runtime is unaffected
        signal_test::raise(kSignalB);
        auto got = co_await coro::timeout(5s, std::move(sig));
        co_return got.index() == 0;
    }(registered));
    other.join();

    EXPECT_TRUE(delivered);
    EXPECT_TRUE(other_delivered);
}

// A CurrentThreadExecutor with a caller-supplied parker never turns the IoDriver, so a
// watcher there could never be woken: creating one throws instead of hanging later.
TEST(SignalTest, ThrowsWithoutDriver) {
    Runtime rt(std::in_place_type<CurrentThreadExecutor>,
               std::make_unique<PollingParker>([] {}));
    rt.block_on([]() -> Coro<void> {
        EXPECT_THROW((void)signal_test::watch(kSignalA), std::logic_error);
        EXPECT_THROW((void)signal_test::watch_stream({kSignalA}), std::logic_error);
        EXPECT_THROW((void)coro::ctrl_c(), std::logic_error);
        EXPECT_THROW((void)coro::shutdown_requested(), std::logic_error);
        co_return;
    }());
    EXPECT_TRUE(default_behaviour(kSignalA));   // nothing was installed
}

// ---------------------------------------------------------------------------
// shutdown_requested (every platform)
// ---------------------------------------------------------------------------

static_assert(Future<ShutdownFuture>);

namespace {

// The requests shutdown_requested() watches by default, as the platform delivers them.
#ifdef _WIN32
using Request = coro::windows::ConsoleEvent;
const Request kRequests[] = {Request::Close, Request::Logoff, Request::Shutdown};
#else
using Request = int;
const Request kRequests[] = {SIGTERM, SIGHUP};
#endif

// Delivers `r` on the calling thread. On Windows the handler doesn't return until the
// guard is dropped, so call this on a thread of its own (Windows uses one too).
void raise_request(Request r) {
#ifdef _WIN32
    EXPECT_TRUE(signal_test::raise(r));
#else
    ::raise(r);
#endif
}

} // namespace

TEST(ShutdownTest, ResolvesOnEachRequest) {
    Runtime rt(std::size_t{2});
    for (Request r : kRequests) {
        std::thread os_thread;   // stands in for the thread Windows runs the handler on
        const bool delivered = rt.block_on([&]() -> Coro<bool> {
            auto request = coro::shutdown_requested();
            os_thread = std::thread([r] { raise_request(r); });
            auto got = co_await coro::timeout(5s, std::move(request));
            co_return got.index() == 0;   // the guard is dropped with `got`
        }());
        os_thread.join();
        EXPECT_TRUE(delivered) << "request " << static_cast<int>(r);
    }
}

// While the guard exists the request stays handled: on Windows the handler doesn't
// return (the process would end), on POSIX a repeated SIGTERM is absorbed. The future
// is a temporary, gone once the await completes: the guard alone holds on then.
TEST(ShutdownTest, GuardHoldsUntilDropped) {
#ifndef _WIN32
    ASSERT_TRUE(default_behaviour(SIGTERM));
#endif
    Runtime rt(std::size_t{2});
    std::atomic<bool> handler_returned{false};
    std::thread os_thread;

    const bool held = rt.block_on([&]() -> Coro<bool> {
        auto request = coro::shutdown_requested();
        os_thread = std::thread([&] {
            raise_request(kRequests[0]);
            handler_returned.store(true);
        });
        auto got = co_await coro::timeout(5s, std::move(request));
        if (got.index() != 0) co_return false;
        ShutdownGuard guard = std::move(std::get<0>(got).value);
        co_await coro::sleep_for(50ms);   // the application's "cleanup"
#ifdef _WIN32
        const bool holding = !handler_returned.load();
#else
        const bool holding = !default_behaviour(SIGTERM);
        ::raise(SIGTERM);   // absorbed; with the default action the test process ends here
#endif
        {
            auto released = std::move(guard);   // dropped here
        }
        co_return holding;
    }());
    os_thread.join();

    EXPECT_TRUE(held) << "the request wasn't held while the guard was alive";
    EXPECT_TRUE(handler_returned.load());
#ifndef _WIN32
    EXPECT_TRUE(default_behaviour(SIGTERM));   // restored once the guard went
#endif
}

#ifndef _WIN32

TEST(ShutdownTest, HangupCanBeLeftOut) {
    ASSERT_TRUE(default_behaviour(SIGHUP));
    Runtime rt(std::size_t{1});
    rt.block_on([]() -> Coro<void> {
        auto request = coro::shutdown_requested({.hangup = false});
        EXPECT_FALSE(default_behaviour(SIGTERM));
        EXPECT_TRUE(default_behaviour(SIGHUP));
        co_return;
    }());
}

// Under nohup SIGHUP is ignored; watching it would undo that.
TEST(ShutdownTest, IgnoredHangupStaysIgnored) {
    struct sigaction ignore {}, previous {};
    ignore.sa_handler = SIG_IGN;
    sigemptyset(&ignore.sa_mask);
    ASSERT_EQ(::sigaction(SIGHUP, &ignore, &previous), 0);

    Runtime rt(std::size_t{1});
    rt.block_on([]() -> Coro<void> {
        auto request = coro::shutdown_requested();
        struct sigaction current {};
        ::sigaction(SIGHUP, nullptr, &current);
        EXPECT_EQ(current.sa_handler, SIG_IGN);
        EXPECT_FALSE(default_behaviour(SIGTERM));
        co_return;
    }());

    ::sigaction(SIGHUP, &previous, nullptr);
}

#endif  // !_WIN32

#ifdef _WIN32

// The events that end the process are only watched through shutdown_requested(),
// whose guard makes the handler wait; a plain watcher couldn't hold it through the
// cleanup.
TEST(SignalTest, ConsoleEventRejectsEventsThatEndTheProcess) {
    using coro::windows::ConsoleEvent;
    Runtime rt(std::size_t{1});
    rt.block_on([]() -> Coro<void> {
        for (ConsoleEvent e : {ConsoleEvent::Close, ConsoleEvent::Logoff, ConsoleEvent::Shutdown}) {
            EXPECT_THROW((void)coro::windows::console_event(e), std::invalid_argument);
            EXPECT_THROW((void)coro::windows::console_event_stream({ConsoleEvent::CtrlC, e}),
                         std::invalid_argument);
        }
        co_return;
    }());
}

// Ctrl-C and Ctrl-Break don't end the process, so their handler never waits.
TEST(SignalTest, CtrlBreakHandlerReturnsAtOnce) {
    Runtime rt(std::size_t{1});
    rt.block_on([]() -> Coro<void> {
        auto sig = signal_test::watch(coro::windows::ConsoleEvent::CtrlBreak);
        const auto start = std::chrono::steady_clock::now();
        EXPECT_TRUE(signal_test::raise(coro::windows::ConsoleEvent::CtrlBreak));
        EXPECT_LT(std::chrono::steady_clock::now() - start, 1s);
        co_return;
    }());
}

#endif  // _WIN32
