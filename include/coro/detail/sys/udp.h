#pragma once

// UDP backend seam: the only place a UDP socket syscall is made.
//
// UdpSocket's futures call these and nothing else, so a new platform supplies this
// file's functions and reuses UdpSocket unchanged. See doc/design/io_driver.md,
// "Porting to a new platform", and doc/design/udp_socket.md, "Desktop (IoDriver)
// backend".
//
// Current backends:
//   POSIX: src/detail/sys/udp_posix.cpp
//
// The try_ functions never block. They return the byte count, or an errno; a
// would_block() errno means "wait for readiness and retry" (see
// IoRegistration::poll_io). Setup functions throw std::system_error.

#include <coro/detail/sys/socket.h>
#include <coro/io/socket_address.h>

#include <cstddef>

namespace coro::detail::sys {

/// Creates a non-blocking, close-on-exec UDP socket of `local`'s address family and
/// binds it to `local`.
/// @throws std::system_error on failure (nothing is leaked).
RawFd udp_open(const SocketAddress& local);

/// Fixes `peer` as the socket's only correspondent (connect(2)).
/// @throws std::system_error on failure.
void udp_connect(RawFd fd, const SocketAddress& peer);

/// SO_BROADCAST. @throws std::system_error on failure.
void udp_set_broadcast(RawFd fd, bool enabled);

/// IP_ADD_MEMBERSHIP (`join`) or IP_DROP_MEMBERSHIP. An all-zero `iface` lets the OS
/// choose the interface. @throws std::system_error on failure.
void udp_set_membership(RawFd fd, Ipv4Address group, Ipv4Address iface, bool join);

/// UDP_SEGMENT (Linux GSO); 0 disables.
/// @throws std::system_error on failure, or ENOTSUP off Linux.
void udp_set_segment_size(RawFd fd, std::size_t bytes);

/// UDP_GRO (Linux). @throws std::system_error on failure, or ENOTSUP off Linux.
void udp_set_gro(RawFd fd, bool enabled);

/// One non-blocking send of one datagram (or one GSO buffer). `dest == nullptr` sends
/// to the connected peer.
IoResult udp_try_send(RawFd fd, const std::byte* data, std::size_t size,
                      const SocketAddress* dest) noexcept;

/// One non-blocking receive of one datagram, truncated to `size`. Fills `*sender`
/// when it is non-null.
IoResult udp_try_recv(RawFd fd, std::byte* data, std::size_t size,
                      SocketAddress* sender) noexcept;

/// Like udp_try_recv(), but also reports the UDP_GRO segment size, which equals the
/// returned size when the kernel did not coalesce (and always off Linux).
IoResult udp_try_recv_segments(RawFd fd, std::byte* data, std::size_t size,
                               SocketAddress& sender, std::size_t& segment_size) noexcept;

} // namespace coro::detail::sys
