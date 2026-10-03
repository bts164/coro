// lookup_host() (doc/design/file_io.md, "lookup_host"). Names that need resolving use
// "localhost", which /etc/hosts answers without the network, or the RFC 6761 ".invalid"
// TLD, which never resolves.

#include <gtest/gtest.h>
#include <coro/io/lookup_host.h>
#include <coro/runtime/runtime.h>
#include <coro/runtime/current_thread_executor.h>
#include <coro/runtime/parker.h>
#include <coro/coro.h>
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

using namespace coro;

namespace {

bool is_loopback(const SocketAddress& addr) {
    if (auto* v4 = std::get_if<Ipv4Address>(&addr.address)) return v4->octets[0] == 127;
    const auto& v6 = std::get<Ipv6Address>(addr.address);
    std::array<uint8_t, 16> loopback{};
    loopback[15] = 1;
    return v6.octets == loopback;
}

// Awaits lookup_host(host) and returns the error it threw, or a default error_code.
Coro<std::error_code> lookup_error(std::string host) {
    try {
        auto addrs = co_await lookup_host(std::move(host), 80);
        (void)addrs;
    } catch (const std::system_error& e) {
        co_return e.code();
    }
    co_return std::error_code{};
}

} // namespace

// ---------------------------------------------------------------------------
// lookup_host
// ---------------------------------------------------------------------------

TEST(LookupHostTest, NumericLiteralsReturnThemselves) {
    Runtime rt;
    auto got = rt.block_on([]() -> Coro<std::pair<std::vector<SocketAddress>,
                                                  std::vector<SocketAddress>>> {
        auto v4 = co_await lookup_host("192.0.2.7", 1234);
        auto v6 = co_await lookup_host("::1", 4321);
        co_return std::pair{std::move(v4), std::move(v6)};
    }());
    ASSERT_EQ(got.first.size(), 1u);
    EXPECT_EQ(got.first[0], SocketAddress::parse("192.0.2.7", 1234).value());
    ASSERT_EQ(got.second.size(), 1u);
    EXPECT_EQ(got.second[0], SocketAddress::parse("::1", 4321).value());
}

TEST(LookupHostTest, LocalhostResolvesToLoopbackWithPort) {
    Runtime rt;
    auto addrs = rt.block_on([]() -> Coro<std::vector<SocketAddress>> {
        co_return co_await lookup_host("localhost", 8080);
    }());
    ASSERT_FALSE(addrs.empty());
    for (const auto& addr : addrs) {
        EXPECT_TRUE(is_loopback(addr)) << addr.to_string();
        EXPECT_EQ(addr.port, 8080);
    }
}

TEST(LookupHostTest, UnresolvableNameThrowsDnsError) {
    Runtime rt;
    auto code = rt.block_on(lookup_error("nonexistent.invalid"));
    EXPECT_TRUE(code);
    EXPECT_EQ(&code.category(), &dns_error_category()) << code.message();
}

// The resolver runs on the blocking pool, which doesn't need the IoDriver.
TEST(LookupHostTest, WorksWithoutDriver) {
    Runtime rt(std::in_place_type<CurrentThreadExecutor>,
               std::make_unique<PollingParker>([] {}));
    auto addrs = rt.block_on([]() -> Coro<std::vector<SocketAddress>> {
        co_return co_await lookup_host("localhost", 53);
    }());
    EXPECT_FALSE(addrs.empty());
}
