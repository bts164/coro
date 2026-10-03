// POSIX backend for the UDP seam (include/coro/detail/sys/udp.h).

#include <coro/detail/sys/udp.h>
#include "sockaddr_posix.h"

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <system_error>
#include <variant>

#ifdef __linux__
#include <netinet/udp.h>
#endif

namespace coro::detail::sys {

namespace {

[[noreturn]] void throw_errno(int err, const char* what) {
    throw std::system_error(err, std::system_category(), what);
}

void set_int_option(RawFd fd, int level, int name, int value, const char* what) {
    if (::setsockopt(fd, level, name, &value, sizeof(value)) != 0)
        throw_errno(errno, what);
}

} // namespace

RawFd udp_open(const SocketAddress& local) {
    const int family = std::holds_alternative<Ipv4Address>(local.address) ? AF_INET : AF_INET6;
    const RawFd fd = ::socket(family, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) throw_errno(errno, "UdpSocket::bind");

    sockaddr_storage storage;
    const socklen_t addrlen = to_sockaddr(local, storage);
    if (::bind(fd, reinterpret_cast<const sockaddr*>(&storage), addrlen) != 0) {
        const int err = errno;
        ::close(fd);
        throw_errno(err, "UdpSocket::bind");
    }
    return fd;
}

void udp_connect(RawFd fd, const SocketAddress& peer) {
    sockaddr_storage storage;
    const socklen_t addrlen = to_sockaddr(peer, storage);
    // A UDP connect() only records the peer; it never returns EINPROGRESS.
    if (::connect(fd, reinterpret_cast<const sockaddr*>(&storage), addrlen) != 0)
        throw_errno(errno, "UdpSocket::connect");
}

void udp_set_broadcast(RawFd fd, bool enabled) {
    set_int_option(fd, SOL_SOCKET, SO_BROADCAST, enabled ? 1 : 0, "UdpSocket::set_broadcast");
}

void udp_set_membership(RawFd fd, Ipv4Address group, Ipv4Address iface, bool join) {
    ip_mreq mreq{};
    std::memcpy(&mreq.imr_multiaddr, group.octets.data(), 4);
    std::memcpy(&mreq.imr_interface, iface.octets.data(), 4);   // all-zero = INADDR_ANY
    if (::setsockopt(fd, IPPROTO_IP, join ? IP_ADD_MEMBERSHIP : IP_DROP_MEMBERSHIP,
                     &mreq, sizeof(mreq)) != 0)
        throw_errno(errno, join ? "UdpSocket::join_multicast" : "UdpSocket::leave_multicast");
}

void udp_set_segment_size(RawFd fd, std::size_t bytes) {
#ifdef __linux__
    // Potential race: a send in flight on another worker may be segmented with
    // either the old or the new size. The kernel reads the option once per send,
    // so each send uses one or the other.
    set_int_option(fd, IPPROTO_UDP, UDP_SEGMENT, static_cast<int>(bytes),
                   "UdpSocket::set_segment_size");
#else
    (void)fd;
    (void)bytes;
    throw std::system_error(std::make_error_code(std::errc::not_supported),
                            "UdpSocket::set_segment_size");
#endif
}

void udp_set_gro(RawFd fd, bool enabled) {
#ifdef __linux__
    // Potential race: a datagram queued before the change keeps the form it was
    // queued in, so a receive just after set_gro(false) may still return a coalesced
    // buffer (which recv_segments_from() splits correctly; recv_from() would not).
    set_int_option(fd, IPPROTO_UDP, UDP_GRO, enabled ? 1 : 0, "UdpSocket::set_gro");
#else
    (void)fd;
    (void)enabled;
    throw std::system_error(std::make_error_code(std::errc::not_supported), "UdpSocket::set_gro");
#endif
}

IoResult udp_try_send(RawFd fd, const std::byte* data, std::size_t size,
                      const SocketAddress* dest) noexcept {
    ssize_t n;
    if (dest) {
        sockaddr_storage storage;
        const socklen_t addrlen = to_sockaddr(*dest, storage);
        n = ::sendto(fd, data, size, MSG_DONTWAIT,
                     reinterpret_cast<const sockaddr*>(&storage), addrlen);
    } else {
        // Connected socket: the kernel fails this with EDESTADDRREQ if unconnected.
        n = ::send(fd, data, size, MSG_DONTWAIT);
    }
    if (n < 0) return std::unexpected(errno);
    return static_cast<std::size_t>(n);
}

IoResult udp_try_recv(RawFd fd, std::byte* data, std::size_t size,
                      SocketAddress* sender) noexcept {
    ssize_t n;
    if (sender) {
        sockaddr_storage storage;
        socklen_t addrlen = sizeof(storage);
        n = ::recvfrom(fd, data, size, MSG_DONTWAIT,
                       reinterpret_cast<sockaddr*>(&storage), &addrlen);
        if (n >= 0) *sender = from_sockaddr(reinterpret_cast<const sockaddr*>(&storage));
    } else {
        n = ::recv(fd, data, size, MSG_DONTWAIT);
    }
    if (n < 0) return std::unexpected(errno);
    return static_cast<std::size_t>(n);
}

IoResult udp_try_recv_segments(RawFd fd, std::byte* data, std::size_t size,
                               SocketAddress& sender, std::size_t& segment_size) noexcept {
    sockaddr_storage storage;
    iovec iov{data, size};
    msghdr msg{};
    msg.msg_name    = &storage;
    msg.msg_namelen = sizeof(storage);
    msg.msg_iov     = &iov;
    msg.msg_iovlen  = 1;
#ifdef __linux__
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))];
    msg.msg_control    = control;
    msg.msg_controllen = sizeof(control);
#endif
    const ssize_t n = ::recvmsg(fd, &msg, MSG_DONTWAIT);
    if (n < 0) return std::unexpected(errno);
    sender = from_sockaddr(reinterpret_cast<const sockaddr*>(&storage));
    segment_size = static_cast<std::size_t>(n);
#ifdef __linux__
    for (cmsghdr* c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
        if (c->cmsg_level == IPPROTO_UDP && c->cmsg_type == UDP_GRO) {
            int gso_size;
            std::memcpy(&gso_size, CMSG_DATA(c), sizeof(gso_size));
            if (gso_size > 0) segment_size = static_cast<std::size_t>(gso_size);
        }
    }
#endif
    return static_cast<std::size_t>(n);
}

} // namespace coro::detail::sys
