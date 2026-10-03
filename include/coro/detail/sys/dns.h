#pragma once

// Backend seam for name resolution. A blocking call: lookup_host runs it on the
// Runtime's blocking pool, because getaddrinfo has no non-blocking form (glibc's
// getaddrinfo_a uses threads internally anyway).
//
// Current backends:
//   POSIX: src/detail/sys/dns_posix.cpp
//
// See doc/design/file_io.md, "lookup_host".

#include <coro/io/socket_address.h>

#include <cstdint>
#include <expected>
#include <string>
#include <system_error>
#include <vector>

namespace coro::detail::sys {

/// Resolves `host` (a name or a numeric address) to every address it has, each with
/// `port`, in the resolver's preference order. Blocks. Uses the system resolver, so
/// /etc/hosts, nsswitch and search domains apply.
/// @return The addresses (never empty), or an error in dns_category() (or
///         std::system_category() for a system error inside the resolver).
std::expected<std::vector<SocketAddress>, std::error_code>
resolve_host(const std::string& host, uint16_t port);

/// The error category of the resolver's own failure codes (e.g. "Name or service not
/// known"). Named "coro.dns".
const std::error_category& dns_category() noexcept;

} // namespace coro::detail::sys
