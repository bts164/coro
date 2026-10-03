// Desktop UdpSocket on the IoDriver. Every syscall goes through the backend seam
// in include/coro/detail/sys/udp.h. See doc/design/udp_socket.md, "Desktop
// (IoDriver) backend".

#include <coro/io/udp_socket.h>
#include <coro/detail/sys/udp.h>
#include <coro/io/lookup_host.h>
#include <exception>
#include <system_error>

namespace coro {

// ---------------------------------------------------------------------------
// UdpSocket
// ---------------------------------------------------------------------------

UdpSocket::UdpSocket(std::shared_ptr<State> state) : m_state(std::move(state)) {}

UdpSocket::UdpSocket(UdpSocket&&) noexcept = default;
UdpSocket& UdpSocket::operator=(UdpSocket&&) noexcept = default;
UdpSocket::~UdpSocket() = default;

Coro<UdpSocket> UdpSocket::bind(std::string host, uint16_t port) {
    IoDriver& driver = detail::socket_io_driver("UdpSocket::bind");
    // The first resolved address that binds wins, as for TcpListener::bind.
    const std::vector<SocketAddress> locals = co_await lookup_host(host, port);
    std::exception_ptr last_error;
    for (const SocketAddress& local : locals) {
        try {
            co_return UdpSocket(std::make_shared<State>(driver, detail::sys::udp_open(local)));
        } catch (const std::system_error&) {
            last_error = std::current_exception();
        }
    }
    std::rethrow_exception(last_error);   // non-null: lookup_host never returns empty
}

Coro<void> UdpSocket::connect(SocketAddress peer) {
    return connect_impl(m_state, peer);
}

Coro<void> UdpSocket::connect_impl(std::shared_ptr<State> state, SocketAddress peer) {
    detail::sys::udp_connect(state->fd, peer);
    co_return;
}

Coro<void> UdpSocket::set_broadcast(bool enabled) {
    return set_broadcast_impl(m_state, enabled);
}

Coro<void> UdpSocket::set_broadcast_impl(std::shared_ptr<State> state, bool enabled) {
    detail::sys::udp_set_broadcast(state->fd, enabled);
    co_return;
}

void UdpSocket::set_segment_size(std::size_t bytes) {
    detail::sys::udp_set_segment_size(m_state->fd, bytes);
}

void UdpSocket::set_gro(bool enabled) {
    detail::sys::udp_set_gro(m_state->fd, enabled);
}

Coro<void> UdpSocket::join_multicast(Ipv4Address group, Ipv4Address iface) {
    return set_membership_impl(m_state, group, iface, true);
}

Coro<void> UdpSocket::leave_multicast(Ipv4Address group, Ipv4Address iface) {
    return set_membership_impl(m_state, group, iface, false);
}

Coro<void> UdpSocket::set_membership_impl(std::shared_ptr<State> state, Ipv4Address group,
                                          Ipv4Address iface, bool join) {
    detail::sys::udp_set_membership(state->fd, group, iface, join);
    co_return;
}

} // namespace coro
