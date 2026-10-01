#pragma once

// Template method bodies for the libuv-backed UdpSocket. Included at the
// bottom of udp_socket.h; never include this file directly.

#include <coro/io/socket_address_uv.h>
#include <coro/runtime/uv_future.h>
#include <coro/task/spawn_on.h>
#include <cerrno>
#include <optional>
#include <type_traits>
#include <cstring>
#include <sys/socket.h>
#include <system_error>

namespace coro {

namespace detail {
[[noreturn]] inline void throw_uv_error(int status, const char* what) {
    throw std::system_error(
        std::error_code(-status, std::system_category()), what);
}

/// One non-blocking recvmsg() for UdpRecvSegmentsFuture, reading the UDP_GRO
/// segment size alongside the data (segment_size = the returned size when
/// there is none). Returns -1 with errno set on failure, like recvmsg().
/// Defined in udp_socket.cpp to keep the Linux-only headers out of here.
ssize_t recv_segments(int fd, std::byte* data, std::size_t size,
                      SocketAddress& sender, std::size_t& segment_size);
} // namespace detail

// ---------------------------------------------------------------------------
// send_to / send
// ---------------------------------------------------------------------------

/**
 * @brief Future returned by `UdpSocket::send()` / `send_to()`.
 *
 * Hand-written rather than a `Coro<Buf>` so the overwhelmingly common case (the
 * kernel accepts the datagram immediately) costs no coroutine frame allocation
 * and no CoroutineScope bookkeeping: the first `poll()` -- which
 * `FutureAwaitable::await_ready()` runs eagerly -- does the non-blocking
 * syscall and returns `PollReady`.
 *
 * A UDP datagram is sent atomically -- sendto() either accepts the whole thing
 * or fails outright, unlike TCP's partial-write case, so there's no
 * partial-send bookkeeping. On EAGAIN (kernel send buffer momentarily full) the
 * future builds the uv-thread slow path once (a Coro spawned on the uv
 * executor using uv_udp_send()) and forwards poll()/cancel() to its
 * JoinHandle from then on.
 *
 * Race note: overlapping send()/send_to() calls on one socket aren't supported
 * (see udp_socket.h's Concurrency note).
 *
 * Not copyable; the datagram buffer is owned by the future until it completes.
 */
template<ByteBuffer Buf>
class UdpSendFuture {
    using Handle = UdpSocket::Handle;
public:
    using OutputType = Buf;

    UdpSendFuture(std::shared_ptr<Handle> handle, SingleThreadedUvExecutor* uv_exec,
                  Buf buf, std::optional<SocketAddress> dest)
        : m_handle(std::move(handle)), m_uv_exec(uv_exec),
          m_buf(std::move(buf)), m_dest(dest) {}

    PollResult<Buf> poll(detail::Context& ctx) {
        if (m_slow) {
            return m_slow->poll(ctx);
        }

        ssize_t n;
        if (m_dest) {
            sockaddr_storage storage;
            socklen_t addrlen = detail::to_sockaddr(*m_dest, storage);
            n = ::sendto(m_handle->raw_fd, std::ranges::data(m_buf), std::ranges::size(m_buf),
                MSG_DONTWAIT, reinterpret_cast<sockaddr*>(&storage), addrlen);
        } else {
            // Connected UDP socket -- dest is whatever peer connect() fixed; the
            // kernel rejects this with EDESTADDRREQ if unconnected.
            n = ::send(m_handle->raw_fd, std::ranges::data(m_buf), std::ranges::size(m_buf), MSG_DONTWAIT);
        }
        if (n >= 0) {
            return std::move(m_buf);
        }
        int err = errno;
        if (err != EAGAIN && err != EWOULDBLOCK) {
            return PollError(std::make_exception_ptr(std::system_error(
                err, std::system_category(), m_dest ? "UdpSocket::send_to" : "UdpSocket::send")));
        }

        // Slow path: kernel send buffer is full -- hop to the uv thread and let
        // uv_udp_send() queue the datagram and retry once space frees up.
        m_slow.emplace(with_context(*m_uv_exec,
            [](std::shared_ptr<Handle> handle, Buf buf, std::optional<SocketAddress> dest) -> Coro<Buf> {
                const char* what = dest ? "UdpSocket::send_to" : "UdpSocket::send";
                UvCallbackResult<int> result;
                uv_udp_send_t req;
                req.data = &result;

                uv_buf_t uv_buf = uv_buf_init(
                    reinterpret_cast<char*>(std::ranges::data(buf)),
                    static_cast<unsigned int>(std::ranges::size(buf)));

                sockaddr_storage storage;
                const sockaddr* addr = nullptr;
                if (dest) {
                    detail::to_sockaddr(*dest, storage);
                    addr = reinterpret_cast<const sockaddr*>(&storage);
                }

                int r = uv_udp_send(&req, &handle->handle, &uv_buf, 1, addr,
                    [](uv_udp_send_t* req, int status) {
                        static_cast<UvCallbackResult<int>*>(req->data)->complete(status);
                    });
                if (r < 0) detail::throw_uv_error(r, what);

                auto [status] = co_await wait(result);
                if (status < 0) detail::throw_uv_error(status, what);
                co_return std::move(buf);
            }(m_handle, std::move(m_buf), m_dest)
        ));
        return m_slow->poll(ctx);
    }

    /// Only meaningful once the slow path is in flight (the fast path completes
    /// in its first poll()); forwards to the spawned uv-thread task.
    void cancel() noexcept {
        if (m_slow) m_slow->cancel();
    }

private:
    std::shared_ptr<Handle>        m_handle;
    SingleThreadedUvExecutor*      m_uv_exec;
    Buf                            m_buf;
    std::optional<SocketAddress>   m_dest;   // nullopt => send() on a connected socket
    std::optional<JoinHandle<Buf>> m_slow;   // engaged only after EAGAIN
};

template<ByteBuffer Buf>
UdpSendFuture<Buf> UdpSocket::send_to(Buf buf, SocketAddress dest) {
    return UdpSendFuture<Buf>(m_handle, m_uv_exec, std::move(buf), dest);
}

template<ByteBuffer Buf>
UdpSendFuture<Buf> UdpSocket::send(Buf buf) {
    return UdpSendFuture<Buf>(m_handle, m_uv_exec, std::move(buf), std::nullopt);
}

// ---------------------------------------------------------------------------
// recv_from / recv
// ---------------------------------------------------------------------------

/**
 * @brief Future returned by `UdpSocket::recv_from()` (`WithSender = true`, yields
 * `{n, buf, sender}`) and `UdpSocket::recv()` (`WithSender = false`, yields `{n, buf}`).
 *
 * Hand-written for the same reason as `UdpSendFuture`: when a datagram is already
 * queued in the kernel's receive buffer, the first `poll()` (run eagerly by
 * `FutureAwaitable::await_ready()`) does a non-blocking `recvfrom()` and completes
 * with no coroutine frame allocation and no CoroutineScope bookkeeping. Only on
 * EAGAIN does it spawn the uv-thread slow path (single-shot
 * uv_udp_recv_start()/uv_udp_recv_stop() around exactly one datagram) and forward
 * poll()/cancel() to its JoinHandle.
 *
 * Race note: another coroutine could in principle be reading this socket
 * concurrently -- the API contract (see udp_socket.h) disallows overlapping
 * recv_from()/recv() calls, so this is not guarded further here.
 */
template<ByteBuffer Buf, bool WithSender>
class UdpRecvFuture {
    using Handle = UdpSocket::Handle;
    using Full   = std::tuple<std::size_t, Buf, SocketAddress>;
public:
    using OutputType = std::conditional_t<WithSender, Full, std::pair<std::size_t, Buf>>;

    UdpRecvFuture(std::shared_ptr<Handle> handle, SingleThreadedUvExecutor* uv_exec, Buf buf)
        : m_handle(std::move(handle)), m_uv_exec(uv_exec), m_buf(std::move(buf)) {}

    PollResult<OutputType> poll(detail::Context& ctx) {
        if (!m_slow) {
            sockaddr_storage storage;
            socklen_t addrlen = sizeof(storage);
            ssize_t n = ::recvfrom(m_handle->raw_fd,
                std::ranges::data(m_buf), std::ranges::size(m_buf), MSG_DONTWAIT,
                reinterpret_cast<sockaddr*>(&storage), &addrlen);
            if (n >= 0) {
                return make_output(static_cast<std::size_t>(n), std::move(m_buf),
                    WithSender ? detail::from_sockaddr(reinterpret_cast<sockaddr*>(&storage))
                               : SocketAddress{});
            }
            int err = errno;
            if (err != EAGAIN && err != EWOULDBLOCK) {
                return PollError(std::make_exception_ptr(std::system_error(
                    err, std::system_category(), "UdpSocket::recv_from")));
            }

            // Slow path: nothing queued -- hop to the uv thread and arm a
            // single-shot receive around exactly one datagram.
            m_slow.emplace(with_context(*m_uv_exec,
                [](std::shared_ptr<Handle> handle, Buf buf) -> Coro<Full> {
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
                            // to indicate "no datagram available this tick" -- must NOT stop
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
                    co_return Full{static_cast<std::size_t>(nread), std::move(buf), sender};
                }(m_handle, std::move(m_buf))
            ));
        }

        auto r = m_slow->poll(ctx);
        if (r.isPending()) return PollPending;
        if (r.isDropped()) return PollDropped;
        if (r.isError())   return PollError(r.error());
        auto [n, buf, sender] = std::move(r).value();
        return make_output(n, std::move(buf), sender);
    }

    /// Only meaningful once the slow path is in flight (the fast path completes
    /// in its first poll()); forwards to the spawned uv-thread task.
    void cancel() noexcept {
        if (m_slow) m_slow->cancel();
    }

private:
    static OutputType make_output(std::size_t n, Buf buf, SocketAddress sender) {
        if constexpr (WithSender) return Full{n, std::move(buf), sender};
        else                      return std::pair<std::size_t, Buf>{n, std::move(buf)};
    }

    std::shared_ptr<Handle>        m_handle;
    SingleThreadedUvExecutor*      m_uv_exec;
    Buf                            m_buf;
    std::optional<JoinHandle<Full>> m_slow;   // engaged only after EAGAIN
};

template<ByteBuffer Buf>
UdpRecvFuture<Buf, true> UdpSocket::recv_from(Buf buf) {
    return UdpRecvFuture<Buf, true>(m_handle, m_uv_exec, std::move(buf));
}

template<ByteBuffer Buf>
UdpRecvFuture<Buf, false> UdpSocket::recv(Buf buf) {
    // Connected socket -- the OS already filtered to only our peer, so the
    // sender address is dropped (and not even decoded on the fast path).
    return UdpRecvFuture<Buf, false>(m_handle, m_uv_exec, std::move(buf));
}

// ---------------------------------------------------------------------------
// recv_segments_from
// ---------------------------------------------------------------------------

/**
 * @brief Future returned by `UdpSocket::recv_segments_from()`.
 *
 * Same fast path as `UdpRecvFuture`, but with recvmsg() so the UDP_GRO segment
 * size comes back too. libuv's receive callback can't report that (it never
 * reads control messages), so the slow path differs: it only waits on the uv
 * thread until the socket is readable -- uv_udp_recv_start() with an alloc
 * callback that returns an empty buffer, which libuv answers with UV_ENOBUFS
 * without reading anything -- and poll() then retries the recvmsg() here.
 * Like `UdpRecvFuture`, don't cancel it once the slow path is armed: the uv
 * callback would complete a result that no longer exists.
 *
 * Race note: another reader could empty the socket between the wakeup and the
 * retry; the retry then sees EAGAIN and simply waits again. The API contract
 * (see udp_socket.h) disallows overlapping receives anyway.
 */
template<ByteBuffer Buf>
class UdpRecvSegmentsFuture {
    using Handle = UdpSocket::Handle;
public:
    using OutputType = UdpSegments<Buf>;

    UdpRecvSegmentsFuture(std::shared_ptr<Handle> handle, SingleThreadedUvExecutor* uv_exec, Buf buf)
        : m_handle(std::move(handle)), m_uv_exec(uv_exec), m_buf(std::move(buf)) {}

    PollResult<OutputType> poll(detail::Context& ctx) {
        for (;;) {
            if (m_readable) {
                auto r = m_readable->poll(ctx);
                if (r.isPending()) return PollPending;
                if (r.isDropped()) return PollDropped;
                if (r.isError())   return PollError(r.error());
                m_readable.reset();
            }

            SocketAddress sender;
            std::size_t segment_size = 0;
            ssize_t n = detail::recv_segments(m_handle->raw_fd,
                reinterpret_cast<std::byte*>(std::ranges::data(m_buf)), std::ranges::size(m_buf),
                sender, segment_size);
            if (n >= 0) {
                return OutputType{static_cast<std::size_t>(n), std::move(m_buf), sender, segment_size};
            }
            int err = errno;
            if (err != EAGAIN && err != EWOULDBLOCK) {
                return PollError(std::make_exception_ptr(std::system_error(
                    err, std::system_category(), "UdpSocket::recv_segments_from")));
            }

            // Slow path: nothing queued -- wait on the uv thread for readability.
            m_readable.emplace(with_context(*m_uv_exec,
                [](std::shared_ptr<Handle> handle) -> Coro<void> {
                    UvCallbackResult<ssize_t> result;
                    handle->handle.data = &result;
                    int r = uv_udp_recv_start(&handle->handle,
                        [](uv_handle_t*, size_t, uv_buf_t* out_buf) { *out_buf = uv_buf_init(nullptr, 0); },
                        [](uv_udp_t* h, ssize_t nread, const uv_buf_t*, const struct sockaddr*, unsigned) {
                            uv_udp_recv_stop(h);
                            static_cast<UvCallbackResult<ssize_t>*>(h->data)->complete(nread);
                        });
                    if (r < 0) detail::throw_uv_error(r, "UdpSocket::recv_segments_from");

                    // UV_ENOBUFS is the expected "readable" signal; anything
                    // else is a socket error.
                    auto [status] = co_await wait(result);
                    if (status < 0 && status != UV_ENOBUFS)
                        detail::throw_uv_error(static_cast<int>(status), "UdpSocket::recv_segments_from");
                }(m_handle)
            ));
        }
    }

    /// Only meaningful once the slow path is in flight; forwards to the
    /// spawned uv-thread task.
    void cancel() noexcept {
        if (m_readable) m_readable->cancel();
    }

private:
    std::shared_ptr<Handle>         m_handle;
    SingleThreadedUvExecutor*       m_uv_exec;
    Buf                             m_buf;
    std::optional<JoinHandle<void>> m_readable;   // engaged while waiting on the uv thread
};

template<ByteBuffer Buf>
UdpRecvSegmentsFuture<Buf> UdpSocket::recv_segments_from(Buf buf) {
    return UdpRecvSegmentsFuture<Buf>(m_handle, m_uv_exec, std::move(buf));
}

} // namespace coro
