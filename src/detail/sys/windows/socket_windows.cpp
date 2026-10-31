// Windows backend for the shared socket seam (include/coro/detail/sys/socket.h).

#include <coro/detail/sys/socket.h>

#include "win.h"

namespace coro::detail::sys {

using namespace win;

void close_socket(RawFd fd) noexcept {
    ::closesocket(to_socket(fd));
}

bool parse_ipv4(const char* text, std::array<std::uint8_t, 4>& out) noexcept {
    // inet_pton is part of Winsock, but needs no WSAStartup().
    return ::inet_pton(AF_INET, text, out.data()) == 1;
}

bool parse_ipv6(const char* text, std::array<std::uint8_t, 16>& out) noexcept {
    return ::inet_pton(AF_INET6, text, out.data()) == 1;
}

} // namespace coro::detail::sys
