#include <coro/io/socket_address.h>
#include <cstdio>
#include <cstring>
#include <type_traits>

#ifdef CORO_PICO
#include <lwip/ip4_addr.h>
#else
#include <arpa/inet.h>
#endif

namespace coro {

namespace {

std::optional<Ipv4Address> parse_ipv4(std::string_view host) {
    // inet_pton/ip4addr_aton both require a NUL-terminated C string.
    std::string s(host);

#ifdef CORO_PICO
    ip4_addr_t addr;
    if (!ip4addr_aton(s.c_str(), &addr)) return std::nullopt;
    Ipv4Address result;
    std::memcpy(result.octets.data(), &addr.addr, 4);
    return result;
#else
    Ipv4Address result;
    if (inet_pton(AF_INET, s.c_str(), result.octets.data()) != 1) return std::nullopt;
    return result;
#endif
}

#ifndef CORO_PICO
std::optional<Ipv6Address> parse_ipv6(std::string_view host) {
    // Strip a "%<scope>" zone suffix (numeric interface index only) before
    // handing the address portion to inet_pton, which doesn't understand it.
    std::string_view addr_part = host;
    uint32_t scope_id = 0;
    if (auto pos = host.find('%'); pos != std::string_view::npos) {
        addr_part = host.substr(0, pos);
        std::string_view zone = host.substr(pos + 1);
        uint32_t value = 0;
        for (char c : zone) {
            if (c < '0' || c > '9') return std::nullopt;
            value = value * 10 + static_cast<uint32_t>(c - '0');
        }
        scope_id = value;
    }

    std::string s(addr_part);
    Ipv6Address result;
    if (inet_pton(AF_INET6, s.c_str(), result.octets.data()) != 1) return std::nullopt;
    result.scope_id = scope_id;
    return result;
}
#endif

// Renders 16 raw octets as compressed IPv6 text (e.g. "fe80::1"), matching the
// canonical form from RFC 5952: the longest run of two or more zero groups is
// collapsed to "::" (the first such run if there's a tie), everything else is
// printed as lowercase hex with no leading zeros.
std::string ipv6_to_string(const std::array<uint8_t, 16>& octets) {
    uint16_t groups[8];
    for (int i = 0; i < 8; ++i)
        groups[i] = static_cast<uint16_t>((octets[2 * i] << 8) | octets[2 * i + 1]);

    int best_start = -1, best_len = 0;
    int cur_start = -1, cur_len = 0;
    for (int i = 0; i < 8; ++i) {
        if (groups[i] == 0) {
            if (cur_start < 0) cur_start = i;
            ++cur_len;
            if (cur_len > best_len) { best_start = cur_start; best_len = cur_len; }
        } else {
            cur_start = -1;
            cur_len = 0;
        }
    }
    if (best_len < 2) best_start = -1; // only collapse runs of 2+ groups

    std::string out;
    char buf[8];
    for (int i = 0; i < 8; ) {
        if (i == best_start) {
            out += "::";
            i += best_len;
            continue;
        }
        if (!out.empty() && out.back() != ':') out += ':';
        std::snprintf(buf, sizeof(buf), "%x", groups[i]);
        out += buf;
        ++i;
    }
    if (out.empty()) out = "::";
    return out;
}

} // namespace

std::optional<SocketAddress> SocketAddress::parse(std::string_view host, uint16_t port) {
    // A colon in the address text (outside of a "%scope" suffix, which never
    // contains one) means IPv6 — dotted-decimal IPv4 never does.
    bool looks_ipv6 = host.find(':') != std::string_view::npos;

    if (!looks_ipv6) {
        if (auto v4 = parse_ipv4(host))
            return SocketAddress{*v4, port};
        return std::nullopt;
    }

#ifdef CORO_PICO
    // No IPv6 support on the lwIP backend (LWIP_IPV6 is 0 in this project's
    // lwipopts.h) — see doc/design/udp_socket.md's "Known limitations" section.
    (void)port;
    return std::nullopt;
#else
    if (auto v6 = parse_ipv6(host))
        return SocketAddress{*v6, port};
    return std::nullopt;
#endif
}

std::string SocketAddress::to_string() const {
    return std::visit([this](const auto& addr) -> std::string {
        using T = std::decay_t<decltype(addr)>;
        char buf[16];
        if constexpr (std::is_same_v<T, Ipv4Address>) {
            std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u",
                addr.octets[0], addr.octets[1], addr.octets[2], addr.octets[3]);
            return std::string(buf) + ":" + std::to_string(port);
        } else {
            std::string text = "[" + ipv6_to_string(addr.octets);
            if (addr.scope_id != 0) text += "%" + std::to_string(addr.scope_id);
            text += "]:" + std::to_string(port);
            return text;
        }
    }, address);
}

} // namespace coro
