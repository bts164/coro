#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace coro {

/**
 * @brief A 4-byte IPv4 address, stored as raw octets (never as a string).
 */
struct Ipv4Address {
    std::array<uint8_t, 4> octets{};

    friend bool operator==(const Ipv4Address&, const Ipv4Address&) = default;
};

/**
 * @brief A 16-byte IPv6 address plus scope id, stored as raw octets.
 */
struct Ipv6Address {
    std::array<uint8_t, 16> octets{};
    // Interface index disambiguating link-local addresses (fe80::/10), which are not
    // globally unique — the same address can be valid on multiple interfaces at once.
    // Zero for global-scope addresses, where it's meaningless. Corresponds directly to
    // sockaddr_in6::sin6_scope_id.
    uint32_t scope_id = 0;

    friend bool operator==(const Ipv6Address&, const Ipv6Address&) = default;
};

/**
 * @brief An IPv4 or IPv6 address plus port. The library's shared address type,
 * used anywhere a peer address must be produced or consumed (e.g. `UdpSocket::recv_from`).
 */
struct SocketAddress {
    std::variant<Ipv4Address, Ipv6Address> address;
    uint16_t port = 0;

    friend bool operator==(const SocketAddress&, const SocketAddress&) = default;

    /// Parses a numeric IPv4 or IPv6 address ("127.0.0.1", "::1", "fe80::1%3") plus
    /// port into a SocketAddress. Returns std::nullopt on malformed input — there is
    /// no way to construct a SocketAddress holding an invalid address.
    static std::optional<SocketAddress> parse(std::string_view host, uint16_t port);

    /// Renders back to text, e.g. "127.0.0.1:9001" or "[fe80::1%3]:9001". For
    /// logging/diagnostics only — allocates, so it must never sit on the send/recv
    /// hot path.
    std::string to_string() const;
};

} // namespace coro
