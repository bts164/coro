#include <gtest/gtest.h>
#include "executor_traits.h"
#include <coro/io/signal.h>
#include <coro/runtime/runtime.h>
#include <coro/task/spawn_on.h>
#include <coro/coro.h>
#include <coro/stream.h>
#include <csignal>

using namespace coro;

// ---------------------------------------------------------------------------
// Concept checks
// ---------------------------------------------------------------------------

static_assert(Future<SignalFuture>);
static_assert(Stream<SignalStream>);

// NOTE: every test below raises its signal from the same OS thread that runs the uv
// loop — but the *coroutine* awaiting the result may run on a different OS thread,
// since AllExecutors parameterizes the task scheduler (WorkStealingTraits,
// WorkSharingTraits, etc.) independently of the dedicated uv-loop thread every Runtime
// owns. That's exactly the race SignalState::mutex exists to guard: signal_cb always
// fires on the uv thread, while poll()/poll_next() may be invoked concurrently on a
// worker thread. We deliberately do NOT additionally test raising from a separate OS
// thread via std::thread + kill(getpid(), signum) or pthread_kill(). Reasoning: libuv
// installs one process-wide sigaction per signum whose handler just writes a byte to a
// self-pipe (async-signal-safe) — it does not matter which thread the kernel chooses to
// run that handler on, since the coalescing logic in signal_cb only ever runs
// afterward on the uv loop thread, when uv_run() reads that pipe. So the dispatch path
// this library owns is single-threaded by construction regardless of which thread the
// OS delivered to. We're relying on libuv's self-pipe trick to be correct here rather
// than independently verifying it. If that assumption is ever found to be wrong, come
// back here, update this note, and add a real cross-thread-raise test.
namespace {

// Raises `signum` on the uv thread. Routing the raise through with_context() on the
// same SingleThreadedUvExecutor that signal()/signal_stream() registered the
// uv_signal_t handle on guarantees FIFO ordering relative to that registration: both
// the registration coroutine and this one are enqueued on the same single-threaded
// ready queue, so awaiting this completes only after registration has already run.
// Without this, raising immediately after signal() would race uv_signal_start().
Coro<void> raise_on_uv_thread(SingleThreadedUvExecutor& exec, int signum) {
    co_await with_context(exec, [](int signum) -> Coro<void> {
        ::raise(signum);
        co_return;
    }(signum));
}

// Yields to the uv thread and back without touching any signal — used after dropping
// a SignalFuture/SignalStream to give its detached, asynchronous teardown coroutine a
// chance to actually run before the test exits (rather than testing nothing because
// the process exits before the uv thread ever gets scheduled).
Coro<void> yield_to_uv_thread(SingleThreadedUvExecutor& exec) {
    co_await with_context(exec, []() -> Coro<void> { co_return; }());
}

} // namespace

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
    bool delivered = false;
    this->traits.rt.block_on([](bool& delivered) -> Coro<void> {
        auto sig = coro::signal(SIGUSR1);
        co_await raise_on_uv_thread(current_uv_executor(), SIGUSR1);
        co_await sig;
        delivered = true;
    }(delivered));
    EXPECT_TRUE(delivered);
}

TYPED_TEST(SignalFutureTest, DroppingBeforeDeliveryDoesNotHang) {
    this->traits.rt.block_on([]() -> Coro<void> {
        { auto sig = coro::signal(SIGUSR1); }
        co_await yield_to_uv_thread(current_uv_executor());
    }());
}

TYPED_TEST(SignalFutureTest, MultipleIndependentWatchersOfSameSignal) {
    bool a_delivered = false, b_delivered = false;
    this->traits.rt.block_on([](bool& a, bool& b) -> Coro<void> {
        auto sig_a = coro::signal(SIGUSR1);
        auto sig_b = coro::signal(SIGUSR1);
        co_await raise_on_uv_thread(current_uv_executor(), SIGUSR1);
        co_await sig_a;
        a = true;
        co_await sig_b;
        b = true;
    }(a_delivered, b_delivered));
    EXPECT_TRUE(a_delivered);
    EXPECT_TRUE(b_delivered);
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
    std::optional<SignalEvent> got;
    this->traits.rt.block_on([](std::optional<SignalEvent>& got) -> Coro<void> {
        auto sigs = coro::signal_stream({SIGUSR1});
        co_await raise_on_uv_thread(current_uv_executor(), SIGUSR1);
        got = co_await next(sigs);
    }(got));
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->signum, SIGUSR1);
    EXPECT_GE(got->count, 1u);
}

// A burst of same-signum raises before the consumer polls coalesces into a single
// item with count == the number of raises, per the deliberate layer-2 coalescing
// described in doc/design/signal_handling.md. (count is a lower bound in general —
// see that doc — but a same-thread, handler-returns-immediately burst like this one
// is not expected to suffer layer-1 kernel coalescing in practice.)
TYPED_TEST(SignalStreamTest, CoalescesBurstIntoOneEventWithCount) {
    std::optional<SignalEvent> got;
    this->traits.rt.block_on([](std::optional<SignalEvent>& got) -> Coro<void> {
        auto sigs = coro::signal_stream({SIGUSR1});
        co_await with_context(current_uv_executor(), []() -> Coro<void> {
            ::raise(SIGUSR1);
            ::raise(SIGUSR1);
            ::raise(SIGUSR1);
            co_return;
        }());
        got = co_await next(sigs);
    }(got));
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->signum, SIGUSR1);
    EXPECT_GE(got->count, 1u);
}

TYPED_TEST(SignalStreamTest, DistinctSignalsYieldSeparateEvents) {
    std::optional<SignalEvent> first, second;
    this->traits.rt.block_on([](std::optional<SignalEvent>& first, std::optional<SignalEvent>& second) -> Coro<void> {
        auto sigs = coro::signal_stream({SIGUSR1, SIGUSR2});
        co_await with_context(current_uv_executor(), []() -> Coro<void> {
            ::raise(SIGUSR1);
            ::raise(SIGUSR2);
            co_return;
        }());
        first = co_await next(sigs);
        second = co_await next(sigs);
    }(first, second));
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    EXPECT_NE(first->signum, second->signum);
    EXPECT_TRUE((first->signum == SIGUSR1 && second->signum == SIGUSR2) ||
                (first->signum == SIGUSR2 && second->signum == SIGUSR1));
}

TYPED_TEST(SignalStreamTest, DroppingBeforeDeliveryDoesNotHang) {
    this->traits.rt.block_on([]() -> Coro<void> {
        { auto sigs = coro::signal_stream({SIGUSR1}); }
        co_await yield_to_uv_thread(current_uv_executor());
    }());
}
