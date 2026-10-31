#pragma once

// Raw sockets for tests that drive the Poller or the IoDriver directly, below the
// socket types, on every desktop platform. Byte I/O goes through the sys seams, so
// errors are errno values everywhere.
//
//   SocketPair   a connected, non-blocking stream pair: an AF_UNIX socketpair on
//                POSIX, a loopback TCP connection (TCP_NODELAY) on Windows
//   UdpLoopback  a non-blocking UDP socket bound to 127.0.0.1 on an ephemeral port
//
// Used by test_poller.cpp, test_io_driver.cpp and runtime/io_test_util.h.

#include <coro/detail/sys/poller.h>
#include <coro/detail/sys/socket.h>
#include <coro/detail/sys/tcp.h>
#include <coro/detail/sys/udp.h>
#include <coro/io/socket_address.h>

#include <gtest/gtest.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <cstddef>
#include <cstdint>
#include <expected>
#include <mutex>
#include <unordered_map>

namespace coro::test_sockets {

using detail::sys::RawFd;
using detail::sys::kInvalidFd;

/// One non-blocking read of up to `size` bytes, or errno. 0 is end of stream.
inline std::expected<std::size_t, int> read_some(RawFd fd, void* data, std::size_t size) {
    return detail::sys::tcp_try_read(fd, static_cast<std::byte*>(data), size);
}

/// One non-blocking write of up to `size` bytes, or errno.
inline std::expected<std::size_t, int> write_some(RawFd fd, const void* data, std::size_t size) {
    return detail::sys::tcp_try_write(fd, static_cast<const std::byte*>(data), size);
}

/// Reads until the stream has nothing more (would block), ended, or failed.
inline void drain(RawFd fd) {
    char buf[4096];
    for (;;) {
        auto n = read_some(fd, buf, sizeof(buf));
        if (!n || *n == 0) return;
    }
}

/// Writes one byte; a 1-byte write to an idle pair never finds the buffer full.
/// EXPECT, not ASSERT: coroutines call it too.
inline void write_byte(RawFd fd, char c = 'x') {
    auto n = write_some(fd, &c, 1);
    EXPECT_TRUE(n.has_value() && *n == 1) << "write failed: errno " << (n ? 0 : n.error());
}

/// Reads one byte that must be there.
inline char read_byte(RawFd fd) {
    char c = 0;
    auto n = read_some(fd, &c, 1);
    EXPECT_TRUE(n.has_value() && *n == 1) << "read failed: errno " << (n ? 0 : n.error());
    return c;
}

#ifdef _WIN32
namespace impl {

inline void ensure_winsock() {
    static const bool started = [] {
        WSADATA data;
        return ::WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    EXPECT_TRUE(started);
}

// The other end of each live SocketPair end, for write_byte_and_settle().
inline std::mutex& peers_mutex() {
    static std::mutex m;
    return m;
}
inline std::unordered_map<RawFd, RawFd>& peers() {
    static std::unordered_map<RawFd, RawFd> p;
    return p;
}

inline void set_peers(RawFd a, RawFd b) {
    std::lock_guard lock(peers_mutex());
    peers()[a] = b;
    peers()[b] = a;
}

inline void forget(RawFd fd) {
    std::lock_guard lock(peers_mutex());
    peers().erase(fd);
}

} // namespace impl
#endif

/// write_byte(), then, where the transport is asynchronous (loopback TCP on Windows),
/// waits until the byte has reached the other end of fd's SocketPair, so a
/// zero-timeout poll right after it sees the readiness, as it does on an AF_UNIX
/// socketpair. Only for a peer nobody is reading from meanwhile.
inline void write_byte_and_settle(RawFd fd, char c = 'x') {
    write_byte(fd, c);
#ifdef _WIN32
    RawFd peer = kInvalidFd;
    {
        std::lock_guard lock(impl::peers_mutex());
        const auto it = impl::peers().find(fd);
        if (it != impl::peers().end()) peer = it->second;
    }
    if (peer == kInvalidFd) return;
    // WSAPoll asks AFD the same question the Poller's pending poll asks; AFD answers
    // both from one indication of the data.
    WSAPOLLFD p{};
    p.fd     = static_cast<SOCKET>(peer);
    p.events = POLLRDNORM;
    EXPECT_EQ(::WSAPoll(&p, 1, 1000), 1) << "the byte never arrived";
#endif
}

/// A connected, non-blocking stream pair; closes both ends.
struct SocketPair {
    RawFd a = kInvalidFd;
    RawFd b = kInvalidFd;

    SocketPair() {
#ifdef _WIN32
        impl::ensure_winsock();
        const SOCKET listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        EXPECT_NE(listener, INVALID_SOCKET);
        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        int len = sizeof(addr);
        EXPECT_EQ(::bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
        EXPECT_EQ(::listen(listener, 1), 0);
        EXPECT_EQ(::getsockname(listener, reinterpret_cast<sockaddr*>(&addr), &len), 0);
        const SOCKET client = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        EXPECT_EQ(::connect(client, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
        const SOCKET server = ::accept(listener, nullptr, nullptr);
        EXPECT_NE(server, INVALID_SOCKET);
        ::closesocket(listener);
        for (SOCKET s : {client, server}) {
            // Single bytes go out at once, as they do on a socketpair.
            const BOOL nodelay = TRUE;
            ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay),
                         sizeof(nodelay));
            u_long nonblocking = 1;
            ::ioctlsocket(s, FIONBIO, &nonblocking);
        }
        a = static_cast<RawFd>(client);
        b = static_cast<RawFd>(server);
        impl::set_peers(a, b);
#else
        int fds[2];
        EXPECT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds), 0);
        a = fds[0];
        b = fds[1];
#endif
    }

    ~SocketPair() {
        close_a();
        close_b();
    }

    SocketPair(const SocketPair&)            = delete;
    SocketPair& operator=(const SocketPair&) = delete;

    void close_a() { close_end(a); }
    void close_b() { close_end(b); }

private:
    static void close_end(RawFd& fd) {
        if (fd == kInvalidFd) return;
#ifdef _WIN32
        impl::forget(fd);
#endif
        detail::sys::close_socket(fd);
        fd = kInvalidFd;
    }
};

/// A non-blocking UDP socket bound to 127.0.0.1 on an ephemeral port.
struct UdpLoopback {
    RawFd    fd   = kInvalidFd;
    uint16_t port = 0;

    UdpLoopback() {
        fd = detail::sys::udp_open(SocketAddress{Ipv4Address{{127, 0, 0, 1}}, 0});
        sockaddr_storage storage{};
#ifdef _WIN32
        int len = sizeof(storage);
        const auto s = static_cast<SOCKET>(fd);
#else
        socklen_t len = sizeof(storage);
        const int s = fd;
#endif
        EXPECT_EQ(::getsockname(s, reinterpret_cast<sockaddr*>(&storage), &len), 0);
        port = ntohs(reinterpret_cast<const sockaddr_in*>(&storage)->sin_port);
    }
    ~UdpLoopback() {
        if (fd != kInvalidFd) detail::sys::close_socket(fd);
    }

    UdpLoopback(const UdpLoopback&)            = delete;
    UdpLoopback& operator=(const UdpLoopback&) = delete;

    SocketAddress address() const { return SocketAddress{Ipv4Address{{127, 0, 0, 1}}, port}; }

    /// Sends one datagram to `to`; true if all of it went.
    bool send_to(const UdpLoopback& to, const void* data, std::size_t size) const {
        const SocketAddress dest = to.address();
        auto n = detail::sys::udp_try_send(fd, static_cast<const std::byte*>(data), size, &dest);
        return n.has_value() && *n == size;
    }

    /// One non-blocking receive, or errno.
    std::expected<std::size_t, int> recv(void* data, std::size_t size) const {
        return detail::sys::udp_try_recv(fd, static_cast<std::byte*>(data), size, nullptr);
    }
};

} // namespace coro::test_sockets
