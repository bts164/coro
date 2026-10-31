// Tests for coro::detail::sys::Poller / PollWaker — the platform readiness layer.
// See doc/design/io_driver.md, "The sys layer", and for the Windows backend's own
// tests at the end, doc/design/windows_port.md.

#include <gtest/gtest.h>
#include <coro/detail/sys/poller.h>
#include "socket_test_util.h"

#include <algorithm>
#include <chrono>
#include <memory>
#include <system_error>
#include <thread>
#include <vector>

using namespace coro::detail::sys;
using namespace std::chrono_literals;
using coro::test_sockets::drain;
using coro::test_sockets::SocketPair;
using coro::test_sockets::UdpLoopback;

namespace {

// The byte is at the peer when this returns, so a zero-timeout poll sees it.
void write_byte(RawFd fd) {
    coro::test_sockets::write_byte_and_settle(fd);
}

const Event* find_key(const std::vector<Event>& events, void* key) {
    for (const auto& e : events)
        if (e.key == key) return &e;
    return nullptr;
}

int g_key_a;  // addresses used as registration keys
int g_key_b;

} // namespace

TEST(Poller, ZeroTimeoutWithNoFdsReturnsImmediately) {
    Poller poller;
    std::vector<Event> events;
    const auto start = std::chrono::steady_clock::now();
    poller.poll(events, 0ns);
    EXPECT_TRUE(events.empty());
    EXPECT_LT(std::chrono::steady_clock::now() - start, 100ms);
}

TEST(Poller, TimeoutIsHonored) {
    Poller poller;
    std::vector<Event> events;
    const auto start = std::chrono::steady_clock::now();
    poller.poll(events, 20ms);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_TRUE(events.empty());
    EXPECT_GE(elapsed, 20ms);
    EXPECT_LT(elapsed, 1s);
}

TEST(Poller, SubMillisecondTimeoutDoesNotWaitForever) {
    Poller poller;
    std::vector<Event> events;
    const auto start = std::chrono::steady_clock::now();
    poller.poll(events, 200us);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_TRUE(events.empty());
    EXPECT_GE(elapsed, 200us);
    EXPECT_LT(elapsed, 100ms);
}

TEST(Poller, ReadableEventCarriesKey) {
    Poller poller;
    SocketPair sp;
    poller.register_fd(sp.a, &g_key_a, Interest::read());
    write_byte(sp.b);

    std::vector<Event> events;
    poller.poll(events, 1s);
    const Event* e = find_key(events, &g_key_a);
    ASSERT_NE(e, nullptr);
    EXPECT_TRUE(e->readable);
    EXPECT_FALSE(e->writable);
    EXPECT_FALSE(e->error);
}

TEST(Poller, WritableReportedOnRegistration) {
    Poller poller;
    SocketPair sp;
    poller.register_fd(sp.a, &g_key_a, Interest::write());

    std::vector<Event> events;
    poller.poll(events, 1s);
    const Event* e = find_key(events, &g_key_a);
    ASSERT_NE(e, nullptr);
    EXPECT_TRUE(e->writable);
}

TEST(Poller, IsEdgeTriggered) {
    Poller poller;
    SocketPair sp;
    poller.register_fd(sp.a, &g_key_a, Interest::read());
    write_byte(sp.b);

    std::vector<Event> events;
    poller.poll(events, 1s);
    ASSERT_NE(find_key(events, &g_key_a), nullptr);

    // Data is still unread, but readiness hasn't changed: no new event.
    events.clear();
    poller.poll(events, 0ns);
    EXPECT_EQ(find_key(events, &g_key_a), nullptr);

    // A consumer waits again only after it has seen EAGAIN, and tells the poller so
    // (a no-op on epoll; on Windows the direction is re-enabled). New data is then a
    // new edge.
    drain(sp.a);
    poller.rearm(sp.a, Interest::read());
    write_byte(sp.b);
    poller.poll(events, 1s);
    EXPECT_NE(find_key(events, &g_key_a), nullptr);
}

TEST(Poller, LevelTriggeredReportsUntilDrained) {
    Poller poller;
    SocketPair sp;
    poller.register_fd(sp.a, &g_key_a, Interest::read(), Trigger::Level);
    write_byte(sp.b);

    // Unread data is reported by every poll, not only the first.
    std::vector<Event> events;
    for (int i = 0; i < 3; ++i) {
        events.clear();
        poller.poll(events, 1s);
        const Event* e = find_key(events, &g_key_a);
        ASSERT_NE(e, nullptr) << "poll " << i;
        EXPECT_TRUE(e->readable);
    }

    drain(sp.a);
    events.clear();
    poller.poll(events, 0ns);
    EXPECT_EQ(find_key(events, &g_key_a), nullptr);
}

TEST(Poller, LevelTriggeredFollowsInterestChange) {
    Poller poller;
    SocketPair sp;
    // An idle socket is always writable, so write interest reports on every poll.
    poller.register_fd(sp.a, &g_key_a, Interest::read_write(), Trigger::Level);
    std::vector<Event> events;
    for (int i = 0; i < 2; ++i) {
        events.clear();
        poller.poll(events, 1s);
        const Event* e = find_key(events, &g_key_a);
        ASSERT_NE(e, nullptr) << "poll " << i;
        EXPECT_TRUE(e->writable);
        EXPECT_FALSE(e->readable);
    }

    // Without write interest it is quiet until there is something to read.
    poller.reregister_fd(sp.a, &g_key_a, Interest::read(), Trigger::Level);
    events.clear();
    poller.poll(events, 0ns);
    EXPECT_EQ(find_key(events, &g_key_a), nullptr);

    write_byte(sp.b);
    poller.poll(events, 1s);
    const Event* e = find_key(events, &g_key_a);
    ASSERT_NE(e, nullptr);
    EXPECT_TRUE(e->readable);
    EXPECT_FALSE(e->writable);

    // No interest at all: unread data is not reported.
    poller.reregister_fd(sp.a, &g_key_a, Interest{}, Trigger::Level);
    events.clear();
    poller.poll(events, 0ns);
    EXPECT_EQ(find_key(events, &g_key_a), nullptr);
}

TEST(Poller, DeregisterStopsEvents) {
    Poller poller;
    SocketPair sp;
    poller.register_fd(sp.a, &g_key_a, Interest::read());
    EXPECT_TRUE(poller.deregister_fd(sp.a));
    write_byte(sp.b);

    std::vector<Event> events;
    poller.poll(events, 20ms);
    EXPECT_EQ(find_key(events, &g_key_a), nullptr);
}

TEST(Poller, DeregisterUnknownFdIsHarmless) {
    Poller poller;
    SocketPair sp;
    EXPECT_FALSE(poller.deregister_fd(sp.a));  // never registered
    EXPECT_FALSE(poller.deregister_fd(kInvalidFd));
}

TEST(Poller, ReregisterChangesKeyAndInterest) {
    Poller poller;
    SocketPair sp;
    poller.register_fd(sp.a, &g_key_a, Interest::read());

    std::vector<Event> events;
    poller.poll(events, 0ns);
    EXPECT_EQ(find_key(events, &g_key_a), nullptr);  // no data, no write interest

    poller.reregister_fd(sp.a, &g_key_b, Interest::read_write());
    poller.poll(events, 1s);
    EXPECT_EQ(find_key(events, &g_key_a), nullptr);
    const Event* e = find_key(events, &g_key_b);
    ASSERT_NE(e, nullptr);
    EXPECT_TRUE(e->writable);
}

TEST(Poller, RegisterTwiceThrows) {
    Poller poller;
    SocketPair sp;
    poller.register_fd(sp.a, &g_key_a, Interest::read());
    EXPECT_THROW(poller.register_fd(sp.a, &g_key_a, Interest::read()), std::system_error);
}

TEST(Poller, PeerCloseReportsHup) {
    Poller poller;
    SocketPair sp;
    poller.register_fd(sp.a, &g_key_a, Interest::read());
    sp.close_b();

    std::vector<Event> events;
    poller.poll(events, 1s);
    const Event* e = find_key(events, &g_key_a);
    ASSERT_NE(e, nullptr);
    EXPECT_TRUE(e->hup);
}

TEST(Poller, PollAppendsWithoutClearing) {
    Poller poller;
    SocketPair sp;
    poller.register_fd(sp.a, &g_key_a, Interest::read());
    write_byte(sp.b);

    std::vector<Event> events{Event{&g_key_b, false, false, false, false}};
    poller.poll(events, 1s);
    EXPECT_NE(find_key(events, &g_key_b), nullptr);
    EXPECT_NE(find_key(events, &g_key_a), nullptr);
}

TEST(Poller, UdpDatagramMakesSocketReadable) {
    Poller poller;
    UdpLoopback rx;
    UdpLoopback tx;
    poller.register_fd(rx.fd, &g_key_a, Interest::read_write());

    // Consume the initial writable edge.
    std::vector<Event> events;
    poller.poll(events, 1s);
    events.clear();

    const char msg[] = "hello";
    ASSERT_TRUE(tx.send_to(rx, msg, sizeof(msg)));
    poller.poll(events, 1s);
    const Event* e = find_key(events, &g_key_a);
    ASSERT_NE(e, nullptr);
    EXPECT_TRUE(e->readable);
}

// ---------------------------------------------------------------------------
// PollWaker
// ---------------------------------------------------------------------------

TEST(PollWaker, WakesBlockedPollFromAnotherThread) {
    Poller poller;
    PollWaker waker(poller, &g_key_a);

    std::thread t([&waker] {
        std::this_thread::sleep_for(50ms);
        waker.wake();
    });

    std::vector<Event> events;
    const auto start = std::chrono::steady_clock::now();
    poller.poll(events, std::nullopt);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    t.join();

    EXPECT_NE(find_key(events, &g_key_a), nullptr);
    EXPECT_GE(elapsed, 40ms);
}

TEST(PollWaker, WakeBeforePollIsNotLost) {
    Poller poller;
    PollWaker waker(poller, &g_key_a);
    waker.wake();

    std::vector<Event> events;
    poller.poll(events, 1s);
    EXPECT_NE(find_key(events, &g_key_a), nullptr);
}

TEST(PollWaker, ResetConsumesWakes) {
    Poller poller;
    PollWaker waker(poller, &g_key_a);
    waker.wake();
    waker.wake();

    std::vector<Event> events;
    poller.poll(events, 1s);
    ASSERT_NE(find_key(events, &g_key_a), nullptr);
    waker.reset();

    events.clear();
    poller.poll(events, 0ns);
    EXPECT_EQ(find_key(events, &g_key_a), nullptr);
}

TEST(PollWaker, WakeAfterResetFiresAgain) {
    Poller poller;
    PollWaker waker(poller, &g_key_a);
    waker.wake();

    std::vector<Event> events;
    poller.poll(events, 1s);
    waker.reset();

    events.clear();
    waker.wake();
    poller.poll(events, 1s);
    EXPECT_NE(find_key(events, &g_key_a), nullptr);
}

TEST(PollWaker, CoexistsWithIoRegistrations) {
    Poller poller;
    PollWaker waker(poller, &g_key_b);
    SocketPair sp;
    poller.register_fd(sp.a, &g_key_a, Interest::read());

    write_byte(sp.b);
    waker.wake();

    std::vector<Event> events;
    // Both may arrive in one batch or two; collect until both seen.
    for (int i = 0; i < 2 && (!find_key(events, &g_key_a) || !find_key(events, &g_key_b)); ++i)
        poller.poll(events, 1s);
    EXPECT_NE(find_key(events, &g_key_a), nullptr);
    EXPECT_NE(find_key(events, &g_key_b), nullptr);
    drain(sp.a);
}

#ifdef CORO_SYS_WINDOWS
// ---------------------------------------------------------------------------
// The IOCP/AFD backend: edge emulation, AFD handle groups, poll lifetime, the
// timeout timer. See doc/design/windows_port.md.
// ---------------------------------------------------------------------------

// A direction that has been reported is not polled for again until rearm(): more
// data alone produces no event.
TEST(Poller, ReportedDirectionStaysQuietUntilRearm) {
    Poller poller;
    SocketPair sp;
    poller.register_fd(sp.a, &g_key_a, Interest::read());
    write_byte(sp.b);

    std::vector<Event> events;
    poller.poll(events, 1s);
    ASSERT_NE(find_key(events, &g_key_a), nullptr);

    events.clear();
    write_byte(sp.b);
    poller.poll(events, 50ms);
    EXPECT_EQ(find_key(events, &g_key_a), nullptr);

    poller.rearm(sp.a, Interest::read());
    poller.poll(events, 1s);
    const Event* e = find_key(events, &g_key_a);
    ASSERT_NE(e, nullptr);
    EXPECT_TRUE(e->readable);
}

// Data that arrived while the direction was disarmed is reported once it is re-armed:
// an AFD poll reports the socket's state when it is submitted. No edge is lost.
TEST(Poller, RearmWithDataAlreadyPresentReportsAtOnce) {
    Poller poller;
    SocketPair sp;
    poller.register_fd(sp.a, &g_key_a, Interest::read());
    write_byte(sp.b);

    std::vector<Event> events;
    poller.poll(events, 1s);
    ASSERT_NE(find_key(events, &g_key_a), nullptr);
    drain(sp.a);                 // ... EAGAIN, but before the consumer re-arms:
    write_byte(sp.b);            // new data
    poller.rearm(sp.a, Interest::read());

    events.clear();
    poller.poll(events, 1s);
    EXPECT_NE(find_key(events, &g_key_a), nullptr);
}

// Re-arming one direction re-enables only that one. Each re-arm cancels the poll in
// flight and resubmits it wider, without losing what was armed already.
TEST(Poller, RearmReEnablesOnlyThatDirection) {
    Poller poller;
    SocketPair sp;
    poller.register_fd(sp.a, &g_key_a, Interest::read_write());
    write_byte(sp.b);

    // The first poll may report writable alone (the registration's poll can complete
    // before the byte arrives); collect until both directions have been seen.
    std::vector<Event> events;
    bool readable = false;
    bool writable = false;
    for (int i = 0; i < 3 && !(readable && writable); ++i) {
        events.clear();
        poller.poll(events, 1s);
        if (const Event* e = find_key(events, &g_key_a)) {
            readable = readable || e->readable;
            writable = writable || e->writable;
        }
    }
    ASSERT_TRUE(readable);
    ASSERT_TRUE(writable);

    events.clear();
    poller.rearm(sp.a, Interest::write());
    poller.poll(events, 1s);
    const Event* e = find_key(events, &g_key_a);
    ASSERT_NE(e, nullptr);
    EXPECT_TRUE(e->writable);
    EXPECT_FALSE(e->readable);   // still unread, but read isn't re-armed

    events.clear();
    poller.rearm(sp.a, Interest::read());
    poller.poll(events, 1s);
    e = find_key(events, &g_key_a);
    ASSERT_NE(e, nullptr);
    EXPECT_TRUE(e->readable);
    EXPECT_FALSE(e->writable);
}

// A deregistration cancels the poll in flight; its completion arrives later and must
// report nothing.
TEST(Poller, DeregisterWithPollInFlightReportsNothing) {
    Poller poller;
    SocketPair sp;
    poller.register_fd(sp.a, &g_key_a, Interest::read());
    std::vector<Event> events;
    poller.poll(events, 0ns);
    ASSERT_TRUE(poller.deregister_fd(sp.a));
    write_byte(sp.b);

    for (int i = 0; i < 3; ++i) poller.poll(events, 20ms);
    EXPECT_EQ(find_key(events, &g_key_a), nullptr);
}

// The destructor collects every outstanding poll before it frees the state the
// kernel writes into. Meaningful under Application Verifier or ASan.
TEST(Poller, DestroyWithPollsInFlight) {
    SocketPair sp1, sp2, sp3;
    {
        Poller poller;
        poller.register_fd(sp1.a, &g_key_a, Interest::read());
        poller.register_fd(sp2.a, &g_key_a, Interest::read_write());
        poller.register_fd(sp3.a, &g_key_b, Interest::read());
        ASSERT_TRUE(poller.deregister_fd(sp3.a));   // one cancelled, two left registered
    }
    SUCCEED();
}

// I/O sent through a handle bound to a completion port is not cancelled when the
// thread that sent it exits.
TEST(Poller, RegisterFromExitedThread) {
    Poller poller;
    SocketPair sp;
    std::thread([&] { poller.register_fd(sp.a, &g_key_a, Interest::read()); }).join();
    write_byte(sp.b);

    std::vector<Event> events;
    poller.poll(events, 1s);
    EXPECT_NE(find_key(events, &g_key_a), nullptr);
}

// More sockets than one \Device\Afd handle serves (32) each get their own events.
TEST(Poller, ManySocketsSpanAfdGroups) {
    constexpr int kSockets = 40;
    Poller poller;
    std::vector<std::unique_ptr<SocketPair>> pairs;
    for (int i = 0; i < kSockets; ++i) {
        pairs.push_back(std::make_unique<SocketPair>());
        poller.register_fd(pairs.back()->a, pairs.back().get(), Interest::read());
    }
    for (auto& p : pairs) write_byte(p->b);

    std::vector<Event> events;
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    auto all_seen = [&] {
        return std::all_of(pairs.begin(), pairs.end(),
                           [&](const auto& p) { return find_key(events, p.get()) != nullptr; });
    };
    while (!all_seen() && std::chrono::steady_clock::now() < deadline) poller.poll(events, 100ms);
    EXPECT_TRUE(all_seen());
    for (auto& p : pairs) EXPECT_TRUE(poller.deregister_fd(p->a));
}

// A reset by the peer (AFD_POLL_ABORT) is an error and a hang-up.
TEST(Poller, ResetConnectionReportsErrorAndHup) {
    Poller poller;
    SocketPair sp;
    poller.register_fd(sp.a, &g_key_a, Interest::read());
    // Linger on, zero timeout: closesocket() resets the connection.
    const LINGER linger{1, 0};
    ASSERT_EQ(::setsockopt(static_cast<SOCKET>(sp.b), SOL_SOCKET, SO_LINGER,
                           reinterpret_cast<const char*>(&linger), sizeof(linger)), 0);
    sp.close_b();

    std::vector<Event> events;
    poller.poll(events, 1s);
    const Event* e = find_key(events, &g_key_a);
    ASSERT_NE(e, nullptr);
    EXPECT_TRUE(e->error);
    EXPECT_TRUE(e->hup);
}

// The high-resolution timer ends a timed wait well under the 15.6 ms system tick a
// millisecond wait would round to, and never early.
TEST(Poller, SubMillisecondTimeoutIsPrecise) {
    Poller poller;
    std::vector<Event> events;
    std::vector<std::chrono::steady_clock::duration> took;
    for (int i = 0; i < 21; ++i) {
        const auto start = std::chrono::steady_clock::now();
        poller.poll(events, 300us);
        took.push_back(std::chrono::steady_clock::now() - start);
        EXPECT_GE(took.back(), 300us);
    }
    std::sort(took.begin(), took.end());
    EXPECT_LT(took[took.size() / 2], 3ms) << "median of 21 waits of 300 us";
    EXPECT_TRUE(events.empty());
}

// Wakes before a poll() post one packet between them, not one each.
TEST(PollWaker, RepeatedWakesPostOnePacket) {
    Poller poller;
    PollWaker waker(poller, &g_key_a);
    for (int i = 0; i < 100; ++i) waker.wake();

    std::vector<Event> events;
    poller.poll(events, 1s);
    EXPECT_EQ(std::count_if(events.begin(), events.end(),
                            [](const Event& e) { return e.key == &g_key_a; }), 1);
    waker.reset();

    events.clear();
    poller.poll(events, 0ns);
    EXPECT_TRUE(events.empty());
}
#endif  // CORO_SYS_WINDOWS
