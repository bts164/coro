// POSIX backend for the shared socket seam (include/coro/detail/sys/socket.h).

#include <coro/detail/sys/socket.h>

#include <arpa/inet.h>
#include <unistd.h>

namespace coro::detail::sys {

void close_socket(RawFd fd) noexcept {
    // EINTR is not retried: on Linux the fd is released even when close() is
    // interrupted, so a retry could close an fd another thread just opened.
    ::close(fd);
}

bool parse_ipv4(const char* text, std::array<std::uint8_t, 4>& out) noexcept {
    return ::inet_pton(AF_INET, text, out.data()) == 1;
}

bool parse_ipv6(const char* text, std::array<std::uint8_t, 16>& out) noexcept {
    return ::inet_pton(AF_INET6, text, out.data()) == 1;
}

} // namespace coro::detail::sys
