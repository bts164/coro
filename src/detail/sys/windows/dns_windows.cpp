// Windows backend for the name resolution seam (include/coro/detail/sys/dns.h). See
// doc/design/windows_port.md, "DNS".

#include <coro/detail/sys/dns.h>

#include "sockaddr_windows.h"
#include "win.h"

#include <memory>

namespace coro::detail::sys {

using namespace win;

namespace {

class DnsCategory final : public std::error_category {
public:
    const char* name() const noexcept override { return "coro.dns"; }
    // The codes are WSA codes (EAI_* are aliases of them). FormatMessageW rather than
    // gai_strerrorA(), which returns a static buffer and isn't thread-safe.
    std::string message(int code) const override {
        wchar_t* text = nullptr;
        const DWORD n = ::FormatMessageW(
            FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr, static_cast<DWORD>(code), 0, reinterpret_cast<wchar_t*>(&text), 0, nullptr);
        if (n == 0 || text == nullptr) return "name resolution error " + std::to_string(code);
        std::string out = to_utf8(text);
        ::LocalFree(text);
        while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' '))
            out.pop_back();
        return out;
    }
};

} // namespace

const std::error_category& dns_category() noexcept {
    static const DnsCategory category;
    return category;
}

std::error_code host_not_found_error() noexcept {
    return std::error_code(WSAHOST_NOT_FOUND, dns_category());
}

std::expected<std::vector<SocketAddress>, std::error_code>
resolve_host(const std::string& host, uint16_t port) {
    winsock_init();
    const std::wstring wide = to_wide(host);

    ADDRINFOW hints{};
    hints.ai_family = AF_UNSPEC;
    // Any one socket type, or each address comes back once per type. The address is
    // the same for UDP, so UdpSocket uses this too.
    hints.ai_socktype = SOCK_STREAM;

    ADDRINFOW* list = nullptr;
    const int rc = ::GetAddrInfoW(wide.c_str(), nullptr, &hints, &list);
    if (rc != 0) return std::unexpected(std::error_code(rc, dns_category()));
    std::unique_ptr<ADDRINFOW, decltype(&::FreeAddrInfoW)> owner(list, &::FreeAddrInfoW);

    std::vector<SocketAddress> out;
    for (const ADDRINFOW* ai = list; ai; ai = ai->ai_next) {
        if (ai->ai_family != AF_INET && ai->ai_family != AF_INET6) continue;
        SocketAddress addr = from_sockaddr(ai->ai_addr);
        addr.port = port;
        out.push_back(addr);
    }
    if (out.empty()) return std::unexpected(host_not_found_error());
    return out;
}

} // namespace coro::detail::sys
