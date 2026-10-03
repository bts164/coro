#pragma once

// TCP backend seam: the only place a TCP socket syscall is made.
//
// TcpStream's and TcpListener's futures call these and nothing else, so a new
// platform supplies this file's functions and reuses both types unchanged. See
// doc/design/io_driver.md, "Porting to a new platform", and doc/design/tcp_stream.md,
// "The sys/tcp.h seam".
//
// Current backends:
//   POSIX: src/detail/sys/tcp_posix.cpp
//
// The try_ functions never block. They return a result or an errno; a would_block()
// errno means "wait for readiness and retry" (see IoRegistration::poll_io). Setup
// functions throw std::system_error.

#include <coro/detail/sys/socket.h>
#include <coro/io/socket_address.h>

#include <cstddef>
#include <expected>

namespace coro::detail::sys {

/// Creates a non-blocking, close-on-exec TCP socket of `peer`'s address family and
/// starts connecting it to `peer`. The connection may still be in progress on return:
/// finish it with tcp_try_finish_connect().
/// @throws std::system_error if the socket can't be created or the connect fails at
///         once (nothing is leaked).
RawFd tcp_connect_start(const SocketAddress& peer);

/// Completes a connect started by tcp_connect_start(). Returns 0 once connected, an
/// EAGAIN error while the handshake is still in progress (wait for write readiness),
/// or the connect's errno (e.g. ECONNREFUSED) if it failed.
IoResult tcp_try_finish_connect(RawFd fd) noexcept;

/// Creates a non-blocking, close-on-exec TCP socket of `local`'s address family with
/// SO_REUSEADDR, binds it to `local` and listens with `backlog`.
/// @throws std::system_error on failure (nothing is leaked).
RawFd tcp_listen(const SocketAddress& local, int backlog);

/// One non-blocking accept. The new socket is non-blocking and close-on-exec. Errors
/// that only concern the aborted connection (ECONNABORTED, EPROTO, EINTR) are skipped
/// internally, so an error returned here concerns the listener (e.g. EMFILE).
std::expected<RawFd, int> tcp_try_accept(RawFd listener) noexcept;

/// One non-blocking read of up to `size` bytes. 0 means the peer closed its side.
IoResult tcp_try_read(RawFd fd, std::byte* data, std::size_t size) noexcept;

/// One non-blocking write of up to `size` bytes; may write fewer. Never raises
/// SIGPIPE: a write to a closed connection fails with EPIPE instead.
IoResult tcp_try_write(RawFd fd, const std::byte* data, std::size_t size) noexcept;

} // namespace coro::detail::sys
