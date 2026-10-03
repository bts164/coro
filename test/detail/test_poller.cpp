// Tests for coro::detail::sys::Poller / PollWaker — the platform readiness layer.
// See doc/design/io_driver.md, "The sys layer".

#include <gtest/gtest.h>
#include <coro/detail/sys/poller.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <system_error>
#include <thread>
#include <vector>

using namespace coro::detail::sys;
using namespace std::chrono_literals;

namespace {

// Owns a non-blocking AF_UNIX stream socketpair.
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
    void close_b() { ::close(b); b = -1; }
};

// Non-blocking UDP socket bound to 127.0.0.1 on an ephemeral port.
struct UdpLoopback {
    int fd = -1;
    sockaddr_in addr{};
    UdpLoopback() {
        fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        EXPECT_GE(fd, 0);
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port        = 0;
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

void drain(int fd) {
    char buf[256];
    while (::read(fd, buf, sizeof(buf)) > 0) {}
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

    // New data is a new edge.
    write_byte(sp.b);
    poller.poll(events, 1s);
    EXPECT_NE(find_key(events, &g_key_a), nullptr);
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
    EXPECT_FALSE(poller.deregister_fd(-1));    // invalid
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
    ASSERT_EQ(::sendto(tx.fd, msg, sizeof(msg), 0,
                       reinterpret_cast<const sockaddr*>(&rx.addr), sizeof(rx.addr)),
              static_cast<ssize_t>(sizeof(msg)));
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
