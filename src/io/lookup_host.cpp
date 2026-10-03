// lookup_host: numeric addresses inline, names through the resolver seam
// (include/coro/detail/sys/dns.h) on the blocking pool. See doc/design/file_io.md,
// "lookup_host".

#include <coro/io/lookup_host.h>
#include <coro/detail/sys/dns.h>
#include <coro/task/spawn_blocking.h>

namespace coro {

Coro<std::vector<SocketAddress>> lookup_host(std::string host, uint16_t port) {
    if (auto numeric = SocketAddress::parse(host, port))
        co_return std::vector<SocketAddress>{*numeric};

    co_return co_await spawn_blocking([host, port]() -> std::vector<SocketAddress> {
        auto resolved = detail::sys::resolve_host(host, port);
        if (!resolved)
            throw std::system_error(resolved.error(), "coro::lookup_host: " + host);
        return std::move(*resolved);
    });
}

const std::error_category& dns_error_category() noexcept {
    return detail::sys::dns_category();
}

} // namespace coro
