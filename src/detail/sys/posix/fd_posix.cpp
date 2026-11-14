#include <coro/detail/sys/fd.h>

#include <fcntl.h>

#include <cerrno>
#include <system_error>

namespace coro::detail::sys {

void fd_set_nonblocking(RawFd fd) {
    const int flags = ::fcntl(fd, F_GETFL);
    if (flags == -1)
        throw std::system_error(errno, std::generic_category(), "fcntl(F_GETFL)");
    if ((flags & O_NONBLOCK) != 0) return;
    if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1)
        throw std::system_error(errno, std::generic_category(), "fcntl(F_SETFL, O_NONBLOCK)");
}

} // namespace coro::detail::sys
