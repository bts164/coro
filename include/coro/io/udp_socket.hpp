#pragma once

// Template method bodies for the libuv-backed UdpSocket. Included at the
// bottom of udp_socket.h; never include this file directly.

#include <coro/io/socket_address_uv.h>
#include <coro/runtime/uv_future.h>
#include <coro/task/spawn_on.h>
#include <cerrno>
#include <cstring>
#include <sys/socket.h>
#include <system_error>

namespace coro {

namespace detail {
[[noreturn]] inline void throw_uv_error(int status, const char* what) {
    throw std::system_error(
        std::error_code(-status, std::system_category()), what);
}
} // namespace detail

// ---------------------------------------------------------------------------
// send_to / send
// ---------------------------------------------------------------------------

template<ByteBuffer Buf>
JoinHandle<Buf> UdpSocket::send_to(Buf buf, SocketAddress dest) {
    return with_context(*m_uv_exec,
        [](std::shared_ptr<Handle> handle, Buf buf, SocketAddress dest) -> Coro<Buf> {
            UvCallbackResult<int> result;
            uv_udp_send_t req;
            req.data = &result;

            uv_buf_t uv_buf = uv_buf_init(
                reinterpret_cast<char*>(std::ranges::data(buf)),
                static_cast<unsigned int>(std::ranges::size(buf)));

            sockaddr_storage storage;
            detail::to_sockaddr(dest, storage);

            int r = uv_udp_send(&req, &handle->handle, &uv_buf, 1,
                reinterpret_cast<const struct sockaddr*>(&storage),
                [](uv_udp_send_t* req, int status) {
                    static_cast<UvCallbackResult<int>*>(req->data)->complete(status);
                });
            if (r < 0) detail::throw_uv_error(r, "UdpSocket::send_to");

            auto [status] = co_await wait(result);
            if (status < 0) detail::throw_uv_error(status, "UdpSocket::send_to");
            co_return std::move(buf);
        }(m_handle, std::move(buf), dest)
    );
}

template<ByteBuffer Buf>
JoinHandle<Buf> UdpSocket::send(Buf buf) {
    // Connected UDP socket — dest is whatever peer connect() fixed; libuv (and
    // the kernel underneath) rejects this with UV_EDESTADDRREQ if unconnected.
    return with_context(*m_uv_exec,
        [](std::shared_ptr<Handle> handle, Buf buf) -> Coro<Buf> {
            UvCallbackResult<int> result;
            uv_udp_send_t req;
            req.data = &result;

            uv_buf_t uv_buf = uv_buf_init(
                reinterpret_cast<char*>(std::ranges::data(buf)),
                static_cast<unsigned int>(std::ranges::size(buf)));

            int r = uv_udp_send(&req, &handle->handle, &uv_buf, 1, nullptr,
                [](uv_udp_send_t* req, int status) {
                    static_cast<UvCallbackResult<int>*>(req->data)->complete(status);
                });
            if (r < 0) detail::throw_uv_error(r, "UdpSocket::send");

            auto [status] = co_await wait(result);
            if (status < 0) detail::throw_uv_error(status, "UdpSocket::send");
            co_return std::move(buf);
        }(m_handle, std::move(buf))
    );
}

// ---------------------------------------------------------------------------
// recv_from / recv
// ---------------------------------------------------------------------------

template<ByteBuffer Buf>
Coro<std::tuple<std::size_t, Buf, SocketAddress>> UdpSocket::recv_from_impl(
        std::shared_ptr<Handle> handle, SingleThreadedUvExecutor* uv_exec, Buf buf) {

    // Fast path: try a non-blocking read directly on the calling thread first,
    // avoiding a uv-thread hop entirely when a datagram is already queued in
    // the kernel's receive buffer. Race note: another coroutine could in
    // principle be doing the same on this socket concurrently — the API
    // contract (see udp_socket.h) disallows overlapping recv_from()/recv()
    // calls, so this is not guarded further here.
    sockaddr_storage storage;
    socklen_t addrlen = sizeof(storage);
    ssize_t n = ::recvfrom(handle->raw_fd,
        std::ranges::data(buf), std::ranges::size(buf), MSG_DONTWAIT,
        reinterpret_cast<sockaddr*>(&storage), &addrlen);

    if (n >= 0) {
        SocketAddress sender = detail::from_sockaddr(reinterpret_cast<sockaddr*>(&storage));
        co_return std::tuple<std::size_t, Buf, SocketAddress>{
            static_cast<std::size_t>(n), std::move(buf), sender};
    }
    if (errno != EAGAIN && errno != EWOULDBLOCK) {
        throw std::system_error(errno, std::system_category(), "UdpSocket::recv_from");
    }

    // Slow path: nothing was queued — hop to the uv thread and arm a single-shot
    // uv_udp_recv_start()/uv_udp_recv_stop() around exactly one datagram.
    auto [count, received_buf, sender] = co_await with_context(*uv_exec,
        [](std::shared_ptr<Handle> handle, Buf buf) -> Coro<std::tuple<std::size_t, Buf, SocketAddress>> {
            struct RecvState {
                UvCallbackResult<ssize_t, SocketAddress> result;
                Buf* buf;
            } state{{}, &buf};
            handle->handle.data = &state;

            int r = uv_udp_recv_start(&handle->handle,
                [](uv_handle_t* h, size_t suggested_size, uv_buf_t* out_buf) {
                    auto* state = static_cast<RecvState*>(h->data);
                    (void)suggested_size;
                    *out_buf = uv_buf_init(
                        reinterpret_cast<char*>(std::ranges::data(*state->buf)),
                        static_cast<unsigned int>(std::ranges::size(*state->buf)));
                },
                [](uv_udp_t* h, ssize_t nread, const uv_buf_t*, const struct sockaddr* addr, unsigned) {
                    // libuv can invoke this callback with nread == 0 and addr == nullptr
                    // to indicate "no datagram available this tick" — must NOT stop
                    // receiving in that case, or the real datagram would never arrive.
                    if (nread == 0 && addr == nullptr) return;
                    auto* state = static_cast<RecvState*>(h->data);
                    uv_udp_recv_stop(h);
                    SocketAddress sender = addr
                        ? detail::from_sockaddr(addr)
                        : SocketAddress{};
                    state->result.complete(nread, sender);
                });
            if (r < 0) detail::throw_uv_error(r, "UdpSocket::recv_from");

            auto [nread, sender] = co_await wait(state.result);
            if (nread < 0) detail::throw_uv_error(static_cast<int>(nread), "UdpSocket::recv_from");
            co_return std::tuple<std::size_t, Buf, SocketAddress>{
                static_cast<std::size_t>(nread), std::move(buf), sender};
        }(handle, std::move(buf))
    );

    co_return std::tuple<std::size_t, Buf, SocketAddress>{count, std::move(received_buf), sender};
}

template<ByteBuffer Buf>
Coro<std::tuple<std::size_t, Buf, SocketAddress>> UdpSocket::recv_from(Buf buf) {
    return recv_from_impl<Buf>(m_handle, m_uv_exec, std::move(buf));
}

template<ByteBuffer Buf>
Coro<std::pair<std::size_t, Buf>> UdpSocket::recv(Buf buf) {
    auto [n, received_buf, sender] = co_await recv_from_impl<Buf>(m_handle, m_uv_exec, std::move(buf));
    (void)sender; // connected socket — the OS already filtered to only our peer
    co_return std::pair<std::size_t, Buf>{n, std::move(received_buf)};
}

} // namespace coro
