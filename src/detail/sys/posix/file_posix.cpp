// POSIX backend for the file seam (include/coro/detail/sys/file.h).

#include <coro/detail/sys/file.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>

namespace coro::detail::sys {

std::expected<RawFd, int> file_open(const std::string& path, FileOpenFlags flags) noexcept {
    int oflags = O_CLOEXEC;
    if (flags.read && flags.write) oflags |= O_RDWR;
    else if (flags.write)          oflags |= O_WRONLY;
    else                           oflags |= O_RDONLY;
    if (flags.create)     oflags |= O_CREAT;
    if (flags.truncate)   oflags |= O_TRUNC;
    if (flags.append)     oflags |= O_APPEND;
    if (flags.create_new) oflags |= O_CREAT | O_EXCL;
    for (;;) {
        const int fd = ::open(path.c_str(), oflags, 0644);
        if (fd >= 0) return fd;
        if (errno != EINTR) return std::unexpected(errno);
    }
}

IoResult file_read(RawFd fd, std::byte* data, std::size_t size, int64_t offset) noexcept {
    for (;;) {
        const ssize_t n = offset < 0 ? ::read(fd, data, size)
                                     : ::pread(fd, data, size, static_cast<off_t>(offset));
        if (n >= 0) return static_cast<std::size_t>(n);
        if (errno != EINTR) return std::unexpected(errno);
    }
}

IoResult file_write(RawFd fd, const std::byte* data, std::size_t size,
                    int64_t offset) noexcept {
    for (;;) {
        const ssize_t n = offset < 0 ? ::write(fd, data, size)
                                     : ::pwrite(fd, data, size, static_cast<off_t>(offset));
        if (n >= 0) return static_cast<std::size_t>(n);
        if (errno != EINTR) return std::unexpected(errno);
    }
}

std::expected<void, int> file_sync(RawFd fd) noexcept {
    for (;;) {
        if (::fsync(fd) == 0) return {};
        if (errno != EINTR) return std::unexpected(errno);
    }
}

std::expected<FileStat, int> file_stat(RawFd fd) noexcept {
    struct stat st{};
    if (::fstat(fd, &st) != 0) return std::unexpected(errno);
    FileStat out;
    out.size        = static_cast<uint64_t>(st.st_size);
    out.modified_ns = static_cast<int64_t>(st.st_mtim.tv_sec) * 1'000'000'000
                    + st.st_mtim.tv_nsec;
    out.regular     = S_ISREG(st.st_mode);
    return out;
}

void file_close(RawFd fd) noexcept {
    // Not retried on EINTR: on Linux the fd is released even then, and a retry could
    // close an fd another thread has just been given.
    (void)::close(fd);
}

} // namespace coro::detail::sys
