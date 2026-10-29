// POSIX backend for the notify seam (include/coro/detail/sys/notify.h): an eventfd.

#include <coro/detail/sys/notify.h>

#include <cerrno>
#include <cstdint>
#include <system_error>

#include <sys/eventfd.h>
#include <unistd.h>

namespace coro::detail::sys {

RawFd notify_open() {
    const int fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (fd < 0) throw std::system_error(errno, std::generic_category(), "eventfd");
    return fd;
}

void notify_signal(RawFd fd) noexcept {
    // Every write wakes epoll, so an edge-triggered registration reports each
    // signal without the counter ever being read back. The counter itself cannot
    // reach its EAGAIN limit (2^64 - 2) in practice. write() is async-signal-safe.
    const std::uint64_t one = 1;
    [[maybe_unused]] const ssize_t n = ::write(fd, &one, sizeof(one));
}

} // namespace coro::detail::sys
