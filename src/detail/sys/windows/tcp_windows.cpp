// Windows backend for the TCP seam (include/coro/detail/sys/tcp.h). See
// doc/design/windows_port.md, "TCP".

#include <coro/detail/sys/tcp.h>

#include "sockaddr_windows.h"
#include "win.h"

#include <cerrno>
#include <climits>
#include <variant>

namespace coro::detail::sys {

using namespace win;

namespace {

int family_of(const SocketAddress& addr) {
    return std::holds_alternative<Ipv4Address>(addr.address) ? AF_INET : AF_INET6;
}

[[noreturn]] void close_and_throw(SOCKET s, int err, const char* what) {
    ::closesocket(s);
    throw_errno(err, what);
}

// A non-blocking, non-inheritable stream socket. IPv6 sockets are made dual-stack, as
// they are by default on Linux and not on Windows.
SOCKET open_stream_socket(int family, const char* what) {
    winsock_init();
    const SOCKET s = ::WSASocketW(family, SOCK_STREAM, IPPROTO_TCP, nullptr, 0,
                                  WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
    if (s == INVALID_SOCKET) throw_errno(last_wsa_errno(), what);
    u_long nonblocking = 1;
    if (::ioctlsocket(s, FIONBIO, &nonblocking) != 0)
        close_and_throw(s, last_wsa_errno(), what);
    if (family == AF_INET6) {
        DWORD v6only = 0;
        (void)::setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY,
                           reinterpret_cast<const char*>(&v6only), sizeof(v6only));
    }
    return s;
}

int clamp_len(std::size_t size) noexcept {
    return static_cast<int>(size > static_cast<std::size_t>(INT_MAX) ? INT_MAX : size);
}

} // namespace

RawFd tcp_connect_start(const SocketAddress& peer) {
    const SOCKET s = open_stream_socket(family_of(peer), "TcpStream::connect");
    sockaddr_storage storage;
    const int addrlen = to_sockaddr(peer, storage);
    if (::connect(s, reinterpret_cast<const sockaddr*>(&storage), addrlen) != 0) {
        const int err = ::WSAGetLastError();
        // A non-blocking connect reports WSAEWOULDBLOCK where POSIX says EINPROGRESS.
        if (err != WSAEWOULDBLOCK && err != WSAEINPROGRESS)
            close_and_throw(s, wsa_to_errno(err), "TcpStream::connect");
    }
    return to_fd(s);
}

IoResult tcp_try_finish_connect(RawFd fd) noexcept {
    const SOCKET s = to_socket(fd);
    // A failed connect leaves its error in SO_ERROR (reading it clears it).
    int err = 0;
    int len = sizeof(err);
    if (::getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &len) != 0)
        return std::unexpected(last_wsa_errno());
    if (err != 0) return std::unexpected(wsa_to_errno(err));

    // SO_ERROR is also 0 while the handshake is still running, so ask whether the
    // connection is established. Not getpeername(), as on POSIX: on Windows it succeeds
    // while the handshake is still in progress (seen connecting to an IPv6 port nobody
    // listens on, during the SYN retries before the refusal), which would report a
    // connection that then fails. SO_CONNECT_TIME is all ones until the connection is
    // established.
    // Race (handled): the connect may fail between the two calls. It then reads as "in
    // progress"; the failure's AFD_POLL_CONNECT_FAIL event bumps the registration's
    // tick, so poll_io retries and the SO_ERROR check above sees it.
    DWORD seconds = 0;
    len = sizeof(seconds);
    if (::getsockopt(s, SOL_SOCKET, SO_CONNECT_TIME, reinterpret_cast<char*>(&seconds),
                     &len) != 0)
        return std::unexpected(last_wsa_errno());
    if (seconds == 0xFFFFFFFF) return std::unexpected(EAGAIN);
    return std::size_t{0};
}

RawFd tcp_listen(const SocketAddress& local, int backlog) {
    const SOCKET s = open_stream_socket(family_of(local), "TcpListener::bind");
    // Not SO_REUSEADDR: on Windows it lets another socket take over a bound port.
    // Binding while old connections sit in TIME_WAIT needs no option here.
    const BOOL exclusive = TRUE;
    if (::setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                     reinterpret_cast<const char*>(&exclusive), sizeof(exclusive)) != 0)
        close_and_throw(s, last_wsa_errno(), "TcpListener::bind");

    sockaddr_storage storage;
    const int addrlen = to_sockaddr(local, storage);
    if (::bind(s, reinterpret_cast<const sockaddr*>(&storage), addrlen) != 0)
        close_and_throw(s, last_wsa_errno(), "TcpListener::bind");
    if (::listen(s, backlog) != 0)
        close_and_throw(s, last_wsa_errno(), "TcpListener::bind");
    return to_fd(s);
}

std::expected<RawFd, int> tcp_try_accept(RawFd listener) noexcept {
    for (;;) {
        // The accepted socket inherits the listener's non-blocking mode.
        const SOCKET s = ::accept(to_socket(listener), nullptr, nullptr);
        if (s != INVALID_SOCKET) {
            // Potential race: until this call the socket is inheritable, so a process
            // created on another thread in between can inherit it. Windows has no
            // accept4().
            (void)::SetHandleInformation(reinterpret_cast<HANDLE>(s), HANDLE_FLAG_INHERIT, 0);
            return to_fd(s);
        }
        const int err = ::WSAGetLastError();
        // The connection died while queued: not the listener's problem, so try the
        // next one.
        if (err == WSAECONNRESET || err == WSAECONNABORTED || err == WSAEINTR) continue;
        return std::unexpected(wsa_to_errno(err));
    }
}

IoResult tcp_try_read(RawFd fd, std::byte* data, std::size_t size) noexcept {
    const int n = ::recv(to_socket(fd), reinterpret_cast<char*>(data), clamp_len(size), 0);
    if (n == SOCKET_ERROR) {
        const int err = ::WSAGetLastError();
        // An aborted connection reads as reset, as on POSIX.
        if (err == WSAECONNABORTED) return std::unexpected(ECONNRESET);
        return std::unexpected(wsa_to_errno(err));
    }
    return static_cast<std::size_t>(n);
}

IoResult tcp_try_write(RawFd fd, const std::byte* data, std::size_t size) noexcept {
    // No SIGPIPE on Windows: a write to a closed connection just fails.
    const int n = ::send(to_socket(fd), reinterpret_cast<const char*>(data), clamp_len(size), 0);
    if (n == SOCKET_ERROR) {
        const int err = ::WSAGetLastError();
        // The connection is gone: EPIPE, as POSIX reports it.
        if (err == WSAECONNABORTED || err == WSAESHUTDOWN) return std::unexpected(EPIPE);
        return std::unexpected(wsa_to_errno(err));
    }
    return static_cast<std::size_t>(n);
}

} // namespace coro::detail::sys
