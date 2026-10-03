// POSIX backend for the FIFO seam (include/coro/detail/sys/pipe.h).

#include <coro/detail/sys/pipe.h>

#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <system_error>

namespace coro::detail::sys {

namespace {

[[noreturn]] void throw_errno(int err, const char* what, const std::string& path) {
    throw std::system_error(err, std::system_category(), std::string(what) + ": " + path);
}

RawFd open_or_throw(const std::string& path, int flags, const char* what) {
    const RawFd fd = ::open(path.c_str(), flags | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) throw_errno(errno, what, path);
    return fd;
}

} // namespace

void fifo_create(const std::string& path, int permission) {
    if (::mkfifo(path.c_str(), static_cast<mode_t>(permission)) != 0 && errno != EEXIST)
        throw_errno(errno, "Pipe::create", path);
}

RawFd fifo_open_read(const std::string& path) {
    return open_or_throw(path, O_RDONLY, "Pipe::open");
}

RawFd fifo_open_read_write(const std::string& path) {
    // O_RDWR on a FIFO is left undefined by POSIX; Linux and the BSDs open it at once.
    return open_or_throw(path, O_RDWR, "Pipe::open");
}

std::expected<RawFd, int> fifo_try_open_write(const std::string& path) noexcept {
    const RawFd fd = ::open(path.c_str(), O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return std::unexpected(errno);
    return fd;
}

IoResult fifo_try_writer_seen(RawFd fd) noexcept {
    // Linux reports POLLHUP on a FIFO's read end only once a writer has come and gone
    // since this end was opened, never for "no writer yet". So POLLIN or POLLHUP here
    // means a writer has been there. A writer that has opened but not yet written shows
    // neither: Pipe::open keeps waiting until it writes or closes.
    // Potential race: other platforms may report POLLHUP for "no writer yet", which
    // would end the wait at once and turn the first read into an early EOF.
    pollfd pfd{fd, POLLIN, 0};
    const int n = ::poll(&pfd, 1, 0);
    if (n < 0) return std::unexpected(errno);
    if (n == 0 || (pfd.revents & (POLLIN | POLLHUP | POLLERR)) == 0)
        return std::unexpected(EAGAIN);
    return 0;
}

IoResult pipe_try_read(RawFd fd, std::byte* data, std::size_t size) noexcept {
    for (;;) {
        const ssize_t n = ::read(fd, data, size);
        if (n >= 0) return static_cast<std::size_t>(n);
        if (errno != EINTR) return std::unexpected(errno);
    }
}

IoResult pipe_try_write(RawFd fd, const std::byte* data, std::size_t size) noexcept {
    for (;;) {
        const ssize_t n = ::write(fd, data, size);
        if (n >= 0) return static_cast<std::size_t>(n);
        if (errno != EINTR) return std::unexpected(errno);
    }
}

} // namespace coro::detail::sys
