// Windows backend for the UDP seam (include/coro/detail/sys/udp.h). See
// doc/design/windows_port.md, "UDP".

#include <coro/detail/sys/udp.h>

#include "sockaddr_windows.h"
#include "win.h"

#include <cerrno>
#include <climits>
#include <cstring>
#include <system_error>
#include <variant>

#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

namespace coro::detail::sys {

using namespace win;

namespace {

void set_int_option(RawFd fd, int level, int name, int value, const char* what) {
    if (::setsockopt(to_socket(fd), level, name, reinterpret_cast<const char*>(&value),
                     sizeof(value)) != 0)
        throw_errno(last_wsa_errno(), what);
}

int clamp_len(std::size_t size) noexcept {
    return static_cast<int>(size > static_cast<std::size_t>(INT_MAX) ? INT_MAX : size);
}

// One receive. A datagram longer than the buffer fails with WSAEMSGSIZE although the
// buffer was filled: that is the truncated read POSIX returns without MSG_TRUNC.
IoResult recv_one(RawFd fd, std::byte* data, std::size_t size, SocketAddress* sender) noexcept {
    sockaddr_storage storage{};
    int addrlen = sizeof(storage);
    const int len = clamp_len(size);
    const int n = sender
        ? ::recvfrom(to_socket(fd), reinterpret_cast<char*>(data), len, 0,
                     reinterpret_cast<sockaddr*>(&storage), &addrlen)
        : ::recv(to_socket(fd), reinterpret_cast<char*>(data), len, 0);
    std::size_t got;
    if (n != SOCKET_ERROR) {
        got = static_cast<std::size_t>(n);
    } else {
        const int err = ::WSAGetLastError();
        if (err != WSAEMSGSIZE) return std::unexpected(wsa_to_errno(err));
        got = static_cast<std::size_t>(len);
    }
    if (sender) *sender = from_sockaddr(reinterpret_cast<const sockaddr*>(&storage));
    return got;
}

} // namespace

RawFd udp_open(const SocketAddress& local) {
    winsock_init();
    const int family = std::holds_alternative<Ipv4Address>(local.address) ? AF_INET : AF_INET6;
    const SOCKET s = ::WSASocketW(family, SOCK_DGRAM, IPPROTO_UDP, nullptr, 0,
                                  WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
    if (s == INVALID_SOCKET) throw_errno(last_wsa_errno(), "UdpSocket::bind");

    auto fail = [s](int err) {
        ::closesocket(s);
        throw_errno(err, "UdpSocket::bind");
    };
    u_long nonblocking = 1;
    if (::ioctlsocket(s, FIONBIO, &nonblocking) != 0) fail(last_wsa_errno());
    // Windows reports an ICMP "port unreachable" for an earlier send as a
    // WSAECONNRESET on the next receive, even on an unconnected socket.
    BOOL report_resets = FALSE;
    DWORD bytes = 0;
    (void)::WSAIoctl(s, SIO_UDP_CONNRESET, &report_resets, sizeof(report_resets), nullptr, 0,
                     &bytes, nullptr, nullptr);
    if (family == AF_INET6) {
        // Dual-stack, as Linux is by default.
        DWORD v6only = 0;
        (void)::setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY,
                           reinterpret_cast<const char*>(&v6only), sizeof(v6only));
    }

    sockaddr_storage storage;
    const int addrlen = to_sockaddr(local, storage);
    if (::bind(s, reinterpret_cast<const sockaddr*>(&storage), addrlen) != 0)
        fail(last_wsa_errno());
    return to_fd(s);
}

void udp_connect(RawFd fd, const SocketAddress& peer) {
    sockaddr_storage storage;
    const int addrlen = to_sockaddr(peer, storage);
    if (::connect(to_socket(fd), reinterpret_cast<const sockaddr*>(&storage), addrlen) != 0)
        throw_errno(last_wsa_errno(), "UdpSocket::connect");
}

void udp_set_broadcast(RawFd fd, bool enabled) {
    set_int_option(fd, SOL_SOCKET, SO_BROADCAST, enabled ? 1 : 0, "UdpSocket::set_broadcast");
}

void udp_set_membership(RawFd fd, Ipv4Address group, Ipv4Address iface, bool join) {
    ip_mreq mreq{};
    std::memcpy(&mreq.imr_multiaddr, group.octets.data(), 4);
    std::memcpy(&mreq.imr_interface, iface.octets.data(), 4);   // all-zero = INADDR_ANY
    if (::setsockopt(to_socket(fd), IPPROTO_IP, join ? IP_ADD_MEMBERSHIP : IP_DROP_MEMBERSHIP,
                     reinterpret_cast<const char*>(&mreq), sizeof(mreq)) != 0)
        throw_errno(last_wsa_errno(),
                    join ? "UdpSocket::join_multicast" : "UdpSocket::leave_multicast");
}

// TODO: Windows has UDP_SEND_MSG_SIZE (USO) and UDP_RECV_MAX_COALESCED_SIZE (URO); see
// doc/design/windows_port.md, "UDP".
void udp_set_segment_size(RawFd, std::size_t) {
    throw std::system_error(std::make_error_code(std::errc::not_supported),
                            "UdpSocket::set_segment_size");
}

void udp_set_gro(RawFd, bool) {
    throw std::system_error(std::make_error_code(std::errc::not_supported), "UdpSocket::set_gro");
}

IoResult udp_try_send(RawFd fd, const std::byte* data, std::size_t size,
                      const SocketAddress* dest) noexcept {
    int n;
    if (dest) {
        sockaddr_storage storage;
        const int addrlen = to_sockaddr(*dest, storage);
        n = ::sendto(to_socket(fd), reinterpret_cast<const char*>(data), clamp_len(size), 0,
                     reinterpret_cast<const sockaddr*>(&storage), addrlen);
    } else {
        n = ::send(to_socket(fd), reinterpret_cast<const char*>(data), clamp_len(size), 0);
    }
    if (n == SOCKET_ERROR) {
        const int err = ::WSAGetLastError();
        // send() on an unconnected socket: EDESTADDRREQ, as POSIX reports it.
        if (!dest && err == WSAENOTCONN) return std::unexpected(EDESTADDRREQ);
        return std::unexpected(wsa_to_errno(err));
    }
    return static_cast<std::size_t>(n);
}

IoResult udp_try_recv(RawFd fd, std::byte* data, std::size_t size,
                      SocketAddress* sender) noexcept {
    return recv_one(fd, data, size, sender);
}

IoResult udp_try_recv_segments(RawFd fd, std::byte* data, std::size_t size,
                               SocketAddress& sender, std::size_t& segment_size) noexcept {
    // No GRO here: every receive is one datagram.
    auto n = recv_one(fd, data, size, &sender);
    if (n) segment_size = *n;
    return n;
}

} // namespace coro::detail::sys
