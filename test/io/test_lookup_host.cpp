// lookup_host() (doc/design/file_io.md, "lookup_host") and hostnames in connect() and
// bind(). Names that need resolving use "localhost", which the hosts file answers
// without asking DNS. A failed lookup, or a chosen list of addresses, comes from a fake
// resolver (fake_resolver.h); what the real one reports for a name that doesn't exist
// is test_system_resolver.cpp's.

#include <gtest/gtest.h>
#include "fake_resolver.h"
#include <coro/detail/sys/dns.h>
#include <coro/io/lookup_host.h>
#include <coro/io/tcp_listener.h>
#include <coro/io/tcp_stream.h>
#include <coro/io/udp_socket.h>
#include <coro/runtime/runtime.h>
#include <coro/runtime/current_thread_executor.h>
#include <coro/runtime/parker.h>
#include <coro/coro.h>
#include <array>
#include <cstdint>
#include <expected>
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

// The resolver's failure comes out of lookup_host() as it is.
TEST(LookupHostTest, UnresolvableNameThrowsDnsError) {
    FakeResolver fake(resolve_nothing);
    Runtime rt;
    auto code = rt.block_on(lookup_error("nonexistent.invalid"));
    EXPECT_EQ(code, detail::sys::host_not_found_error()) << code.message();
}

// The backend's "not found" code is in the public DNS category and has a message of
// its own, without asking any resolver.
TEST(LookupHostTest, NotFoundErrorIsInDnsCategory) {
    const std::error_code code = detail::sys::host_not_found_error();
    EXPECT_TRUE(code);
    EXPECT_EQ(&code.category(), &dns_error_category());
    EXPECT_STREQ(code.category().name(), "coro.dns");
    EXPECT_FALSE(code.message().empty());
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

// ---------------------------------------------------------------------------
// Hostnames in connect() / bind()
// ---------------------------------------------------------------------------

// "localhost" may resolve to ::1 before 127.0.0.1. Nothing listens on ::1 here, so
// that attempt is refused and connect() must fall through to the IPv4 address.
TEST(LookupHostTest, ConnectByNameTriesEachAddress) {
    Runtime rt;
    auto echoed = rt.block_on([]() -> Coro<std::string> {
        auto listener = co_await TcpListener::bind("127.0.0.1", 31100);
        auto client   = co_await TcpStream::connect("localhost", 31100);
        auto server   = co_await listener.accept();
        co_await client.write(std::string("hi"));
        auto [n, buf] = co_await server.read_exact(std::string(2, '\0'));
        buf.resize(n);
        co_return buf;
    }());
    EXPECT_EQ(echoed, "hi");
}

TEST(LookupHostTest, BindByName) {
    Runtime rt;
    rt.block_on([]() -> Coro<void> {
        auto listener = co_await TcpListener::bind("localhost", 0);
        auto socket   = co_await UdpSocket::bind("localhost", 0);
        (void)listener;
        (void)socket;
    }());
}

// Every address failing rethrows the last failure, not a generic error.
TEST(LookupHostTest, ConnectByNameRethrowsLastError) {
    Runtime rt;
    auto code = rt.block_on([]() -> Coro<std::error_code> {
        try {
            auto s = co_await TcpStream::connect("localhost", 31101);   // nothing listening
            (void)s;
        } catch (const std::system_error& e) {
            co_return e.code();
        }
        co_return std::error_code{};
    }());
    EXPECT_EQ(code, std::errc::connection_refused);
}

// Two addresses for every name: first one nothing listens on (refused), then
// 127.0.0.1 with the port asked for.
static std::expected<std::vector<SocketAddress>, std::error_code>
resolve_refused_then_loopback(const std::string&, uint16_t port) {
    return std::vector{SocketAddress::parse("127.0.0.1", 31102).value(),
                       SocketAddress::parse("127.0.0.1", port).value()};
}

// connect() tries each address in the resolver's order. "localhost" exercises this
// only where it resolves to ::1 first; the fake makes the first address fail always.
TEST(LookupHostTest, ConnectTriesEachResolvedAddress) {
    FakeResolver fake(resolve_refused_then_loopback);
    Runtime rt;
    auto echoed = rt.block_on([]() -> Coro<std::string> {
        auto listener = co_await TcpListener::bind("127.0.0.1", 31103);
        auto client   = co_await TcpStream::connect("two.addresses.test", 31103);
        auto server   = co_await listener.accept();
        co_await client.write(std::string("hi"));
        auto [n, buf] = co_await server.read_exact(std::string(2, '\0'));
        buf.resize(n);
        co_return buf;
    }());
    EXPECT_EQ(echoed, "hi");
}

TEST(LookupHostTest, ConnectToUnresolvableNameThrowsDnsError) {
    FakeResolver fake(resolve_nothing);
    Runtime rt;
    auto code = rt.block_on([]() -> Coro<std::error_code> {
        try {
            auto s = co_await TcpStream::connect("nonexistent.invalid", 80);
            (void)s;
        } catch (const std::system_error& e) {
            co_return e.code();
        }
        co_return std::error_code{};
    }());
    EXPECT_EQ(&code.category(), &dns_error_category()) << code.message();
}
