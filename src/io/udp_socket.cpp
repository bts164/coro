#include <coro/io/udp_socket.h>
#include <coro/runtime/uv_future.h>
#include <coro/task/spawn_on.h>
#include <coro/coro.h>
#include <coro/io/socket_address_uv.h>
#include <cerrno>
#include <cstring>
#include <system_error>

#ifdef __linux__
#include <netinet/in.h>
#include <netinet/udp.h>
#include <sys/socket.h>
#endif

namespace coro {

namespace {
[[noreturn]] void throw_uv_error(int status, const char* what) {
    throw std::system_error(
        std::error_code(-status, std::system_category()), what);
}
} // namespace

UdpSocket::UdpSocket(std::shared_ptr<Handle> handle, SingleThreadedUvExecutor* uv_exec)
    : m_handle(std::move(handle)), m_uv_exec(uv_exec) {}

UdpSocket::UdpSocket(UdpSocket&&) noexcept = default;
UdpSocket& UdpSocket::operator=(UdpSocket&&) noexcept = default;

UdpSocket::~UdpSocket() {
    if (!m_handle) return;
    with_context(*m_uv_exec,
        [](std::shared_ptr<Handle> handle) -> Coro<void> {
            UvCallbackResult<int> result;
            handle->handle.data = &result;
            uv_close(reinterpret_cast<uv_handle_t*>(&handle->handle),
                [](uv_handle_t* h) {
                    static_cast<UvCallbackResult<int>*>(h->data)->complete(0);
                });
            auto [ignored] = co_await wait(result);
            (void)ignored;
        }(std::move(m_handle))
    ).detach();
}

// ---------------------------------------------------------------------------
// bind
// ---------------------------------------------------------------------------

JoinHandle<UdpSocket> UdpSocket::bind(std::string host, uint16_t port) {
    auto& exec = current_uv_executor();
    return with_context(exec,
        [](SingleThreadedUvExecutor& exec,
           std::string host, uint16_t port) -> Coro<UdpSocket> {

            auto handle = std::make_shared<Handle>();
            uv_udp_init(exec.loop(), &handle->handle);

            struct sockaddr_in addr;
            if (int r = uv_ip4_addr(host.c_str(), port, &addr); r != 0)
                throw_uv_error(r, "UdpSocket::bind");

            if (int r = uv_udp_bind(&handle->handle,
                                    reinterpret_cast<const struct sockaddr*>(&addr), 0);
                    r != 0)
                throw_uv_error(r, "UdpSocket::bind");

            // Cached once, on the uv thread, right after the socket exists — lets
            // recv_from_impl's fast path avoid a libuv accessor call off the uv
            // thread later. See doc/design/udp_socket.md's "Receive path" section.
            uv_os_fd_t fd;
            if (int r = uv_fileno(reinterpret_cast<uv_handle_t*>(&handle->handle), &fd);
                    r != 0)
                throw_uv_error(r, "UdpSocket::bind");
            handle->raw_fd = static_cast<int>(fd);

            co_return UdpSocket(std::move(handle), &exec);
        }(exec, std::move(host), port)
    );
}

// ---------------------------------------------------------------------------
// connect
// ---------------------------------------------------------------------------

JoinHandle<void> UdpSocket::connect(SocketAddress peer) {
    return with_context(*m_uv_exec,
        [](std::shared_ptr<Handle> handle, SocketAddress peer) -> Coro<void> {
            sockaddr_storage storage;
            socklen_t addrlen = detail::to_sockaddr(peer, storage);
            (void)addrlen;
            int r = uv_udp_connect(&handle->handle,
                reinterpret_cast<const struct sockaddr*>(&storage));
            if (r < 0) throw_uv_error(r, "UdpSocket::connect");
            co_return;
        }(m_handle, peer)
    );
}

// ---------------------------------------------------------------------------
// set_broadcast, join_multicast, leave_multicast
// ---------------------------------------------------------------------------

JoinHandle<void> UdpSocket::set_broadcast(bool enabled) {
    return with_context(*m_uv_exec,
        [](std::shared_ptr<Handle> handle, bool enabled) -> Coro<void> {
            int r = uv_udp_set_broadcast(&handle->handle, enabled ? 1 : 0);
            if (r < 0) throw_uv_error(r, "UdpSocket::set_broadcast");
            co_return;
        }(m_handle, enabled)
    );
}

// ---------------------------------------------------------------------------
// set_segment_size
// ---------------------------------------------------------------------------

void UdpSocket::set_segment_size(std::size_t bytes) {
#ifdef __linux__
    // Only the kernel socket is touched, never handle->handle, so this runs on
    // the calling thread. Potential race: a send() in flight on another thread
    // (fast path on the caller's thread, or a queued uv_udp_send() on the uv
    // thread) may be segmented with either the old or the new size — the kernel
    // reads the option once per send, so each datagram is still one or the other.
    int seg = static_cast<int>(bytes);
    if (setsockopt(m_handle->raw_fd, IPPROTO_UDP, UDP_SEGMENT, &seg, sizeof(seg)) != 0)
        throw std::system_error(errno, std::system_category(), "UdpSocket::set_segment_size");
#else
    (void)bytes;
    throw std::system_error(std::make_error_code(std::errc::not_supported),
                            "UdpSocket::set_segment_size");
#endif
}

// ---------------------------------------------------------------------------
// set_gro, recv_segments
// ---------------------------------------------------------------------------

void UdpSocket::set_gro(bool enabled) {
#ifdef __linux__
    // Kernel socket only, like set_segment_size(). Potential race: a datagram
    // queued before the change keeps the form it was queued in, so a receive
    // just after set_gro(false) may still return a coalesced buffer (which
    // recv_segments_from() splits correctly; recv_from() would not).
    int on = enabled ? 1 : 0;
    if (setsockopt(m_handle->raw_fd, IPPROTO_UDP, UDP_GRO, &on, sizeof(on)) != 0)
        throw std::system_error(errno, std::system_category(), "UdpSocket::set_gro");
#else
    (void)enabled;
    throw std::system_error(std::make_error_code(std::errc::not_supported), "UdpSocket::set_gro");
#endif
}

ssize_t detail::recv_segments(int fd, std::byte* data, std::size_t size,
                              SocketAddress& sender, std::size_t& segment_size) {
    sockaddr_storage storage;
    iovec iov{data, size};
    msghdr msg{};
    msg.msg_name    = &storage;
    msg.msg_namelen = sizeof(storage);
    msg.msg_iov     = &iov;
    msg.msg_iovlen  = 1;
#ifdef __linux__
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))];
    msg.msg_control    = control;
    msg.msg_controllen = sizeof(control);
#endif
    ssize_t n = ::recvmsg(fd, &msg, MSG_DONTWAIT);
    if (n < 0) return n;
    sender = detail::from_sockaddr(reinterpret_cast<sockaddr*>(&storage));
    segment_size = static_cast<std::size_t>(n);
#ifdef __linux__
    for (cmsghdr* c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
        if (c->cmsg_level == IPPROTO_UDP && c->cmsg_type == UDP_GRO) {
            int gso_size;
            std::memcpy(&gso_size, CMSG_DATA(c), sizeof(gso_size));
            if (gso_size > 0) segment_size = static_cast<std::size_t>(gso_size);
        }
    }
#endif
    return n;
}

JoinHandle<void> UdpSocket::set_membership(std::shared_ptr<Handle> handle,
        SingleThreadedUvExecutor* uv_exec, Ipv4Address group, Ipv4Address iface,
        uv_membership membership) {
    return with_context(*uv_exec,
        [](std::shared_ptr<Handle> handle, Ipv4Address group, Ipv4Address iface,
           uv_membership membership) -> Coro<void> {
            char group_str[16];
            uv_inet_ntop(AF_INET, group.octets.data(), group_str, sizeof(group_str));

            char iface_str[16];
            bool has_iface = !(iface.octets == Ipv4Address{}.octets);
            if (has_iface) uv_inet_ntop(AF_INET, iface.octets.data(), iface_str, sizeof(iface_str));

            int r = uv_udp_set_membership(&handle->handle, group_str,
                has_iface ? iface_str : nullptr, membership);
            if (r < 0) throw_uv_error(r, "UdpSocket::join_multicast/leave_multicast");
            co_return;
        }(handle, group, iface, membership)
    );
}

JoinHandle<void> UdpSocket::join_multicast(Ipv4Address group, Ipv4Address iface) {
    return set_membership(m_handle, m_uv_exec, group, iface, UV_JOIN_GROUP);
}

JoinHandle<void> UdpSocket::leave_multicast(Ipv4Address group, Ipv4Address iface) {
    return set_membership(m_handle, m_uv_exec, group, iface, UV_LEAVE_GROUP);
}

} // namespace coro
