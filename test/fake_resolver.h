#pragma once

// Replacing the system resolver in a test, so a test of how a lookup's result is used
// doesn't depend on the machine's DNS: a VPN's slow backup servers, or a container
// with no nameserver, can take 10 s or more to fail a lookup. Desktop only; lwIP
// resolves through a query of its own.
//
//   FakeResolver fake(resolve_nothing);   // every name fails, as an unknown one would
//
// The fake stays installed until the FakeResolver is destroyed. It is process-wide, but
// tests run one at a time, and the resolver is only called while the test runs.

#include <coro/detail/sys/dns.h>

#include <cstdint>
#include <expected>
#include <string>
#include <system_error>
#include <vector>

class FakeResolver {
public:
    explicit FakeResolver(coro::detail::sys::ResolveFn fn) noexcept {
        coro::detail::sys::set_resolver_for_testing(fn);
    }
    ~FakeResolver() { coro::detail::sys::set_resolver_for_testing(nullptr); }

    FakeResolver(const FakeResolver&)            = delete;
    FakeResolver& operator=(const FakeResolver&) = delete;
};

/// Fails every name with the error the real resolver gives for one that doesn't exist.
inline std::expected<std::vector<coro::SocketAddress>, std::error_code>
resolve_nothing(const std::string&, uint16_t) {
    return std::unexpected(coro::detail::sys::host_not_found_error());
}
