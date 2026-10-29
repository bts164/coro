// POSIX backend for the TCP seam (include/coro/detail/sys/tcp.h).

#include <coro/detail/sys/tcp.h>
#include "sockaddr_posix.h"

#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <system_error>
#include <variant>

namespace coro::detail::sys {

namespace {

[[noreturn]] void throw_errno(int err, const char* what) {
    throw std::system_error(err, std::system_category(), what);
}

int family_of(const SocketAddress& addr) {
    return std::holds_alternative<Ipv4Address>(addr.address) ? AF_INET : AF_INET6;
}

RawFd open_stream_socket(int family, const char* what) {
    const RawFd fd = ::socket(family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) throw_errno(errno, what);
#ifdef SO_NOSIGPIPE
    // No MSG_NOSIGNAL on this platform (see tcp_try_write); suppress SIGPIPE per socket.
    const int one = 1;
    (void)::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    return fd;
}

[[noreturn]] void close_and_throw(RawFd fd, int err, const char* what) {
    ::close(fd);
    throw_errno(err, what);
}

} // namespace

RawFd tcp_connect_start(const SocketAddress& peer) {
    const RawFd fd = open_stream_socket(family_of(peer), "TcpStream::connect");
    sockaddr_storage storage;
    const socklen_t addrlen = to_sockaddr(peer, storage);
    if (::connect(fd, reinterpret_cast<const sockaddr*>(&storage), addrlen) != 0) {
        const int err = errno;
        // EINTR: the connect carries on asynchronously, exactly like EINPROGRESS.
        if (err != EINPROGRESS && err != EINTR)
            close_and_throw(fd, err, "TcpStream::connect");
    }
    return fd;
}

IoResult tcp_try_finish_connect(RawFd fd) noexcept {
    // A failed connect leaves its errno in SO_ERROR (reading it clears it).
    int err = 0;
    socklen_t len = sizeof(err);
    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0) return std::unexpected(errno);
    if (err != 0) return std::unexpected(err);

    // SO_ERROR is also 0 while the handshake is still running, so ask whether there is
    // a peer yet.
    // Race (handled): the connect may fail between the two calls. getpeername() then
    // reports ENOTCONN, read here as "in progress"; the failure's EPOLLERR event bumps
    // the registration's tick, so poll_io retries and the SO_ERROR check above sees it.
    sockaddr_storage storage;
    socklen_t addrlen = sizeof(storage);
    if (::getpeername(fd, reinterpret_cast<sockaddr*>(&storage), &addrlen) == 0)
        return std::size_t{0};
    if (errno == ENOTCONN) return std::unexpected(EAGAIN);
    return std::unexpected(errno);
}

RawFd tcp_listen(const SocketAddress& local, int backlog) {
    const RawFd fd = open_stream_socket(family_of(local), "TcpListener::bind");
    // So a restarted server can rebind while old connections sit in TIME_WAIT.
    const int one = 1;
    if (::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) != 0)
        close_and_throw(fd, errno, "TcpListener::bind");

    sockaddr_storage storage;
    const socklen_t addrlen = to_sockaddr(local, storage);
    if (::bind(fd, reinterpret_cast<const sockaddr*>(&storage), addrlen) != 0)
        close_and_throw(fd, errno, "TcpListener::bind");
    if (::listen(fd, backlog) != 0)
        close_and_throw(fd, errno, "TcpListener::bind");
    return fd;
}

std::expected<RawFd, int> tcp_try_accept(RawFd listener) noexcept {
    for (;;) {
#if defined(__linux__) || defined(__FreeBSD__)
        const RawFd fd = ::accept4(listener, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
#else
        // Potential race: without accept4 the new fd is briefly inheritable, so a
        // fork+exec on another thread between accept() and fcntl() can leak it.
        RawFd fd = ::accept(listener, nullptr, nullptr);
        if (fd >= 0) {
            (void)::fcntl(fd, F_SETFD, FD_CLOEXEC);
            (void)::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
        }
#endif
        if (fd >= 0) return fd;
        const int err = errno;
        // The connection died while queued, or a signal interrupted us: neither is
        // the listener's problem, so try the next one.
        if (err == ECONNABORTED || err == EPROTO || err == EINTR) continue;
        return std::unexpected(err);
    }
}

IoResult tcp_try_read(RawFd fd, std::byte* data, std::size_t size) noexcept {
    const ssize_t n = ::recv(fd, data, size, MSG_DONTWAIT);
    if (n < 0) return std::unexpected(errno);
    return static_cast<std::size_t>(n);
}

IoResult tcp_try_write(RawFd fd, const std::byte* data, std::size_t size) noexcept {
#ifdef MSG_NOSIGNAL
    constexpr int flags = MSG_DONTWAIT | MSG_NOSIGNAL;
#else
    constexpr int flags = MSG_DONTWAIT;   // SO_NOSIGPIPE was set at socket creation
#endif
    const ssize_t n = ::send(fd, data, size, flags);
    if (n < 0) return std::unexpected(errno);
    return static_cast<std::size_t>(n);
}

} // namespace coro::detail::sys
