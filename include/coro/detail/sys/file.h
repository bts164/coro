#pragma once

// Backend seam for regular files: every syscall File makes. Unlike the socket seams,
// these are ordinary blocking calls; File runs each one on the Runtime's blocking pool,
// because readiness-based reactors can't wait on regular files (epoll_ctl rejects them
// with EPERM; a disk read is always "ready" and then blocks). EINTR is retried here.
//
// Current backends:
//   POSIX: src/detail/sys/file_posix.cpp
//
// See doc/design/file_io.md.

#include <coro/detail/sys/socket.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>

namespace coro::detail::sys {

/// open() flags in backend-neutral form, translated by the backend.
struct FileOpenFlags {
    bool read     = false;
    bool write    = false;
    bool create   = false;
    bool truncate = false;
    bool append   = false;
};

/// Opens `path`, close-on-exec, creating it with mode 0644 (subject to umask) if
/// `flags.create` is set. Returns the fd, or errno.
std::expected<RawFd, int> file_open(const std::string& path, FileOpenFlags flags) noexcept;

/// One read() (`offset < 0`: at the file position, advancing it) or pread() (at
/// `offset`, leaving the position alone). 0 bytes means EOF. Returns errno on failure.
IoResult file_read(RawFd fd, std::byte* data, std::size_t size, int64_t offset) noexcept;

/// One write() or pwrite(), with the same `offset` convention as file_read. May write
/// fewer bytes than asked. Returns errno on failure.
IoResult file_write(RawFd fd, const std::byte* data, std::size_t size,
                    int64_t offset) noexcept;

/// fsync(): flushes the file's data and metadata to the device. Returns errno on failure.
std::expected<void, int> file_sync(RawFd fd) noexcept;

/// Closes `fd`. Errors are ignored: the fd is released either way, and a deferred write
/// error is what file_sync() is for.
void file_close(RawFd fd) noexcept;

} // namespace coro::detail::sys
