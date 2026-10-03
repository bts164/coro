// POSIX backend for the shared socket seam (include/coro/detail/sys/socket.h).

#include <coro/detail/sys/socket.h>

#include <unistd.h>

namespace coro::detail::sys {

void close_socket(RawFd fd) noexcept {
    // EINTR is not retried: on Linux the fd is released even when close() is
    // interrupted, so a retry could close an fd another thread just opened.
    ::close(fd);
}

} // namespace coro::detail::sys
