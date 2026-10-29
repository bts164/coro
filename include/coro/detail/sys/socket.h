#pragma once

// Socket backend seam pieces shared by every socket type (UDP, TCP): the result type
// of the non-blocking try_ functions, close, and the address parser
// SocketAddress::parse() uses.
//
// Current backends:
//   POSIX: src/detail/sys/posix/socket_posix.cpp
//   lwIP:  src/detail/sys/lwip/socket_lwip.cpp
//
// See doc/design/io_driver.md, "Porting to a new platform".

#include <coro/detail/sys/poller.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>

namespace coro::detail::sys {

/// Byte count on success; errno on failure. A would_block() errno means "wait for
/// readiness and retry" (see IoRegistration::poll_io).
using IoResult = std::expected<std::size_t, int>;

/// Closes a socket (or any other fd the driver watches, e.g. a FIFO or a notify
/// object). Deregister it from the driver first.
void close_socket(RawFd fd) noexcept;

/// Parses dotted-decimal IPv4 text (NUL-terminated) with the backend's own parser:
/// inet_pton() on POSIX, ip4addr_aton() on lwIP. Returns false if it is malformed.
bool parse_ipv4(const char* text, std::array<std::uint8_t, 4>& out) noexcept;

/// Parses IPv6 text (NUL-terminated, without a "%scope" suffix). Returns false if
/// it is malformed, and always on a backend without IPv6 (lwIP as configured).
bool parse_ipv6(const char* text, std::array<std::uint8_t, 16>& out) noexcept;

} // namespace coro::detail::sys
