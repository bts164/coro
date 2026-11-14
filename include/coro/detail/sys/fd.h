#pragma once

// Backend seam for AsyncFd (coro/io/async_fd.h): the one syscall it makes on an fd it
// is handed. Registering and closing the fd go through poller.h and socket.h.
//
// Current backends:
//   POSIX: src/detail/sys/posix/fd_posix.cpp
//
// See doc/design/io_driver.md, "`AsyncFd`".

#include <coro/detail/sys/poller.h>

namespace coro::detail::sys {

/// Sets O_NONBLOCK on `fd`'s open file description (so on every dup() of it too).
/// @throws std::system_error on failure (e.g. EBADF).
void fd_set_nonblocking(RawFd fd);

} // namespace coro::detail::sys
