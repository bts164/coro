// lookup_host: numeric addresses inline, names through the resolver seam
// (include/coro/detail/sys/dns.h): on the blocking pool where the resolver blocks
// (POSIX), as a query registered with the driver where it does not (lwIP). See
// doc/design/file_io.md, "lookup_host".

#include <coro/io/lookup_host.h>
#include <coro/detail/sys/dns.h>

#ifdef CORO_SYS_LWIP
#include <coro/detail/context.h>
#include <coro/detail/poll_result.h>
#include <coro/detail/socket_state.h>
#include <coro/runtime/io_driver.h>
#include <exception>
#include <memory>
#else
#include <coro/task/spawn_blocking.h>
#endif

namespace coro {

#ifdef CORO_SYS_LWIP

namespace {

/// Waits for a registered DNS query's answer. A leaf future: dropping it drops the
/// SocketState, which closes the query (lwIP lets the lookup finish and discards
/// the answer).
class DnsQueryFuture {
public:
    using OutputType = std::vector<SocketAddress>;

    DnsQueryFuture(std::shared_ptr<detail::SocketState> state, std::string host,
                   uint16_t port)
        : m_state(std::move(state)), m_host(std::move(host)), m_port(port) {}

    PollResult<OutputType> poll(detail::Context& ctx) {
        for (;;) {
            auto ready = m_state->reg.poll_ready(IoDirection::Read, ctx);
            if (!ready) return PollPending;
            if (ready->shutdown)
                return PollError(std::make_exception_ptr(std::system_error(
                    detail::sys::kDriverShutDown, std::generic_category(),
                    "coro::lookup_host: " + m_host)));
            OutputType out;
            const std::error_code ec = detail::sys::dns_try_result(m_state->fd, m_port, out);
            if (ec == std::errc::resource_unavailable_try_again) {
                // Race (handled): an answer that arrived after the check above bumped
                // the registration's tick, so clear_ready() leaves it ready.
                m_state->reg.clear_ready(*ready);
                continue;
            }
            if (ec)
                return PollError(std::make_exception_ptr(
                    std::system_error(ec, "coro::lookup_host: " + m_host)));
            return out;
        }
    }

private:
    std::shared_ptr<detail::SocketState> m_state;
    std::string                          m_host;
    uint16_t                             m_port;
};

} // namespace

Coro<std::vector<SocketAddress>> lookup_host(std::string host, uint16_t port) {
    if (auto numeric = SocketAddress::parse(host, port))
        co_return std::vector<SocketAddress>{*numeric};

    IoDriver& driver = detail::socket_io_driver("coro::lookup_host");
    auto state = std::make_shared<detail::SocketState>(driver, detail::sys::dns_start(host));
    co_return co_await DnsQueryFuture(std::move(state), std::move(host), port);
}

#else

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

#endif

const std::error_category& dns_error_category() noexcept {
    return detail::sys::dns_category();
}

} // namespace coro
