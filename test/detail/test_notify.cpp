// sys/notify.h, the driver's eventfd analog, on every backend: an eventfd on the
// desktop, an async_context worker in the Pico configuration (on the host against
// the stub async_context, and on the board). The driver is turned directly from the
// test thread. See doc/design/io_driver.md, "The sys layer".
//
// Also the driver's waiter list, on a notify fd: the one fd type every backend has,
// and the one the ISR primitives wait on.

#include <gtest/gtest.h>
#include <coro/detail/context.h>
#include <coro/detail/rc.h>
#include <coro/detail/sys/notify.h>
#include <coro/detail/sys/socket.h>
#include <coro/detail/waker.h>
#include <coro/runtime/clock.h>
#include <coro/runtime/io_driver.h>
#include "executor_traits.h"
#include "isr_trigger.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <expected>
#include <memory>
#include <optional>
#include <vector>

using namespace coro;
using namespace std::chrono_literals;

namespace {

class CountingWaker final : public detail::Waker {
public:
    void wake() override { m_count.fetch_add(1, std::memory_order_relaxed); }
    // An owning Context hands out the waker itself; clone() is never called.
    detail::Rc<detail::Waker> clone() override { return nullptr; }
    int count() const { return m_count.load(std::memory_order_relaxed); }
private:
    std::atomic<int> m_count{0};
};

struct TestTask {
    detail::Rc<CountingWaker> waker = detail::make_rc<CountingWaker>();
    detail::Context ctx{waker};
    int wakes() const { return waker->count(); }
};

// A driver to turn by hand: its own on the desktop; on Pico the one a Runtime owns,
// which brings the async_context it runs on.
struct DriverHarness {
#ifdef CORO_PICO
    SingleThreadRuntime rt;
    IoDriver& driver = rt.io_driver();
#else
    IoDriver driver;
#endif
};

// A notify object registered with the driver; deregistered, then closed.
struct RegisteredNotify {
    detail::sys::RawFd fd;
    IoRegistration     reg;

    explicit RegisteredNotify(IoDriver& driver)
        : fd(detail::sys::notify_open()), reg(driver, fd, detail::sys::Interest::read()) {}
    ~RegisteredNotify() {
        reg.deregister();
        detail::sys::close_socket(fd);
    }
};

// Leaves the read direction not ready, as a failed operation would.
void observe_eagain(IoRegistration& reg, TestTask& task) {
    auto ready = reg.poll_ready(IoDirection::Read, task.ctx);
    ASSERT_TRUE(ready.has_value());
    reg.clear_ready(*ready);
    ASSERT_FALSE(reg.poll_ready(IoDirection::Read, task.ctx).has_value());
}

} // namespace

TEST(Notify, SignalMakesReadable) {
    DriverHarness h;
    RegisteredNotify n(h.driver);
    TestTask task;
    observe_eagain(n.reg, task);

    detail::sys::notify_signal(n.fd);
    EXPECT_GE(h.driver.turn(1s), 1u);
    EXPECT_EQ(task.wakes(), 1);
    EXPECT_TRUE(n.reg.poll_ready(IoDirection::Read, task.ctx).has_value());
}

TEST(Notify, SignalsAreEdges) {
    // Nothing is read back between signals, and each one is reported.
    DriverHarness h;
    RegisteredNotify n(h.driver);
    TestTask task;
    for (int i = 1; i <= 3; ++i) {
        observe_eagain(n.reg, task);
        detail::sys::notify_signal(n.fd);
        EXPECT_GE(h.driver.turn(1s), 1u);
        EXPECT_EQ(task.wakes(), i);
    }
}

TEST(Notify, NoSignalNoEvent) {
    DriverHarness h;
    RegisteredNotify n(h.driver);
    TestTask task;
    observe_eagain(n.reg, task);
    h.driver.turn(0ns);
    EXPECT_EQ(task.wakes(), 0);
}

TEST(Notify, SignalBeforeRegistrationIsHarmless) {
    // On lwIP a signal with no registration is dropped; on POSIX the eventfd keeps
    // it. Either way a new registration starts ready, so nothing is missed.
    DriverHarness h;
    const detail::sys::RawFd fd = detail::sys::notify_open();
    detail::sys::notify_signal(fd);
    {
        IoRegistration reg(h.driver, fd, detail::sys::Interest::read());
        TestTask task;
        EXPECT_TRUE(reg.poll_ready(IoDirection::Read, task.ctx).has_value());
        h.driver.turn(0ns);
        reg.deregister();
    }
    detail::sys::close_socket(fd);
}

TEST(Notify, SignalFromIsrEndsASleepingTurn) {
    // On the board the signal comes from an interrupt and marks the worker pending,
    // which releases the async_context's semaphore and ends the driver's sleep. On
    // the host IsrTrigger's thread stands in for the interrupt.
    DriverHarness h;
    RegisteredNotify n(h.driver);
    TestTask task;
    observe_eagain(n.reg, task);

    const Instant start = Clock::now();
    {
        IsrTrigger isr(20ms, 1, [fd = n.fd](int) { detail::sys::notify_signal(fd); });
        while (task.wakes() == 0 && Clock::now() - start < 5s)
            h.driver.turn(5s);
    }
    EXPECT_EQ(task.wakes(), 1);
    EXPECT_LT(Clock::now() - start, 4s);
}

TEST(Notify, TurnTimesOutWhenIdle) {
    // test_io_driver.cpp's TurnTimesOutWithNoEvents, on every backend. The first turn
    // takes any wake left over from building the runtime, so the second sleeps.
    DriverHarness h;
    h.driver.turn(20ms);
    const Instant start = Clock::now();
    EXPECT_EQ(h.driver.turn(20ms), 0u);
    const auto elapsed = Clock::now() - start;
    EXPECT_GE(elapsed, 20ms);
    EXPECT_LT(elapsed, 500ms);
}

TEST(Notify, SignalWhileDeregisteredLeavesAtMostOneEvent) {
    // On lwIP the signal is dropped, or, if it races the deregistration, leaves the
    // worker pending, to run once when it is attached again. On POSIX the eventfd
    // keeps it. Either way the next registration reports it at most once, and the
    // next signal still wakes it.
    DriverHarness h;
    const detail::sys::RawFd fd = detail::sys::notify_open();
    {
        IoRegistration reg(h.driver, fd, detail::sys::Interest::read());
        h.driver.turn(0ns);
        reg.deregister();
    }
    detail::sys::notify_signal(fd);
    {
        IoRegistration reg(h.driver, fd, detail::sys::Interest::read());
        TestTask task;
        observe_eagain(reg, task);
        h.driver.turn(0ns);   // whatever the stale signal left
        EXPECT_LE(task.wakes(), 1);
        if (auto ready = reg.poll_ready(IoDirection::Read, task.ctx)) reg.clear_ready(*ready);
        ASSERT_FALSE(reg.poll_ready(IoDirection::Read, task.ctx).has_value());

        const int stale = task.wakes();
        h.driver.turn(0ns);
        EXPECT_EQ(task.wakes(), stale);
        detail::sys::notify_signal(fd);
        EXPECT_GE(h.driver.turn(1s), 1u);
        EXPECT_EQ(task.wakes(), stale + 1);
        reg.deregister();
    }
    detail::sys::close_socket(fd);
}

// ---------------------------------------------------------------------------
// The waiter list: any number of waiters on one direction. See
// doc/design/io_driver.md, "More than one waiter".
// ---------------------------------------------------------------------------

namespace {

// Makes the read direction not ready and links `waiter`, as a failed operation and
// the following poll would.
void link_waiter(IoRegistration& reg, IoWaiter& waiter, TestTask& task) {
    auto ready = reg.poll_ready(IoDirection::Read, task.ctx, waiter);
    if (ready) {
        reg.clear_ready(*ready);
        ready = reg.poll_ready(IoDirection::Read, task.ctx, waiter);
    }
    ASSERT_FALSE(ready.has_value());
    ASSERT_TRUE(waiter.linked());
}

} // namespace

TEST(IoWaiter, EveryListedWaiterIsWoken) {
    DriverHarness h;
    RegisteredNotify n(h.driver);
    TestTask tasks[3];
    IoWaiter waiters[3];
    for (int i = 0; i < 3; ++i) link_waiter(n.reg, waiters[i], tasks[i]);

    detail::sys::notify_signal(n.fd);
    EXPECT_GE(h.driver.turn(1s), 1u);
    for (int i = 0; i < 3; ++i) {
        EXPECT_EQ(tasks[i].wakes(), 1);
        EXPECT_FALSE(waiters[i].linked());
        EXPECT_TRUE(n.reg.poll_ready(IoDirection::Read, tasks[i].ctx, waiters[i]).has_value());
    }
}

TEST(IoWaiter, LateWaiterClearingReadinessLosesNoWake) {
    DriverHarness h;
    RegisteredNotify n(h.driver);
    TestTask a, b, late;
    IoWaiter wa, wb, wlate;
    link_waiter(n.reg, wa, a);
    link_waiter(n.reg, wb, b);

    detail::sys::notify_signal(n.fd);
    EXPECT_GE(h.driver.turn(1s), 1u);
    ASSERT_EQ(a.wakes(), 1);
    ASSERT_EQ(b.wakes(), 1);

    // A waiter that arrived after the event runs first: it sees the direction
    // ready, its operation finds nothing for it, and it clears readiness.
    auto ready = n.reg.poll_ready(IoDirection::Read, late.ctx, wlate);
    ASSERT_TRUE(ready.has_value());
    n.reg.clear_ready(*ready);

    // The waiters the event woke still return ready, once each.
    EXPECT_TRUE(n.reg.poll_ready(IoDirection::Read, a.ctx, wa).has_value());
    EXPECT_TRUE(n.reg.poll_ready(IoDirection::Read, b.ctx, wb).has_value());
    // And then wait again like anyone else.
    EXPECT_FALSE(n.reg.poll_ready(IoDirection::Read, a.ctx, wa).has_value());
    n.reg.cancel_wait(wa);
    n.reg.cancel_wait(wb);
    n.reg.cancel_wait(wlate);
}

TEST(IoWaiter, DroppedWaiterIsUnlinked) {
    DriverHarness h;
    RegisteredNotify n(h.driver);
    TestTask kept, dropped;
    IoWaiter wk, wd;
    link_waiter(n.reg, wk, kept);
    link_waiter(n.reg, wd, dropped);

    n.reg.cancel_wait(wd);
    EXPECT_FALSE(wd.linked());
    detail::sys::notify_signal(n.fd);
    EXPECT_GE(h.driver.turn(1s), 1u);
    EXPECT_EQ(kept.wakes(), 1);
    EXPECT_EQ(dropped.wakes(), 0);
    n.reg.cancel_wait(wk);
}

TEST(IoWaiter, MoreThan32WaitersAreAllWoken) {
    // dispatch() wakes listed waiters in batches of IoDriver's kWakeBatch (32).
    constexpr int kWaiters = 75;
    DriverHarness h;
    RegisteredNotify n(h.driver);
    std::vector<std::unique_ptr<TestTask>> tasks;
    std::vector<IoWaiter> waiters(kWaiters);   // sized once: linked waiters never move
    for (int i = 0; i < kWaiters; ++i) {
        tasks.push_back(std::make_unique<TestTask>());
        link_waiter(n.reg, waiters[i], *tasks[i]);
    }

    detail::sys::notify_signal(n.fd);
    EXPECT_GE(h.driver.turn(1s), 1u);
    for (int i = 0; i < kWaiters; ++i) {
        EXPECT_EQ(tasks[i]->wakes(), 1) << "waiter " << i;
        EXPECT_FALSE(waiters[i].linked());
    }
}

TEST(IoWaiter, PollIoWithWaiterRunsTheOperationOnWake) {
    // The operation claims state the signaller set, as an ISR primitive's does: a
    // notify fd has nothing to read.
    DriverHarness h;
    RegisteredNotify n(h.driver);
    TestTask task;
    IoWaiter waiter;
    bool signalled = false;
    int  claims    = 0;
    auto claim = [&]() -> std::expected<std::size_t, int> {
        if (!signalled) return std::unexpected(EAGAIN);
        ++claims;
        return std::size_t{1};
    };

    EXPECT_FALSE(n.reg.poll_io(IoDirection::Read, task.ctx, waiter, claim).has_value());
    EXPECT_TRUE(waiter.linked());
    signalled = true;
    detail::sys::notify_signal(n.fd);
    EXPECT_GE(h.driver.turn(1s), 1u);
    EXPECT_EQ(task.wakes(), 1);
    auto result = n.reg.poll_io(IoDirection::Read, task.ctx, waiter, claim);
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->has_value());
    EXPECT_EQ(**result, 1u);
    EXPECT_EQ(claims, 1);
}

TEST(IoWaiter, ShutdownWakesListedWaiters) {
    DriverHarness h;
    RegisteredNotify n(h.driver);
    TestTask task;
    IoWaiter waiter;
    link_waiter(n.reg, waiter, task);

    h.driver.shutdown();
    EXPECT_EQ(task.wakes(), 1);
    EXPECT_FALSE(waiter.linked());
    auto ready = n.reg.poll_ready(IoDirection::Read, task.ctx, waiter);
    ASSERT_TRUE(ready.has_value());
    EXPECT_TRUE(ready->shutdown);
}
