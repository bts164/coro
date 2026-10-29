// POSIX backend for the name resolution seam (include/coro/detail/sys/dns.h).

#include <coro/detail/sys/dns.h>
#include "sockaddr_posix.h"

#include <netdb.h>
#include <sys/socket.h>

#include <cerrno>
#include <memory>

namespace coro::detail::sys {

namespace {

class DnsCategory final : public std::error_category {
public:
    const char* name() const noexcept override { return "coro.dns"; }
    std::string message(int code) const override { return ::gai_strerror(code); }
};

} // namespace

const std::error_category& dns_category() noexcept {
    static const DnsCategory category;
    return category;
}

std::expected<std::vector<SocketAddress>, std::error_code>
resolve_host(const std::string& host, uint16_t port) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    // Any one socket type, or each address comes back once per type (stream, datagram,
    // raw). The address is the same for UDP, so UdpSocket uses this too.
    hints.ai_socktype = SOCK_STREAM;
    // No AI_ADDRCONFIG: it drops ::1 for "localhost" on a host whose only IPv6
    // address is loopback, as std and tokio also avoid.

    addrinfo* list = nullptr;
    const int rc = ::getaddrinfo(host.c_str(), nullptr, &hints, &list);
    if (rc == EAI_SYSTEM) return std::unexpected(std::error_code(errno, std::system_category()));
    if (rc != 0)          return std::unexpected(std::error_code(rc, dns_category()));
    std::unique_ptr<addrinfo, decltype(&::freeaddrinfo)> owner(list, &::freeaddrinfo);

    std::vector<SocketAddress> out;
    for (const addrinfo* ai = list; ai; ai = ai->ai_next) {
        if (ai->ai_family != AF_INET && ai->ai_family != AF_INET6) continue;
        SocketAddress addr = from_sockaddr(ai->ai_addr);
        addr.port = port;
        out.push_back(addr);
    }
    if (out.empty()) return std::unexpected(std::error_code(EAI_NONAME, dns_category()));
    return out;
}

} // namespace coro::detail::sys
