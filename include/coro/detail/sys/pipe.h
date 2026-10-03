#pragma once

// Backend seam for named pipes (FIFOs): every syscall Pipe makes. Setup functions
// throw std::system_error; the try_ functions never block and return an errno, where
// EAGAIN means "wait for readiness and retry" (IoRegistration::poll_io).
//
// Current backends:
//   POSIX: src/detail/sys/pipe_posix.cpp
//
// See doc/design/pipe_streaming.md, "The sys/pipe.h seam".

#include <coro/detail/sys/socket.h>

#include <cstddef>
#include <expected>
#include <string>

namespace coro::detail::sys {

/// Creates a FIFO at `path` with `permission` (subject to umask). An existing file at
/// `path` is not an error, whatever its type.
/// @throws std::system_error on any other failure.
void fifo_create(const std::string& path, int permission);

/// Opens the read end of the FIFO at `path`, non-blocking and close-on-exec. Succeeds
/// whether or not a writer has it open.
/// @throws std::system_error on failure (e.g. ENOENT).
RawFd fifo_open_read(const std::string& path);

/// Opens the FIFO at `path` for reading and writing, non-blocking and close-on-exec.
/// Never waits for a peer, and never reads EOF (it is a writer itself).
/// @throws std::system_error on failure.
RawFd fifo_open_read_write(const std::string& path);

/// One attempt to open the write end of the FIFO at `path`, non-blocking and
/// close-on-exec. ENXIO means no reader has it open yet; the kernel gives no
/// readiness event for a reader arriving, so the caller retries on a timer.
std::expected<RawFd, int> fifo_try_open_write(const std::string& path) noexcept;

/// Checks, without consuming anything, whether the read end opened by fifo_open_read()
/// has data or has seen a writer come and go. Returns 0 if so, EAGAIN if neither yet.
/// Until then a read() would return 0 as if at EOF, so Pipe::open waits on this.
IoResult fifo_try_writer_seen(RawFd fd) noexcept;

/// One non-blocking read of up to `size` bytes. 0 means every writer has closed.
IoResult pipe_try_read(RawFd fd, std::byte* data, std::size_t size) noexcept;

/// One non-blocking write of up to `size` bytes; may write fewer. A write after the
/// reader closed fails with EPIPE, and also raises SIGPIPE: unlike send(), write()
/// has no per-call flag to suppress it.
IoResult pipe_try_write(RawFd fd, const std::byte* data, std::size_t size) noexcept;

} // namespace coro::detail::sys
