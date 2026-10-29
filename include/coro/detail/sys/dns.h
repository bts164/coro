#pragma once

// Backend seam for name resolution. It has two forms, chosen by kResolverBlocks:
//
//   Blocking (POSIX): resolve_host() calls getaddrinfo, so lookup_host runs it on the
//   Runtime's blocking pool (getaddrinfo has no non-blocking form, and glibc's
//   getaddrinfo_a uses threads internally anyway).
//
//   Registered query (lwIP): dns_start() starts an asynchronous lookup and returns a
//   query object that lookup_host registers with the driver like a socket; it turns
//   readable when the answer is in, and dns_try_result() takes it.
//
// Current backends:
//   POSIX: src/detail/sys/posix/dns_posix.cpp
//   lwIP:  src/detail/sys/lwip/dns_lwip.cpp
//
// See doc/design/file_io.md, "lookup_host", and doc/design/pico_port.md, "Name
// resolution".

#include <coro/detail/sys/poller.h>
#include <coro/io/socket_address.h>

#include <cstdint>
#include <expected>
#include <string>
#include <system_error>
#include <vector>

namespace coro::detail::sys {

#ifdef CORO_SYS_LWIP

/// lwIP resolves asynchronously: lookup_host waits on a registered query.
inline constexpr bool kResolverBlocks = false;

/// Starts resolving the name `host`. The returned query becomes readable once the
/// answer is in. Register it with the driver, deregister it and close it with
/// close_socket(), which may be done before the answer arrives.
/// @throws std::system_error with ENETDOWN on a runtime that does not run lwIP, or
///         ENOMEM / the lookup's own error if it cannot be started.
RawFd dns_start(const std::string& host);

/// Takes the query's answer, each address with `port`, into `out`. Returns an
/// empty error_code on success, `std::errc::resource_unavailable_try_again` while
/// the lookup is still running, and otherwise its failure (in dns_category()).
std::error_code dns_try_result(RawFd query, uint16_t port,
                               std::vector<SocketAddress>& out) noexcept;

#else

/// getaddrinfo blocks: lookup_host runs resolve_host() on the blocking pool.
inline constexpr bool kResolverBlocks = true;

/// Resolves `host` (a name or a numeric address) to every address it has, each with
/// `port`, in the resolver's preference order. Blocks. Uses the system resolver, so
/// /etc/hosts, nsswitch and search domains apply.
/// @return The addresses (never empty), or an error in dns_category() (or
///         std::system_category() for a system error inside the resolver).
std::expected<std::vector<SocketAddress>, std::error_code>
resolve_host(const std::string& host, uint16_t port);

#endif

/// The error category of the resolver's own failure codes (e.g. "Name or service not
/// known"). Named "coro.dns".
const std::error_category& dns_category() noexcept;

} // namespace coro::detail::sys
