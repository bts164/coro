// The system resolver itself, through lookup_host(): what it reports for a name that
// doesn't exist. The one test that asks the machine's DNS, which can take 10 s or more
// to fail where no nameserver answers (a VPN's slow backup servers, a container with
// none), so it is its own executable, labelled "network": `ctest -LE network` skips
// it. Every other test that needs a failed lookup fakes the resolver
// (fake_resolver.h).

#include <gtest/gtest.h>
#include <coro/io/lookup_host.h>
#include <coro/runtime/runtime.h>
#include <coro/coro.h>
#include <string>
#include <system_error>
#include <utility>

using namespace coro;

// RFC 6761 reserves ".invalid": it never resolves. Depending on the resolver the
// error is "not found", or "try again" where no nameserver answered; either way it is
// in the DNS category.
TEST(SystemResolverTest, UnresolvableNameThrowsDnsError) {
    Runtime rt;
    auto code = rt.block_on([]() -> Coro<std::error_code> {
        try {
            auto addrs = co_await lookup_host("nonexistent.invalid", 80);
            (void)addrs;
        } catch (const std::system_error& e) {
            co_return e.code();
        }
        co_return std::error_code{};
    }());
    EXPECT_TRUE(code);
    EXPECT_EQ(&code.category(), &dns_error_category()) << code.message();
}
