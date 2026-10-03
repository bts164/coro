#pragma once

// Socket backend seam pieces shared by every socket type (UDP, TCP): the result type
// of the non-blocking try_ functions, and close.
//
// Current backends:
//   POSIX: src/detail/sys/socket_posix.cpp
//
// See doc/design/io_driver.md, "Porting to a new platform".

#include <coro/detail/sys/poller.h>

#include <cstddef>
#include <expected>

namespace coro::detail::sys {

/// Byte count on success; errno on failure. A would_block() errno means "wait for
/// readiness and retry" (see IoRegistration::poll_io).
using IoResult = std::expected<std::size_t, int>;

/// Closes a socket (or any other fd the driver watches, e.g. a FIFO). Deregister it
/// from the driver first.
void close_socket(RawFd fd) noexcept;

} // namespace coro::detail::sys
