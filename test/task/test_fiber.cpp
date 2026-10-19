#include <coro/coro.h>
#include <coro/co_invoke.h>
#include <coro/runtime/runtime.h>
#include <coro/sync/event.h>
#include <coro/sync/sleep.h>
#include <coro/task/fiber.h>
#include <coro/task/yield_now.h>
#include "executor_traits.h"
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <string>

using namespace std::chrono_literals;

namespace {
#ifdef CORO_PICO
// Every fiber stack comes out of a heap of under 200 KB. 8 KB leaves room for the
// exception unwinder, which PropagatesException runs on the fiber's own stack.
constexpr size_t kStackSize = 8 * 1024;
#else
constexpr size_t kStackSize = 64 * 1024;
#endif

// Minimal Waker for driving FiberFuture<T>::poll() directly, without a Runtime.
struct NoOpWaker : coro::detail::Waker {
    void wake() override {}
    coro::detail::Rc<coro::detail::Waker> clone() override { return coro::detail::make_rc<NoOpWaker>(); }
};

coro::detail::Context make_ctx() {
    return coro::detail::Context(coro::detail::make_rc<NoOpWaker>());
}

struct FailingFuture {
    using OutputType = int;
    coro::PollResult<int> poll(coro::detail::Context&) {
        return coro::PollError(std::make_exception_ptr(std::runtime_error("inner boom")));
    }
};

// Reports PollDropped, as a handle to a cancelled task does.
template<typename T>
struct DroppedFuture {
    using OutputType = T;
    coro::PollResult<T> poll(coro::detail::Context&) { return coro::PollDropped; }
};
} // namespace

// ---------------------------------------------------------------------------
// FiberFuture<T> polled directly (no Runtime) — basic completion, no yield
// ---------------------------------------------------------------------------

TEST(Fiber, RunsToCompletionOnFirstPoll) {
    coro::FiberFuture<int> ff([] { return 42; }, kStackSize);
    auto ctx = make_ctx();
    auto r = ff.poll(ctx);
    ASSERT_TRUE(r.isReady());
    EXPECT_EQ(r.value(), 42);
}

TEST(Fiber, RunsToCompletionVoid) {
    bool ran = false;
    coro::FiberFuture<void> ff([&] { ran = true; }, kStackSize);
    auto ctx = make_ctx();
    auto r = ff.poll(ctx);
    ASSERT_TRUE(r.isReady());
    EXPECT_TRUE(ran);
}

TEST(Fiber, PropagatesException) {
    coro::FiberFuture<int> ff([]() -> int { throw std::runtime_error("boom"); }, kStackSize);
    auto ctx = make_ctx();
    auto r = ff.poll(ctx);
    ASSERT_TRUE(r.isError());
    EXPECT_THROW(std::rethrow_exception(r.error()), std::runtime_error);
}

// ---------------------------------------------------------------------------
// detail::fiber_yield() — explicit multi-poll suspension (white-box: the
// public API only exposes fiber_await(), which always pairs a yield with a
// waker registration; fiber_yield() alone has no such guarantee, hence detail::)
// ---------------------------------------------------------------------------

TEST(Fiber, YieldSuspendsUntilNextPoll) {
    int stage = 0;
    coro::FiberFuture<int> ff([&] {
        stage = 1;
        coro::detail::fiber_yield();
        stage = 2;
        coro::detail::fiber_yield();
        stage = 3;
        return 7;
    }, kStackSize);

    auto ctx = make_ctx();

    auto r1 = ff.poll(ctx);
    EXPECT_TRUE(r1.isPending());
    EXPECT_EQ(stage, 1);

    auto r2 = ff.poll(ctx);
    EXPECT_TRUE(r2.isPending());
    EXPECT_EQ(stage, 2);

    auto r3 = ff.poll(ctx);
    ASSERT_TRUE(r3.isReady());
    EXPECT_EQ(stage, 3);
    EXPECT_EQ(r3.value(), 7);
}

TEST(Fiber, YieldOutsideFiberThrows) {
    EXPECT_THROW(coro::detail::fiber_yield(), std::logic_error);
}

TEST(Fiber, FiberAwaitOutsideFiberThrows) {
    EXPECT_THROW(coro::fiber_await(coro::sleep_for(1ms)), std::logic_error);
}

// ---------------------------------------------------------------------------
// fiber_await() of a future that fails or is dropped — the error is thrown inside
// the fiber, where its body can catch it.
// ---------------------------------------------------------------------------

TEST(Fiber, FiberAwaitRethrowsInnerError) {
    coro::FiberFuture<int> ff([]() -> int {
        try {
            return coro::fiber_await(FailingFuture{});
        } catch (const std::runtime_error&) {
            return -1;
        }
    }, kStackSize);
    auto ctx = make_ctx();
    auto r = ff.poll(ctx);
    ASSERT_TRUE(r.isReady());
    EXPECT_EQ(r.value(), -1);
}

TEST(Fiber, FiberAwaitOfDroppedFutureThrowsTaskCancelled) {
    coro::FiberFuture<int> ff([]() -> int {
        try {
            return coro::fiber_await(DroppedFuture<int>{});
        } catch (const coro::TaskCancelled&) {
            return -1;
        }
    }, kStackSize);
    auto ctx = make_ctx();
    auto r = ff.poll(ctx);
    ASSERT_TRUE(r.isReady());
    EXPECT_EQ(r.value(), -1);
}

// A void await has no value to miss, but carrying on would act as if the work had been done.
TEST(Fiber, VoidFiberAwaitOfDroppedFutureThrowsTaskCancelled) {
    bool carried_on = false;
    bool caught     = false;
    coro::FiberFuture<void> ff([&] {
        try {
            coro::fiber_await(DroppedFuture<void>{});
            carried_on = true;
        } catch (const coro::TaskCancelled&) {
            caught = true;
        }
    }, kStackSize);
    auto ctx = make_ctx();
    ASSERT_TRUE(ff.poll(ctx).isReady());
    EXPECT_TRUE(caught);
    EXPECT_FALSE(carried_on);
}

// ---------------------------------------------------------------------------
// fiber_await() bridging a real coro::Future — driven by a Runtime so wakeups
// are event-driven rather than polled on a fixed cadence.
//
// TODO: run these over every executor (TYPED_TEST_SUITE with AllExecutors), as the
// other task tests do. They use CurrentThreadTraits only for now: on a multi-threaded
// executor a fiber can resume on another worker, and some may fail intermittently (see
// "Thread-locals and worker migration" in doc/design/fiber.md).
//
// The runtime comes from CurrentThreadTraits (executor_traits.h): one thread on
// the desktop, and on Pico a runtime that leaves the Wi-Fi driver alone.
// ---------------------------------------------------------------------------

TEST(Fiber, SpawnFiberRunsAndCompletes) {
    CurrentThreadTraits traits;
    coro::Runtime& rt = traits.rt;
    std::atomic<bool> ran{false};

    rt.block_on([&]() -> coro::Coro<void> {
        [[maybe_unused]] auto h = coro::spawn_fiber<void>([&] { ran.store(true); }, kStackSize);
        co_await coro::sleep_for(20ms);
    }());

    EXPECT_TRUE(ran.load());
}

TEST(Fiber, FiberAwaitBridgesEventWakeup) {
    CurrentThreadTraits traits;
    coro::Runtime& rt = traits.rt;
    coro::Event ev;
    std::atomic<bool> resumed{false};

    rt.block_on([&]() -> coro::Coro<void> {
        [[maybe_unused]] auto h = coro::spawn_fiber<void>([&] {
            coro::fiber_await(ev.wait());
            resumed.store(true);
        }, kStackSize);

        // Give the fiber a chance to run up to fiber_await()'s first Pending poll.
        co_await coro::sleep_for(5ms);
        EXPECT_FALSE(resumed.load());

        ev.set();
        co_await coro::sleep_for(20ms);
    }());

    EXPECT_TRUE(resumed.load());
}

TEST(Fiber, FiberAwaitLoopMultipleWaits) {
    CurrentThreadTraits traits;
    coro::Runtime& rt = traits.rt;
    std::atomic<int> count{0};

    rt.block_on([&]() -> coro::Coro<void> {
        [[maybe_unused]] auto h = coro::spawn_fiber<void>([&] {
            for (int i = 0; i < 3; ++i) {
                coro::fiber_await(coro::sleep_for(1ms));
                count.fetch_add(1);
            }
        }, kStackSize);

        co_await coro::sleep_for(50ms);
    }());

    EXPECT_EQ(count.load(), 3);
}

// ---------------------------------------------------------------------------
// FiberHandle<T> — joining a fiber's result, and dropping without joining.
// ---------------------------------------------------------------------------

TEST(Fiber, HandleCanBeAwaitedForResult) {
    CurrentThreadTraits traits;
    coro::Runtime& rt = traits.rt;
    int result = 0;

    rt.block_on([&]() -> coro::Coro<void> {
        auto h = coro::spawn_fiber<int>([] {
            coro::fiber_await(coro::sleep_for(1ms));
            return 99;
        }, kStackSize);
        result = co_await std::move(h);
    }());

    EXPECT_EQ(result, 99);
}

TEST(Fiber, HandleDroppedWithoutJoinKeepsRunningToCompletion) {
    CurrentThreadTraits traits;
    coro::Runtime& rt = traits.rt;
    std::atomic<bool> ran{false};

    rt.block_on([&]() -> coro::Coro<void> {
        {
            [[maybe_unused]] auto h = coro::spawn_fiber<void>([&] {
                coro::fiber_await(coro::sleep_for(5ms));
                ran.store(true);
            }, kStackSize);
            // h drops here without being awaited -- must detach, not cancel.
        }
        co_await coro::sleep_for(30ms);
    }());

    EXPECT_TRUE(ran.load());
}

TEST(Fiber, HandleRethrowsFiberException) {
    CurrentThreadTraits traits;
    coro::Runtime& rt = traits.rt;

    bool caught = rt.block_on([]() -> coro::Coro<bool> {
        auto h = coro::spawn_fiber<int>([]() -> int {
            coro::fiber_await(coro::sleep_for(1ms));
            throw std::runtime_error("boom");
        }, kStackSize);
        try {
            (void)co_await std::move(h);
        } catch (const std::runtime_error&) {
            co_return true;
        }
        co_return false;
    }());

    EXPECT_TRUE(caught);
}

// Leaving the scope shuts the runtime down with a detached fiber still parked in
// fiber_await(). It must neither hang nor crash.
//
// The fiber's stack is freed, not unwound (the FIXME in doc/design/fiber.md, "a fiber
// still suspended at runtime shutdown is freed, not unwound"), so it waits on a future
// that registers nothing: one parked on an Event would leave the Event a dangling pointer.
TEST(Fiber, RuntimeShutdownWithParkedFiberDoesNotHang) {
    std::atomic<bool> parked{false};
    {
        CurrentThreadTraits traits;
        traits.rt.block_on([](std::atomic<bool>& parked) -> coro::Coro<void> {
            [[maybe_unused]] auto h = coro::spawn_fiber<int>([&parked] {
                parked.store(true);
                return coro::fiber_await(coro::never<int>());
            }, kStackSize);
            while (!parked.load()) co_await coro::yield_now();
        }(parked));
    }
    EXPECT_TRUE(parked.load());
}

// ---------------------------------------------------------------------------
// Stack overflow — guard page below the usable stack region reliably faults
// rather than silently corrupting adjacent memory.
//
// Desktop only. A microcontroller has no guard page and no second process to die
// in: the Pico backend checks a canary word at the next context switch and then
// halts the board (coro_fiber_stack_overflow), which would end the whole run.
// ---------------------------------------------------------------------------

#ifndef CORO_PICO
TEST(FiberDeathTest, StackOverflowFaultsOnGuardPage) {
    EXPECT_DEATH(
        {
            // Deliberately tiny stack; recurse until it overflows into the guard page.
            coro::FiberFuture<void> ff([]() {
                // Recursion is deliberately not in tail position (the '+ pad[0]'
                // after the recursive call) so the compiler can't turn this into
                // a constant-stack loop -- it needs a real, growing stack frame
                // per call for the guard page to ever be reached.
                struct Recur { static int go(int n) {
                    volatile char pad[512];
                    std::memset(const_cast<char*>(pad), 0, sizeof(pad));
                    int r = go(n + 1);
                    return r + pad[0];
                } };
                Recur::go(0);
            }, 4096);
            auto ctx = make_ctx();
            ff.poll(ctx);
        },
        "");
}
#endif  // CORO_PICO
