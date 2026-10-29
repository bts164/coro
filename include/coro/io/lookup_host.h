#pragma once

// Hostname resolution. On desktop the system resolver runs on the Runtime's blocking
// pool; on Pico lwIP's DNS client answers a query registered with the driver. See
// doc/design/file_io.md, "lookup_host".

#include <coro/coro.h>
#include <coro/io/socket_address.h>

#include <cstdint>
#include <string>
#include <system_error>
#include <vector>

namespace coro {

/**
 * @brief Resolves `host` to its addresses, each with `port`, like tokio's
 * `lookup_host`.
 *
 * A numeric address ("127.0.0.1", "::1", "fe80::1%3") is parsed in place and returns
 * at once. Anything else goes to the system resolver (getaddrinfo) on the Runtime's
 * blocking pool, so /etc/hosts, nsswitch and search domains apply. The addresses come
 * back in the resolver's preference order; connect to them in that order.
 *
 * On Pico a name goes to lwIP's DNS client instead, as a query registered with the
 * runtime's driver, and resolves to one IPv4 address. A runtime that does not run
 * lwIP (PicoNetwork::None) fails with ENETDOWN.
 *
 * Dropping the future doesn't cancel the lookup: it finishes (on the pool, or in
 * lwIP) and the result is discarded.
 *
 * Needs a Runtime: for its blocking pool on desktop, where it need not turn the
 * IoDriver; on Pico for its driver.
 *
 * @throws std::system_error (at co_await) if the name doesn't resolve, with a code in
 *         @ref dns_error_category() (e.g. "Name or service not known"), or in
 *         `std::system_category()` for a system error inside the resolver.
 */
[[nodiscard]] Coro<std::vector<SocketAddress>> lookup_host(std::string host, uint16_t port);

/// The category of lookup_host's resolver failures, named "coro.dns". Codes are the
/// platform's getaddrinfo codes (EAI_NONAME, EAI_AGAIN, ...).
const std::error_category& dns_error_category() noexcept;

} // namespace coro
