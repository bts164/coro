#pragma once

// Internal SocketAddress <-> sockaddr conversion helpers for the libuv UDP
// backend. Not part of the public API — lives under include/ (rather than
// src/) only because udp_socket.hpp (a public header, included from
// udp_socket.h) needs it. Never include this directly; it has no use outside
// the libuv UdpSocket implementation.

#include <coro/io/socket_address.h>
#include <arpa/inet.h>
#include <cstring>
#include <sys/socket.h>
#include <variant>

namespace coro::detail {

template<typename... Ts>
struct overloaded : Ts... { using Ts::operator()...; };
template<typename... Ts>
overloaded(Ts...) -> overloaded<Ts...>;

inline socklen_t to_sockaddr(const SocketAddress& addr, sockaddr_storage& out) {
    std::memset(&out, 0, sizeof(out));
    return std::visit(overloaded{
        [&](const Ipv4Address& v4) -> socklen_t {
            auto* sin = reinterpret_cast<sockaddr_in*>(&out);
            sin->sin_family = AF_INET;
            sin->sin_port   = htons(addr.port);
            std::memcpy(&sin->sin_addr, v4.octets.data(), 4);
            return sizeof(sockaddr_in);
        },
        [&](const Ipv6Address& v6) -> socklen_t {
            auto* sin6 = reinterpret_cast<sockaddr_in6*>(&out);
            sin6->sin6_family   = AF_INET6;
            sin6->sin6_port     = htons(addr.port);
            sin6->sin6_scope_id = v6.scope_id;
            std::memcpy(&sin6->sin6_addr, v6.octets.data(), 16);
            return sizeof(sockaddr_in6);
        },
    }, addr.address);
}

inline SocketAddress from_sockaddr(const sockaddr* addr) {
    if (addr->sa_family == AF_INET) {
        auto* sin = reinterpret_cast<const sockaddr_in*>(addr);
        Ipv4Address v4;
        std::memcpy(v4.octets.data(), &sin->sin_addr, 4);
        return SocketAddress{v4, ntohs(sin->sin_port)};
    }
    auto* sin6 = reinterpret_cast<const sockaddr_in6*>(addr);
    Ipv6Address v6;
    std::memcpy(v6.octets.data(), &sin6->sin6_addr, 16);
    v6.scope_id = sin6->sin6_scope_id;
    return SocketAddress{v6, ntohs(sin6->sin6_port)};
}

} // namespace coro::detail
