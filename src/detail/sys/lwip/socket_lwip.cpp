// lwIP backend for the shared socket seam (include/coro/detail/sys/socket.h), and
// the helpers every lwIP backend file shares (io_object.h).

#include "io_object.h"

#include <coro/detail/sys/socket.h>

#include <lwip/ip4_addr.h>

#include <cerrno>
#include <cstring>
#include <system_error>
#include <variant>

namespace coro::detail::sys {

void close_socket(RawFd fd) noexcept {
    if (fd == nullptr) return;
    // Deferred to the end of the Poller's service pass if one is running: see
    // io_object.h, "Teardown".
    close_object_deferred(fd);
}

bool parse_ipv4(const char* text, std::array<std::uint8_t, 4>& out) noexcept {
    ip4_addr_t addr;
    if (!ip4addr_aton(text, &addr)) return false;
    // ip4_addr_t holds the address in network byte order: the octets in order.
    std::memcpy(out.data(), &addr.addr, 4);
    return true;
}

bool parse_ipv6(const char* /*text*/, std::array<std::uint8_t, 16>& /*out*/) noexcept {
    // coro's lwipopts.h leaves LWIP_IPV6 off.
    return false;
}

int lwip_errno(err_t err) noexcept {
    // lwIP's own err_to_errno() table (api/err.c), which a NO_SYS build without the
    // sockets API does not compile.
    switch (err) {
    case ERR_OK:         return 0;
    case ERR_MEM:        return ENOMEM;
    case ERR_BUF:        return ENOBUFS;
    case ERR_TIMEOUT:    return ETIMEDOUT;
    case ERR_RTE:        return EHOSTUNREACH;
    case ERR_INPROGRESS: return EINPROGRESS;
    case ERR_VAL:        return EINVAL;
    case ERR_WOULDBLOCK: return EWOULDBLOCK;
    case ERR_USE:        return EADDRINUSE;
    case ERR_ALREADY:    return EALREADY;
    case ERR_ISCONN:     return EISCONN;
    case ERR_CONN:       return ENOTCONN;
    case ERR_IF:         return EIO;
    case ERR_ABRT:       return ECONNABORTED;
    case ERR_RST:        return ECONNRESET;
    case ERR_CLSD:       return ENOTCONN;
    case ERR_ARG:        return EINVAL;
    default:             return EIO;
    }
}

void throw_errno(int err, const char* what) {
    throw std::system_error(err, std::generic_category(), what);
}

bool to_ip_addr(const SocketAddress& addr, ip_addr_t& out) noexcept {
    const auto* v4 = std::get_if<Ipv4Address>(&addr.address);
    if (v4 == nullptr) return false;
    IP_ADDR4(&out, v4->octets[0], v4->octets[1], v4->octets[2], v4->octets[3]);
    return true;
}

SocketAddress from_ip_addr(const ip_addr_t& addr, uint16_t port) noexcept {
    Ipv4Address v4;
    std::memcpy(v4.octets.data(), &ip_2_ip4(&addr)->addr, 4);
    return SocketAddress{v4, port};
}

} // namespace coro::detail::sys
