// I/O through the IoDriver on the work-stealing runtime: workers park in the driver,
// remote wakes unpark the driver holder, and busy workers still poll I/O.
// See doc/design/work_stealing_executor.md, "Parking in the I/O driver".

#include <gtest/gtest.h>
#include <coro/runtime/runtime.h>
#include <coro/coro.h>
#include <coro/task/join_handle.h>
#include <coro/detail/context.h>
#include <coro/detail/poll_result.h>
#include "io_test_util.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

using namespace coro;
using namespace coro::io_test;
using namespace std::chrono_literals;

namespace {

using Clock = std::chrono::steady_clock;

// Re-wakes itself on every poll until *stop is set: keeps a worker permanently busy,
// so it never parks.
struct SpinFuture {
    using OutputType = void;
    std::atomic<bool>* m_stop;
    PollResult<void> poll(detail::Context& ctx) {
        if (m_stop->load()) return PollReady;
        ctx.getWaker()->wake();
        return PollPending;
    }
};

// A one-shot signal fired from a foreign thread: a remote wake that is not I/O.
struct Signal {
    std::mutex                m_mutex;
    bool                      m_set = false;   // GUARDED BY m_mutex
    detail::Rc<detail::Waker> m_waker;         // GUARDED BY m_mutex

    void fire() {
        detail::Rc<detail::Waker> waker;
        {
            std::lock_guard lock(m_mutex);
            m_set = true;
            waker = std::move(m_waker);
        }
        if (waker) waker->wake();  // outside the lock
    }
};

struct SignalFuture {
    using OutputType = void;
    Signal* m_signal;
    PollResult<void> poll(detail::Context& ctx) {
        std::lock_guard lock(m_signal->m_mutex);
        if (m_signal->m_set) return PollReady;
        m_signal->m_waker = ctx.getWaker();
        return PollPending;
    }
};

Coro<Clock::time_point> read_while_busy(int fd, std::atomic<bool>* stop) {
    auto spin1 = coro::spawn(SpinFuture{stop});
    auto spin2 = coro::spawn(SpinFuture{stop});
    co_await ReadByteFuture{fd, {}};
    const auto read_at = Clock::now();
    stop->store(true);
    co_await std::move(spin1);
    co_await std::move(spin2);
    co_return read_at;
}

Coro<void> echo(int fd, int rounds) {
    AsyncByteReader reader(fd);
    for (int i = 0; i < rounds; ++i) {
        const char c = co_await reader.read();
        write_byte(fd, c);
    }
}

Coro<void> ping(int fd, int rounds) {
    AsyncByteReader reader(fd);
    for (int i = 0; i < rounds; ++i) {
        const char sent = static_cast<char>(i);
        write_byte(fd, sent);
        const char got = co_await reader.read();
        if (got != sent) throw std::runtime_error("ping: echoed byte mismatch");
    }
}

Coro<int> ping_pong_all(std::vector<int> ping_fds, std::vector<int> echo_fds, int rounds) {
    std::vector<JoinHandle<void>> handles;
    for (std::size_t i = 0; i < ping_fds.size(); ++i) {
        handles.push_back(coro::spawn(echo(echo_fds[i], rounds)));
        handles.push_back(coro::spawn(ping(ping_fds[i], rounds)));
    }
    int finished = 0;
    for (auto& h : handles) {
        co_await std::move(h);
        ++finished;
    }
    co_return finished;
}

Coro<void> leave_pending_read(int fd) {
    coro::spawn(ReadByteFuture{fd, {}}).detach();
    co_return;
}

} // namespace

TEST(WorkStealingIoTest, IoEventWakesTaskOnIdleRuntime) {
    SocketPair sp;
    Runtime rt(2);
    std::thread writer([fd = sp.b] {
        std::this_thread::sleep_for(50ms);
        write_byte(fd, 'q');
    });
    const char got = rt.block_on(ReadByteFuture{sp.a, {}});
    writer.join();
    EXPECT_EQ(got, 'q');
}

// Two spinners keep both workers busy, so nobody parks in the driver: the read can
// only complete through a busy worker's every-event_interval try_turn(0).
TEST(WorkStealingIoTest, BusyWorkersStillPollIo) {
    SocketPair sp;
    std::atomic<bool> stop{false};
    std::atomic<bool> finished{false};
    Clock::time_point written_at;

    Runtime rt(2);
    std::thread writer([&written_at, fd = sp.b] {
        std::this_thread::sleep_for(100ms);  // let the spinners spread over both workers
        written_at = Clock::now();
        write_byte(fd, 'q');
    });
    // Without busy polling the read would never complete. Release the spinners after
    // 5 s so the test fails on the latency check below instead of hanging.
    std::thread watchdog([&stop, &finished] {
        const auto deadline = Clock::now() + 5s;
        while (!finished.load() && Clock::now() < deadline)
            std::this_thread::sleep_for(10ms);
        stop.store(true);
    });

    const auto read_at = rt.block_on(read_while_busy(sp.a, &stop));
    finished.store(true);
    writer.join();
    watchdog.join();

    EXPECT_LT(read_at - written_at, 1s);
}

// A lost wakeup anywhere in the park/unpark handoff shows up here as a hang.
TEST(WorkStealingIoTest, ManySocketsPingPong) {
    constexpr int kPairs  = 16;
    constexpr int kRounds = 200;
    std::vector<std::unique_ptr<SocketPair>> pairs;
    std::vector<int> ping_fds;
    std::vector<int> echo_fds;
    for (int i = 0; i < kPairs; ++i) {
        pairs.push_back(std::make_unique<SocketPair>());
        ping_fds.push_back(pairs.back()->a);
        echo_fds.push_back(pairs.back()->b);
    }

    Runtime rt(4);
    EXPECT_EQ(rt.block_on(ping_pong_all(ping_fds, echo_fds, kRounds)), 2 * kPairs);
}

// Wakes from a foreign thread while every worker is idle: one is in the driver, the
// rest on their condvars. Whichever notify_if_needed() picks must wake up. Repeated
// so both kinds of parked worker get picked.
TEST(WorkStealingIoTest, RemoteWakeUnparksDriverHolder) {
    Runtime rt(2);
    for (int i = 0; i < 20; ++i) {
        Signal signal;
        std::thread firer([&signal] {
            std::this_thread::sleep_for(10ms);  // let the workers park
            signal.fire();
        });
        const auto start = Clock::now();
        rt.block_on(SignalFuture{&signal});
        EXPECT_LT(Clock::now() - start, 2s);
        firer.join();
    }
}

// The runtime is destroyed while a detached task still waits on a socket: workers
// parked in the driver must exit, and the task's IoRegistration must deregister from
// a driver that is still alive.
TEST(WorkStealingIoTest, ShutdownWithPendingIoWait) {
    SocketPair sp;
    {
        Runtime rt(2);
        rt.block_on(leave_pending_read(sp.a));
        std::this_thread::sleep_for(20ms);  // let the detached task register and park
    }
    SUCCEED();
}
