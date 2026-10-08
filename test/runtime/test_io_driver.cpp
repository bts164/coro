// Tests for coro::IoDriver / IoRegistration — the readiness handshake and dispatch.
// The driver is turned directly from the test thread; no executor is involved.
// See doc/design/io_driver.md.

#include <gtest/gtest.h>
#include <coro/runtime/io_driver.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <expected>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

using namespace coro;
using namespace std::chrono_literals;

namespace {

class CountingWaker : public detail::Waker,
                      public std::enable_shared_from_this<CountingWaker> {
public:
    void wake() override { count.fetch_add(1, std::memory_order_relaxed); }
    detail::Rc<detail::Waker> clone() override { return shared_from_this(); }
    int value() const { return count.load(std::memory_order_relaxed); }
private:
    std::atomic<int> count{0};
};

// A Context plus the waker it carries, kept alive for the test's duration
// (the registration only stores a weak reference).
struct TestTask {
    std::shared_ptr<CountingWaker> waker = std::make_shared<CountingWaker>();
    detail::Context ctx{waker};
    int wakes() const { return waker->value(); }
};

struct SocketPair {
    int a = -1;
    int b = -1;
    SocketPair() {
        int fds[2];
        EXPECT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds), 0);
        a = fds[0];
        b = fds[1];
    }
    ~SocketPair() {
        if (a >= 0) ::close(a);
        if (b >= 0) ::close(b);
    }
};

struct UdpLoopback {
    int fd = -1;
    sockaddr_in addr{};
    UdpLoopback() {
        fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        EXPECT_GE(fd, 0);
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        EXPECT_EQ(::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
        socklen_t len = sizeof(addr);
        EXPECT_EQ(::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len), 0);
    }
    ~UdpLoopback() { if (fd >= 0) ::close(fd); }
};

void write_byte(int fd) {
    char c = 'x';
    ASSERT_EQ(::write(fd, &c, 1), 1);
}

// Simulates "the syscall returned EAGAIN": clears readiness for the observed tick.
void observe_eagain(IoRegistration& reg, IoDirection dir, TestTask& task) {
    auto ready = reg.poll_ready(dir, task.ctx);
    ASSERT_TRUE(ready.has_value());
    reg.clear_ready(*ready);
}

} // namespace

TEST(IoRegistration, DefaultConstructedIsEmpty) {
    IoRegistration reg;
    EXPECT_FALSE(reg);
    reg.deregister();  // no-op
}

TEST(IoRegistration, InitiallyReadyInBothDirections) {
    IoDriver driver;
    SocketPair sp;
    IoRegistration reg(driver, sp.a);
    TestTask task;

    EXPECT_TRUE(reg);
    EXPECT_TRUE(reg.poll_ready(IoDirection::Read, task.ctx).has_value());
    EXPECT_TRUE(reg.poll_ready(IoDirection::Write, task.ctx).has_value());
}

TEST(IoRegistration, ClearReadyMakesDirectionPending) {
    IoDriver driver;
    SocketPair sp;
    IoRegistration reg(driver, sp.a);
    TestTask task;

    observe_eagain(reg, IoDirection::Read, task);
    EXPECT_FALSE(reg.poll_ready(IoDirection::Read, task.ctx).has_value());
    // The other direction is unaffected.
    EXPECT_TRUE(reg.poll_ready(IoDirection::Write, task.ctx).has_value());
}

TEST(IoDriver, ReadableEventWakesReader) {
    IoDriver driver;
    SocketPair sp;
    IoRegistration reg(driver, sp.a);
    TestTask task;

    observe_eagain(reg, IoDirection::Read, task);
    ASSERT_FALSE(reg.poll_ready(IoDirection::Read, task.ctx).has_value());

    write_byte(sp.b);
    EXPECT_GE(driver.turn(1s), 1u);
    EXPECT_EQ(task.wakes(), 1);
    EXPECT_TRUE(reg.poll_ready(IoDirection::Read, task.ctx).has_value());
}

TEST(IoDriver, DirectionsAreWokenIndependently) {
    IoDriver driver;
    SocketPair sp;
    IoRegistration reg(driver, sp.a);
    TestTask reader;
    TestTask writer;

    observe_eagain(reg, IoDirection::Read, reader);
    observe_eagain(reg, IoDirection::Write, writer);
    ASSERT_FALSE(reg.poll_ready(IoDirection::Read, reader.ctx).has_value());
    ASSERT_FALSE(reg.poll_ready(IoDirection::Write, writer.ctx).has_value());

    // The registration's initial edge reports writable (the socket buffer is empty)
    // but not readable (no data yet).
    driver.turn(1s);
    EXPECT_EQ(writer.wakes(), 1);
    EXPECT_EQ(reader.wakes(), 0);

    write_byte(sp.b);
    driver.turn(1s);
    EXPECT_EQ(reader.wakes(), 1);
}

// The core lost-wakeup race: readiness arrives between the failed syscall and
// clear_ready(). The tick check must keep the new readiness.
TEST(IoDriver, EventBetweenEagainAndClearIsNotLost) {
    IoDriver driver;
    SocketPair sp;
    IoRegistration reg(driver, sp.a);
    TestTask task;

    // Settle the registration's initial edge first.
    driver.turn(0ns);

    auto ready = reg.poll_ready(IoDirection::Read, task.ctx);
    ASSERT_TRUE(ready.has_value());
    // ... syscall returns EAGAIN here; then data arrives and the driver runs:
    write_byte(sp.b);
    driver.turn(1s);
    // ... and only now does the future get to clear readiness.
    reg.clear_ready(*ready);

    EXPECT_TRUE(reg.poll_ready(IoDirection::Read, task.ctx).has_value());
}

TEST(IoDriver, WakerFiresOncePerWait) {
    IoDriver driver;
    SocketPair sp;
    IoRegistration reg(driver, sp.a);
    TestTask task;

    observe_eagain(reg, IoDirection::Read, task);
    ASSERT_FALSE(reg.poll_ready(IoDirection::Read, task.ctx).has_value());

    write_byte(sp.b);
    driver.turn(1s);
    write_byte(sp.b);
    driver.turn(1s);
    // The second event finds no stored waker: the task hasn't waited again.
    EXPECT_EQ(task.wakes(), 1);
}

TEST(IoDriver, LaterPollReplacesStoredWaker) {
    IoDriver driver;
    SocketPair sp;
    IoRegistration reg(driver, sp.a);
    TestTask first;
    TestTask second;

    observe_eagain(reg, IoDirection::Read, first);
    ASSERT_FALSE(reg.poll_ready(IoDirection::Read, first.ctx).has_value());
    ASSERT_FALSE(reg.poll_ready(IoDirection::Read, second.ctx).has_value());

    write_byte(sp.b);
    driver.turn(1s);
    EXPECT_EQ(first.wakes(), 0);
    EXPECT_EQ(second.wakes(), 1);
}

// Dropping the waiting task (e.g. a cancelled future) leaves only an expired weak
// waker behind; dispatch must tolerate it.
TEST(IoDriver, ExpiredWakerIsIgnored) {
    IoDriver driver;
    SocketPair sp;
    IoRegistration reg(driver, sp.a);
    {
        TestTask task;
        observe_eagain(reg, IoDirection::Read, task);
        ASSERT_FALSE(reg.poll_ready(IoDirection::Read, task.ctx).has_value());
    }
    write_byte(sp.b);
    EXPECT_GE(driver.turn(1s), 1u);

    TestTask task;
    EXPECT_TRUE(reg.poll_ready(IoDirection::Read, task.ctx).has_value());
}

TEST(IoDriver, DeregisteredFdNoLongerWakes) {
    IoDriver driver;
    SocketPair sp;
    TestTask task;
    {
        IoRegistration reg(driver, sp.a);
        driver.turn(0ns);  // settle the initial edge
        observe_eagain(reg, IoDirection::Read, task);
        ASSERT_FALSE(reg.poll_ready(IoDirection::Read, task.ctx).has_value());
    }  // deregistered here; the fd stays open

    write_byte(sp.b);
    EXPECT_EQ(driver.turn(20ms), 0u);
    EXPECT_EQ(task.wakes(), 0);
}

// An event fetched for a registration that is then dropped (before dispatch, on
// another thread) must not touch freed memory. Approximated deterministically: drop
// the registration while its event is queued in the kernel, then turn repeatedly.
// Run under ASan to make this meaningful.
TEST(IoDriver, DropWithQueuedEventIsSafe) {
    IoDriver driver;
    SocketPair sp;
    {
        IoRegistration reg(driver, sp.a);
        write_byte(sp.b);
    }
    driver.turn(0ns);
    driver.turn(0ns);
}

TEST(IoDriver, ExplicitDeregisterIsIdempotent) {
    IoDriver driver;
    SocketPair sp;
    IoRegistration reg(driver, sp.a);
    reg.deregister();
    EXPECT_FALSE(reg);
    reg.deregister();
}

TEST(IoDriver, MovedRegistrationKeepsWorking) {
    IoDriver driver;
    SocketPair sp;
    IoRegistration original(driver, sp.a);
    IoRegistration reg(std::move(original));
    EXPECT_FALSE(original);
    ASSERT_TRUE(reg);

    TestTask task;
    observe_eagain(reg, IoDirection::Read, task);
    ASSERT_FALSE(reg.poll_ready(IoDirection::Read, task.ctx).has_value());
    write_byte(sp.b);
    driver.turn(1s);
    EXPECT_EQ(task.wakes(), 1);
}

TEST(IoDriver, MoveAssignDeregistersPrevious) {
    IoDriver driver;
    SocketPair first;
    SocketPair second;
    TestTask task;

    IoRegistration reg(driver, first.a);
    driver.turn(0ns);
    observe_eagain(reg, IoDirection::Read, task);
    ASSERT_FALSE(reg.poll_ready(IoDirection::Read, task.ctx).has_value());

    reg = IoRegistration(driver, second.a);
    write_byte(first.b);       // the old fd: must not wake anything
    EXPECT_EQ(driver.turn(20ms), 1u);  // only the new registration's initial edge
    EXPECT_EQ(task.wakes(), 0);
}

TEST(IoDriver, TurnTimesOutWithNoEvents) {
    IoDriver driver;
    const auto start = std::chrono::steady_clock::now();
    EXPECT_EQ(driver.turn(20ms), 0u);
    EXPECT_GE(std::chrono::steady_clock::now() - start, 20ms);
}

TEST(IoDriver, UnparkWakesBlockedTurn) {
    IoDriver driver;
    std::thread t([&driver] {
        std::this_thread::sleep_for(50ms);
        driver.unpark();
    });
    const auto start = std::chrono::steady_clock::now();
    EXPECT_EQ(driver.turn(std::nullopt), 0u);  // unpark isn't counted as an I/O event
    EXPECT_GE(std::chrono::steady_clock::now() - start, 40ms);
    t.join();
}

TEST(IoDriver, UnparkBeforeTurnIsNotLost) {
    IoDriver driver;
    driver.unpark();
    const auto start = std::chrono::steady_clock::now();
    driver.turn(std::nullopt);
    EXPECT_LT(std::chrono::steady_clock::now() - start, 1s);
}

TEST(IoDriver, UnparkIsConsumedByTurn) {
    IoDriver driver;
    driver.unpark();
    driver.unpark();
    driver.turn(1s);
    const auto start = std::chrono::steady_clock::now();
    driver.turn(20ms);
    EXPECT_GE(std::chrono::steady_clock::now() - start, 20ms);
}

// Dropping registrations from another thread, as many as `count`, while turn() is
// blocked. Returns how long that turn took.
static std::chrono::steady_clock::duration
blocked_turn_while_dropping(std::size_t count, std::chrono::milliseconds timeout) {
    IoDriver driver;
    std::vector<std::unique_ptr<SocketPair>> pairs;
    std::vector<IoRegistration> regs;
    for (std::size_t i = 0; i < count; ++i) {
        pairs.push_back(std::make_unique<SocketPair>());
        regs.emplace_back(driver, pairs.back()->a);
    }
    driver.turn(0ns);  // settle the initial edges

    std::thread t([&regs] {
        std::this_thread::sleep_for(50ms);
        regs.clear();  // deregisters each one; the fds stay open until `pairs` goes
    });
    const auto start = std::chrono::steady_clock::now();
    driver.turn(timeout);
    const auto took = std::chrono::steady_clock::now() - start;
    t.join();
    return took;
}

// The 16th pending release unparks the driver, so they don't wait for an unrelated
// event (IoDriver::kReleaseUnparkThreshold).
TEST(IoDriver, ManyDeregistrationsUnparkBlockedTurn) {
    EXPECT_LT(blocked_turn_while_dropping(16, 5s), 2s);
}

TEST(IoDriver, FewDeregistrationsDoNotUnparkBlockedTurn) {
    EXPECT_GE(blocked_turn_while_dropping(15, 200ms), 200ms);
}

// Registration and deregistration from another thread while turn() is blocked.
TEST(IoDriver, RegisterWhileTurnIsBlocked) {
    IoDriver driver;
    SocketPair sp;
    TestTask task;
    std::atomic<bool> registered{false};
    IoRegistration reg;

    std::thread t([&] {
        std::this_thread::sleep_for(20ms);
        reg = IoRegistration(driver, sp.a);
        observe_eagain(reg, IoDirection::Read, task);
        ASSERT_FALSE(reg.poll_ready(IoDirection::Read, task.ctx).has_value());
        registered.store(true);
        write_byte(sp.b);
    });

    // Keep turning until the reader is woken (the initial writable edge may come first).
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (task.wakes() == 0 && std::chrono::steady_clock::now() < deadline)
        driver.turn(100ms);
    t.join();

    EXPECT_TRUE(registered.load());
    EXPECT_EQ(task.wakes(), 1);
}

// End-to-end handshake as a UDP receive future would run it.
TEST(IoDriver, UdpReceiveHandshake) {
    IoDriver driver;
    UdpLoopback rx;
    UdpLoopback tx;
    IoRegistration reg(driver, rx.fd);
    TestTask task;
    char buf[64];

    // Equivalent of one future poll(): returns bytes received, or -1 for Pending.
    auto try_recv = [&]() -> ssize_t {
        for (;;) {
            auto ready = reg.poll_ready(IoDirection::Read, task.ctx);
            if (!ready) return -1;
            ssize_t n = ::recv(rx.fd, buf, sizeof(buf), MSG_DONTWAIT);
            if (n >= 0) return n;
            EXPECT_TRUE(errno == EAGAIN || errno == EWOULDBLOCK);
            reg.clear_ready(*ready);
        }
    };

    EXPECT_EQ(try_recv(), -1);  // nothing sent yet: EAGAIN, then Pending

    const char msg[] = "datagram";
    ASSERT_EQ(::sendto(tx.fd, msg, sizeof(msg), 0,
                       reinterpret_cast<const sockaddr*>(&rx.addr), sizeof(rx.addr)),
              static_cast<ssize_t>(sizeof(msg)));

    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (task.wakes() == 0 && std::chrono::steady_clock::now() < deadline)
        driver.turn(100ms);
    ASSERT_EQ(task.wakes(), 1);

    EXPECT_EQ(try_recv(), static_cast<ssize_t>(sizeof(msg)));
    EXPECT_STREQ(buf, msg);
    EXPECT_EQ(try_recv(), -1);  // drained: back to Pending
}

// ---------------------------------------------------------------------------
// IoDriverParker
// ---------------------------------------------------------------------------

// try_turn() must never wait for the turn mutex: a work-stealing worker that can't get
// the driver parks on its own condvar instead.
TEST(IoDriver, TryTurnReturnsNulloptWhileAnotherThreadTurns) {
    IoDriver driver;
    std::atomic<bool> turning{false};
    std::thread holder([&] {
        turning.store(true);
        driver.turn(std::nullopt);  // blocks until unpark()
    });
    while (!turning.load()) std::this_thread::yield();
    // RACE: `turning` is set just before turn() takes its mutex; the sleep makes it
    // overwhelmingly likely the holder is blocked in epoll_wait by now.
    std::this_thread::sleep_for(50ms);

    const auto start = std::chrono::steady_clock::now();
    EXPECT_FALSE(driver.try_turn(std::chrono::nanoseconds{0}).has_value());
    EXPECT_FALSE(driver.try_turn(std::nullopt).has_value());  // still doesn't block
    EXPECT_LT(std::chrono::steady_clock::now() - start, 1s);

    driver.unpark();
    holder.join();
}

TEST(IoDriver, TryTurnDispatchesWhenFree) {
    IoDriver driver;
    SocketPair sp;
    IoRegistration reg(driver, sp.a);
    TestTask task;

    observe_eagain(reg, IoDirection::Read, task);
    ASSERT_FALSE(reg.poll_ready(IoDirection::Read, task.ctx).has_value());  // stores waker
    write_byte(sp.b);

    const auto dispatched = driver.try_turn(1s);
    ASSERT_TRUE(dispatched.has_value());
    EXPECT_GE(*dispatched, 1u);
    EXPECT_EQ(task.wakes(), 1);
}

namespace {

// One non-blocking send of `size` bytes, in IoRegistration::poll_io()'s op shape.
std::expected<std::size_t, int> try_send(int fd, std::size_t size) {
    static const char buf[4096] = {};
    const ssize_t n = ::send(fd, buf, size, MSG_DONTWAIT);
    if (n < 0) return std::unexpected(errno);
    return static_cast<std::size_t>(n);
}

} // namespace

// poll_io() loops the handshake: it returns std::nullopt only after a real EAGAIN,
// leaving the waker stored, and the driver's writable event makes it succeed again.
TEST(IoDriver, PollIoWaitsForWritable) {
    IoDriver driver;
    SocketPair sp;
    IoRegistration reg(driver, sp.a);
    TestTask task;

    // Fill the stream's send buffer until poll_io() reports "not ready".
    int sends = 0;
    for (;;) {
        auto r = reg.poll_io(IoDirection::Write, task.ctx, [&] { return try_send(sp.a, 4096); });
        if (!r) break;
        ASSERT_TRUE(r->has_value()) << "send failed: errno " << r->error();
        ASSERT_LT(++sends, 100000) << "send buffer never filled";
    }
    EXPECT_EQ(task.wakes(), 0);

    // Drain the peer so the kernel reports the socket writable again.
    char sink[65536];
    while (::recv(sp.b, sink, sizeof(sink), MSG_DONTWAIT) > 0) {}

    EXPECT_GE(driver.turn(1s), 1u);
    EXPECT_EQ(task.wakes(), 1);
    auto r = reg.poll_io(IoDirection::Write, task.ctx, [&] { return try_send(sp.a, 1); });
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->value_or(0), 1u);
}

// An errno other than would-block is returned to the caller, not waited on.
TEST(IoDriver, PollIoPassesThroughOtherErrors) {
    IoDriver driver;
    SocketPair sp;
    IoRegistration reg(driver, sp.a);
    TestTask task;

    int calls = 0;
    auto r = reg.poll_io(IoDirection::Read, task.ctx, [&]() -> std::expected<std::size_t, int> {
        ++calls;
        return std::unexpected(ECONNRESET);
    });
    ASSERT_TRUE(r.has_value());
    ASSERT_FALSE(r->has_value());
    EXPECT_EQ(r->error(), ECONNRESET);
    EXPECT_EQ(calls, 1);
    // Readiness is untouched, so the next attempt runs at once.
    EXPECT_TRUE(reg.poll_ready(IoDirection::Read, task.ctx).has_value());
}

TEST(IoDriverParker, ZeroWaitTurnsOnlyEveryNthCall) {
    IoDriver driver;
    IoDriverParker parker(driver, /*event_interval=*/3);
    SocketPair sp;
    IoRegistration reg(driver, sp.a);
    TestTask task;
    observe_eagain(reg, IoDirection::Read, task);
    ASSERT_FALSE(reg.poll_ready(IoDirection::Read, task.ctx).has_value());  // stores waker

    write_byte(sp.b);
    parker.park(0ns);  // 1st zero wait: skipped
    parker.park(0ns);  // 2nd: skipped
    EXPECT_EQ(task.wakes(), 0);
    parker.park(0ns);  // 3rd: turns
    EXPECT_EQ(task.wakes(), 1);
}

TEST(IoDriverParker, NonZeroWaitAlwaysTurns) {
    IoDriver driver;
    IoDriverParker parker(driver, /*event_interval=*/100);
    SocketPair sp;
    IoRegistration reg(driver, sp.a);
    TestTask task;
    observe_eagain(reg, IoDirection::Read, task);
    ASSERT_FALSE(reg.poll_ready(IoDirection::Read, task.ctx).has_value());

    parker.park(0ns);  // skipped, counts toward the interval
    write_byte(sp.b);
    const auto start = std::chrono::steady_clock::now();
    parker.park(1s);   // turns despite the count; returns on the event, not the timeout
    EXPECT_EQ(task.wakes(), 1);
    EXPECT_LT(std::chrono::steady_clock::now() - start, 500ms);
}

TEST(IoDriverParker, UnparkInterruptsUnlimitedPark) {
    IoDriver driver;
    IoDriverParker parker(driver);
    std::thread t([&parker] {
        std::this_thread::sleep_for(50ms);
        parker.unpark();
    });
    const auto start = std::chrono::steady_clock::now();
    parker.park(std::nullopt);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    t.join();
    EXPECT_GE(elapsed, 40ms);
}

// --- Timers on the driver ---

// An unbounded turn returns at the earliest deadline, and counts the timer it fired.
TEST(IoDriver, TurnTimesOutAtNextDeadline) {
    IoDriver driver;
    TestTask task;
    const auto start = std::chrono::steady_clock::now();
    driver.add_timer(Clock::now() + 30ms, task.ctx.get_weak_waker());
    EXPECT_EQ(driver.turn(std::nullopt), 1u);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_GE(elapsed, 30ms);
    EXPECT_LT(elapsed, 1s);
    EXPECT_EQ(task.wakes(), 1);
}

// max_wait shorter than the deadline still wins; the timer stays queued.
TEST(IoDriver, ShorterMaxWaitBeatsTimer) {
    IoDriver driver;
    TestTask task;
    driver.add_timer(Clock::now() + 1h, task.ctx.get_weak_waker());
    const auto start = std::chrono::steady_clock::now();
    EXPECT_EQ(driver.turn(10ms), 0u);
    EXPECT_LT(std::chrono::steady_clock::now() - start, 1s);
    EXPECT_EQ(task.wakes(), 0);
}

// A cancelled timer still bounds the wait once, but fires nothing.
TEST(IoDriver, CancelledTimerFiresNothing) {
    IoDriver driver;
    TestTask task;
    const detail::TimerId id = driver.add_timer(Clock::now() + 10ms, task.ctx.get_weak_waker());
    driver.cancel_timer(id);
    const auto start = std::chrono::steady_clock::now();
    EXPECT_EQ(driver.turn(1s), 0u);
    EXPECT_LT(std::chrono::steady_clock::now() - start, 500ms);
    EXPECT_EQ(task.wakes(), 0);
}

// Cancelling a timer that has already fired does nothing, and does not disturb the
// timer that reuses its slot.
TEST(IoDriver, CancelAfterFireIsIgnored) {
    IoDriver driver;
    TestTask first, second;
    const detail::TimerId id = driver.add_timer(Clock::now() + 5ms, first.ctx.get_weak_waker());
    EXPECT_EQ(driver.turn(1s), 1u);
    driver.add_timer(Clock::now() + 5ms, second.ctx.get_weak_waker());
    driver.cancel_timer(id);
    EXPECT_EQ(driver.turn(1s), 1u);
    EXPECT_EQ(first.wakes(), 1);
    EXPECT_EQ(second.wakes(), 1);
}

// A timer added from another thread, earlier than anything the blocked turn is
// waiting for, unparks it so the new deadline is honoured.
TEST(IoDriver, EarlierTimerFromOtherThreadUnparks) {
    IoDriver driver;
    TestTask late, early;
    driver.add_timer(Clock::now() + 1h, late.ctx.get_weak_waker());
    std::thread t([&driver, &early] {
        std::this_thread::sleep_for(20ms);
        driver.add_timer(Clock::now() + 10ms, early.ctx.get_weak_waker());
    });
    const auto start = std::chrono::steady_clock::now();
    // The first turn may return for the unpark alone (0 fired) before the deadline.
    std::size_t fired = 0;
    while (fired == 0 && std::chrono::steady_clock::now() - start < 5s)
        fired = driver.turn(std::nullopt);
    t.join();
    EXPECT_EQ(fired, 1u);
    EXPECT_LT(std::chrono::steady_clock::now() - start, 1s);
    EXPECT_EQ(early.wakes(), 1);
    EXPECT_EQ(late.wakes(), 0);
}
