// ReadinessSource: readiness signalled by a device driver's own code, on the
// desktop and in the Pico configuration (on the host against the stubbed SDK, and on
// the board). See doc/design/io_driver.md, "External drivers" and
// "`ReadinessSource`".
//
// The tests drive a FakeDevice built the way the design doc tells a driver to be
// built: its state behind its own lock, a signal after every change that can turn
// EAGAIN into success, and the signal after the lock is released.

#include <gtest/gtest.h>
#include <coro/coro.h>
#include <coro/detail/context.h>
#include <coro/detail/rc.h>
#include <coro/detail/waker.h>
#include <coro/io/readiness_source.h>
#include <coro/runtime/runtime.h>
#include <coro/sync/sleep.h>
#include <coro/sync/timeout.h>
#include <coro/task/join_handle.h>
#include "executor_traits.h"
#include "isr_trigger.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <expected>
#include <set>
#include <system_error>
#include <vector>

#ifdef CORO_PICO
#include <hardware/sync.h>
#else
#include <mutex>
#include <thread>
#endif

using namespace coro;
using namespace std::chrono_literals;

// Test access to which directions are registered with the driver.
namespace coro::detail {
struct ReadinessSourceProbe {
    static bool read_registered(const ReadinessSource& s) { return s.m_read.registered(); }
    static bool write_registered(const ReadinessSource& s) { return s.m_write.registered(); }
};
} // namespace coro::detail

namespace {

using Probe = detail::ReadinessSourceProbe;

static_assert(Future<ReadinessFuture<decltype([] { return std::expected<int, int>(1); })>>);
static_assert(Future<ReadinessFuture<decltype([] { return std::expected<void, int>(); })>>);

// The device's own lock (rule 3): a hardware spin lock on Pico, since IsrTrigger's
// callback is a real interrupt on the board; a mutex on the desktop.
class DeviceLock {
public:
#ifdef CORO_PICO
    uint32_t lock() { return spin_lock_blocking(m_lock); }
    void unlock(uint32_t save) { spin_unlock(m_lock, save); }
private:
    spin_lock_t* m_lock = spin_lock_instance(next_striped_spin_lock_num());
#else
    uint32_t lock() { m_mutex.lock(); return 0; }
    void unlock(uint32_t) { m_mutex.unlock(); }
private:
    std::mutex m_mutex;
#endif
};

// An in-memory device: a count of received items to take, and a count of free
// transmit slots to fill. Every operation and every change goes through m_lock.
class FakeDevice {
public:
    ReadinessSource ready;

    // The "interrupt": rules 1 to 4. ISR-safe on Pico.
    void receive(int n = 1) {
        uint32_t save = m_lock.lock();
        m_items += n;
        m_lock.unlock(save);
        ready.signal_readable();
    }

    void free_slots(int n = 1) {
        uint32_t save = m_lock.lock();
        m_room += n;
        m_lock.unlock(save);
        ready.signal_writable();
    }

    // The operations. Each item taken gets a distinct sequence number.
    std::expected<int, int> try_recv() {
        uint32_t save = m_lock.lock();
        ++m_recv_calls;
        if (m_items == 0) {
            m_lock.unlock(save);
            return std::unexpected(EAGAIN);
        }
        --m_items;
        const int seq = ++m_taken;
        m_lock.unlock(save);
        return seq;
    }

    std::expected<void, int> try_send() {
        uint32_t save = m_lock.lock();
        ++m_send_calls;
        if (m_room == 0) {
            m_lock.unlock(save);
            return std::unexpected(EAGAIN);
        }
        --m_room;
        m_lock.unlock(save);
        return {};
    }

    int recv_calls() { return read(m_recv_calls); }
    int send_calls() { return read(m_send_calls); }
    int taken()      { return read(m_taken); }

    [[nodiscard]] auto recv() {
        return ready.read_io([this] { return try_recv(); });
    }
    [[nodiscard]] auto send() {
        return ready.write_io([this] { return try_send(); });
    }

private:
    int read(const int& field) {
        uint32_t save = m_lock.lock();
        const int value = field;
        m_lock.unlock(save);
        return value;
    }

    DeviceLock m_lock;
    int        m_items      = 0;
    int        m_room       = 0;
    int        m_taken      = 0;
    int        m_recv_calls = 0;
    int        m_send_calls = 0;
};

// A count shared by tasks that may run on several worker threads.
class SharedCount {
public:
    void add() { uint32_t s = m_lock.lock(); ++m_value; m_lock.unlock(s); }
    int get() { uint32_t s = m_lock.lock(); const int v = m_value; m_lock.unlock(s); return v; }
private:
    DeviceLock m_lock;
    int        m_value = 0;
};

class CountingWaker final : public detail::Waker {
public:
    void wake() override { m_count.fetch_add(1, std::memory_order_relaxed); }
    detail::Rc<detail::Waker> clone() override { return nullptr; }
private:
    std::atomic<int> m_count{0};
};

struct TestTask {
    detail::Rc<CountingWaker> waker = detail::make_rc<CountingWaker>();
    detail::Context ctx{waker};
};

Coro<void> recv_one(FakeDevice& dev, SharedCount& done) {
    co_await dev.recv();
    done.add();
}

} // namespace

template<typename Traits>
class ReadinessSourceTest : public testing::Test {
protected:
    Traits traits;
};
TYPED_TEST_SUITE(ReadinessSourceTest, AllExecutors);

// ---------------------------------------------------------------------------
// The operation and the registration
// ---------------------------------------------------------------------------

// Step 1 of "The registration lives while a direction has waiters": an operation
// that succeeds at once never registers anything.
TEST(ReadinessSource, OpSucceedsWithoutRegistering) {
    SingleThreadRuntime rt;
    FakeDevice dev;
    dev.receive();

    rt.block_on([](FakeDevice& dev) -> Coro<void> {
        TestTask task;
        auto fut = dev.recv();
        auto result = fut.poll(task.ctx);
        EXPECT_TRUE(result.isReady());
        EXPECT_FALSE(result.isError());
        EXPECT_EQ(dev.recv_calls(), 1);
        EXPECT_FALSE(Probe::read_registered(dev.ready));
        EXPECT_FALSE(Probe::write_registered(dev.ready));
        co_return;
    }(dev));
}

// "No signal is lost": a signal made before anyone waits only changes the state,
// and the first waiter's operation finds it.
TYPED_TEST(ReadinessSourceTest, SignalBeforeWaitIsSeen) {
    FakeDevice dev;
    dev.receive();   // signals with no registration: dropped on lwIP, harmless on POSIX
    dev.free_slots();

    const int got = this->traits.rt.block_on([](FakeDevice& dev) -> Coro<int> {
        co_await dev.send();
        co_return co_await dev.recv();
    }(dev));
    EXPECT_EQ(got, 1);
}

// "No signal is lost": the state changes, and is signalled, after the operation
// looked but before it returned EAGAIN. Once on the call before the registration
// exists (the signal finds nothing registered), once on the first call under it (the
// signal lands between the EAGAIN and clear_ready()).
TYPED_TEST(ReadinessSourceTest, SignalBetweenEagainAndClearIsNotLost) {
    for (int racing_call = 1; racing_call <= 2; ++racing_call) {
        FakeDevice dev;
        const auto outcome = this->traits.rt.block_on(
            [](FakeDevice& dev, int racing_call) -> Coro<int> {
                int calls = 0;
                auto op = [&dev, &calls, racing_call]() -> std::expected<int, int> {
                    auto r = dev.try_recv();
                    if (++calls == racing_call) {
                        EXPECT_FALSE(r.has_value());
                        dev.receive();   // the "interrupt" lands now
                        return std::unexpected(EAGAIN);
                    }
                    return r;
                };
                auto r = co_await timeout(2s, dev.ready.read_io(op));
                co_return r.index() == 0 ? 0 : 1;
            }(dev, racing_call));
        EXPECT_EQ(outcome, 0) << "lost the signal racing call " << racing_call;
    }
}

// One notify object per direction: a signal in one direction wakes only that
// direction's waiters.
TYPED_TEST(ReadinessSourceTest, DirectionsAreIndependent) {
    FakeDevice dev;
    this->traits.rt.block_on([](FakeDevice& dev) -> Coro<void> {
        SharedCount received;
        auto reader = spawn(recv_one(dev, received));
        auto writer = spawn([](FakeDevice& dev) -> Coro<void> {
            co_await dev.send();
        }(dev));
        co_await sleep_for(20ms);   // both waiting
        const int recv_calls = dev.recv_calls();

        dev.free_slots();
        co_await writer;
        co_await sleep_for(20ms);
        EXPECT_EQ(dev.recv_calls(), recv_calls) << "a write signal woke the reader";
        EXPECT_EQ(received.get(), 0);

        dev.receive();
        co_await reader;
        EXPECT_EQ(received.get(), 1);
    }(dev));
}

// ---------------------------------------------------------------------------
// Waiters
// ---------------------------------------------------------------------------

// "Waiters": a signal wakes every waiter in the direction; each calls its operation,
// one gets the item, and the others wait again.
TYPED_TEST(ReadinessSourceTest, EveryWaiterIsWokenAndOneClaims) {
    static constexpr int kWaiters = 3;
    FakeDevice dev;
    this->traits.rt.block_on([](FakeDevice& dev) -> Coro<void> {
        SharedCount done;
        std::vector<JoinHandle<void>> waiters;
        for (int i = 0; i < kWaiters; ++i) waiters.push_back(spawn(recv_one(dev, done)));
        co_await sleep_for(20ms);   // all waiting
        const int calls = dev.recv_calls();

        dev.receive();
        co_await sleep_for(20ms);
        EXPECT_GE(dev.recv_calls() - calls, kWaiters) << "not every waiter was woken";
        EXPECT_EQ(done.get(), 1);

        dev.receive(kWaiters - 1);
        for (auto& w : waiters) co_await w;
        EXPECT_EQ(done.get(), kWaiters);
        EXPECT_EQ(dev.taken(), kWaiters);
    }(dev));
}

// "Cancellation": a future dropped while waiting unlinks its waiter, and as the
// direction's last waiter it drops the registration.
TYPED_TEST(ReadinessSourceTest, DroppedFutureUnlinks) {
    FakeDevice dev;
    this->traits.rt.block_on([](FakeDevice& dev) -> Coro<void> {
        auto r = co_await timeout(20ms, dev.recv());
        EXPECT_EQ(r.index(), 1u);
        EXPECT_FALSE(Probe::read_registered(dev.ready));

        // The source still works: the next wait registers afresh.
        SharedCount done;
        auto h = spawn(recv_one(dev, done));
        co_await sleep_for(20ms);
        dev.receive();
        co_await h;
        EXPECT_EQ(done.get(), 1);
    }(dev));
}

// Step 3 of "The registration lives while a direction has waiters": the registration
// is shared, and the last waiter to leave drops it.
TYPED_TEST(ReadinessSourceTest, LastWaiterDeregisters) {
    FakeDevice dev;
    this->traits.rt.block_on([](FakeDevice& dev) -> Coro<void> {
        SharedCount done;
        auto a = spawn(recv_one(dev, done));
        auto b = spawn(recv_one(dev, done));
        co_await sleep_for(20ms);
        EXPECT_TRUE(Probe::read_registered(dev.ready));
        EXPECT_FALSE(Probe::write_registered(dev.ready));

        dev.receive();
        co_await sleep_for(20ms);
        EXPECT_EQ(done.get(), 1);
        EXPECT_TRUE(Probe::read_registered(dev.ready)) << "one waiter is still waiting";

        dev.receive();
        co_await a;
        co_await b;
        EXPECT_FALSE(Probe::read_registered(dev.ready));
    }(dev));
}

#ifndef CORO_PICO
// On the desktop, two first waiters on different worker threads must share one
// registration: registering the eventfd twice fails with EEXIST. Each round starts
// with the direction unregistered, so every round races the first registration.
TEST(ReadinessSource, ConcurrentFirstWaitersRegisterOnce) {
    static constexpr int kRounds  = 50;
    static constexpr int kWaiters = 16;
    Runtime rt{std::size_t{4}};
    FakeDevice dev;

    rt.block_on([](FakeDevice& dev) -> Coro<void> {
        for (int round = 0; round < kRounds; ++round) {
            SharedCount done;
            std::vector<JoinHandle<void>> waiters;
            for (int i = 0; i < kWaiters; ++i) waiters.push_back(spawn(recv_one(dev, done)));
            co_await sleep_for(5ms);
            for (int i = 0; i < kWaiters; ++i) dev.receive();
            for (auto& w : waiters) co_await w;   // rethrows a registration failure
            EXPECT_EQ(done.get(), kWaiters);
            EXPECT_FALSE(Probe::read_registered(dev.ready));
        }
    }(dev));
}
#endif

// ---------------------------------------------------------------------------
// Where the signal comes from
// ---------------------------------------------------------------------------

#ifndef CORO_PICO
TYPED_TEST(ReadinessSourceTest, SignalFromOtherThread) {
    FakeDevice dev;
    std::thread device_thread([&dev] {
        std::this_thread::sleep_for(20ms);
        dev.receive();
    });
    const int got = this->traits.rt.block_on([](FakeDevice& dev) -> Coro<int> {
        auto r = co_await timeout(5s, dev.recv());
        co_return r.index() == 0 ? std::get<0>(r).value : -1;
    }(dev));
    device_thread.join();
    EXPECT_EQ(got, 1);
}
#endif

// The single-threaded runtime sleeps in the driver's turn with nothing else to do;
// the interrupt's signal must end that turn. A hardware alarm on the board, a thread
// on the host.
TEST(ReadinessSource, SignalFromIsrEndsASleepingTurn) {
    SingleThreadRuntime rt;
    FakeDevice dev;
    int got = -1;
    {
        IsrTrigger isr(20ms, 3, [&dev](int) { dev.receive(); });
        got = rt.block_on([](FakeDevice& dev) -> Coro<int> {
            int last = -1;
            for (int i = 0; i < 3; ++i) {
                auto r = co_await timeout(5s, dev.recv());
                if (r.index() != 0) co_return -1;
                last = std::get<0>(r).value;
            }
            co_return last;
        }(dev));
    }
    EXPECT_EQ(got, 3);
}

// "Availability": no network needed. On Pico SingleThreadRuntime is
// PicoNetwork::None (executor_traits.h), so this runs with no network stack at all.
TEST(ReadinessSource, WorksWithoutNetwork) {
    SingleThreadRuntime rt;
    FakeDevice dev;
    rt.block_on([](FakeDevice& dev) -> Coro<void> {
        auto writer = spawn([](FakeDevice& dev) -> Coro<void> { co_await dev.send(); }(dev));
        SharedCount done;
        auto reader = spawn(recv_one(dev, done));
        co_await sleep_for(10ms);
        dev.free_slots();
        dev.receive();
        co_await writer;
        co_await reader;
        EXPECT_EQ(done.get(), 1);
    }(dev));
}

// "Shutdown": a pending future fails with sys::kDriverShutDown (ECANCELED) once the
// driver has been shut down, without calling the operation again.
TEST(ReadinessSource, ShutdownFailsPendingFuture) {
    SingleThreadRuntime rt;
    FakeDevice dev;
    rt.block_on([](FakeDevice& dev) -> Coro<void> {
        int error = 0;
        auto waiter = spawn([](FakeDevice& dev, int& error) -> Coro<void> {
            try {
                co_await dev.recv();
            } catch (const std::system_error& e) {
                error = e.code().value();
            }
        }(dev, error));
        co_await sleep_for(20ms);   // waiting
        const int calls = dev.recv_calls();

        current_runtime().io_driver().shutdown();
        co_await waiter;
        EXPECT_EQ(error, ECANCELED);
        EXPECT_EQ(dev.recv_calls(), calls);
    }(dev));
}
