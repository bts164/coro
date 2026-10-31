// lwIP backend for the resolver seam (include/coro/detail/sys/dns.h): each lookup is
// a query object registered with the driver, made readable by lwIP's found callback.
// See doc/design/pico_port.md, "Name resolution".

#include "io_object.h"

#include <coro/detail/sys/dns.h>

#include <lwip/dns.h>

#include <cerrno>
#include <memory>
#include <string>
#include <system_error>

namespace coro::detail::sys {

namespace {

// The codes of dns_category() on this backend. lwIP reports only "found" or "not".
constexpr int kHostNotFound = 1;

class DnsCategory final : public std::error_category {
public:
    const char* name() const noexcept override { return "coro.dns"; }
    std::string message(int code) const override {
        return code == kHostNotFound ? "Host not found" : "Unknown DNS error";
    }
};

// Runs on the executor's thread inside Poller::poll(). lwIP always calls it in the
// end, with null after its retries if the name did not resolve, so an orphaned
// query is deleted here.
void on_found(const char* /*name*/, const ip_addr_t* addr, void* arg) {
    auto* query = static_cast<LwipDnsQuery*>(arg);
    if (query->orphaned) {
        delete query;
        return;
    }
    query->done = true;
    if (addr != nullptr) {
        query->found = true;
        query->addr  = *addr;
    }
    mark_ready(query, IoObject::kReadable);
}

} // namespace

const std::error_category& dns_category() noexcept {
    static const DnsCategory category;
    return category;
}

std::error_code host_not_found_error() noexcept {
    return std::error_code(kHostNotFound, dns_category());
}

RawFd dns_start(const std::string& host) {
    require_lwip("coro::lookup_host");
    auto query = std::make_unique<LwipDnsQuery>();
    ip_addr_t addr;
    const err_t err = dns_gethostbyname(host.c_str(), &addr, &on_found, query.get());
    if (err == ERR_OK) {
        // Answered from lwIP's cache: readable at once (a new registration starts
        // ready, so the first poll takes it).
        query->done  = true;
        query->found = true;
        query->addr  = addr;
    } else if (err != ERR_INPROGRESS) {
        // ERR_ARG: a malformed name. ERR_VAL: no DNS server, or a name lwIP cannot
        // look up. Both mean this name does not resolve here.
        if (err == ERR_ARG || err == ERR_VAL)
            throw std::system_error(host_not_found_error(), "coro::lookup_host: " + host);
        throw_errno(lwip_errno(err), "coro::lookup_host");
    }
    return query.release();
}

std::error_code dns_try_result(RawFd fd, uint16_t port,
                               std::vector<SocketAddress>& out) noexcept {
    auto* query = static_cast<LwipDnsQuery*>(fd);
    if (!query->done) return std::make_error_code(std::errc::resource_unavailable_try_again);
    if (!query->found) return host_not_found_error();
    out.assign(1, from_ip_addr(query->addr, port));
    return {};
}

void close_dns_query(LwipDnsQuery* query) noexcept {
    // lwIP's DNS has no cancel: a pending query is orphaned, and on_found deletes it.
    if (!query->done) {
        query->orphaned = true;
        return;
    }
    delete query;
}

} // namespace coro::detail::sys
