#pragma once

// Template method bodies for the desktop UdpSocket. Included at the bottom of
// udp_socket.h; never include this file directly.
//
// Every future here is a leaf future (no cancel()): while pending it holds only a
// weak waker in the socket's IoRegistration, so dropping it at any time is safe and
// consumes no datagram. See doc/design/udp_socket.md, "Send and receive futures".

#include <coro/detail/sys/udp.h>
#include <exception>
#include <ranges>
#include <system_error>
#include <type_traits>

namespace coro {

// ---------------------------------------------------------------------------
// send_to / send
// ---------------------------------------------------------------------------

/**
 * @brief Future returned by `UdpSocket::send()` / `send_to()`.
 *
 * Hand-written rather than a `Coro<Buf>` so the common case (the kernel accepts the
 * datagram at once) costs no coroutine frame: the first `poll()`, which
 * `FutureAwaitable::await_ready()` runs eagerly, does the non-blocking send and
 * returns `PollReady`. On EAGAIN (send buffer full) it waits for write readiness.
 *
 * A UDP datagram is sent atomically, so there is no partial-send bookkeeping.
 *
 * Race note: overlapping send()/send_to() calls on one socket aren't supported
 * (see udp_socket.h's Concurrency note); the second would replace the first's
 * waker, leaving the first unwoken.
 */
template<ByteBuffer Buf>
class UdpSendFuture {
    using State = detail::SocketState;
public:
    using OutputType = Buf;

    UdpSendFuture(std::shared_ptr<State> state, Buf buf, std::optional<SocketAddress> dest)
        : m_state(std::move(state)), m_buf(std::move(buf)), m_dest(dest) {}

    PollResult<Buf> poll(detail::Context& ctx) {
        auto result = m_state->reg.poll_io(IoDirection::Write, ctx, [this] {
            return detail::sys::udp_try_send(m_state->fd,
                reinterpret_cast<const std::byte*>(std::ranges::data(m_buf)),
                std::ranges::size(m_buf), m_dest ? &*m_dest : nullptr);
        });
        if (!result) return PollPending;
        if (!*result)
            return PollError(detail::socket_error(result->error(),
                m_dest ? "UdpSocket::send_to" : "UdpSocket::send"));
        return std::move(m_buf);
    }

private:
    std::shared_ptr<State>       m_state;
    Buf                          m_buf;
    std::optional<SocketAddress> m_dest;   // nullopt => send() on a connected socket
};

template<ByteBuffer Buf>
UdpSendFuture<Buf> UdpSocket::send_to(Buf buf, SocketAddress dest) {
    return UdpSendFuture<Buf>(m_state, std::move(buf), dest);
}

template<ByteBuffer Buf>
UdpSendFuture<Buf> UdpSocket::send(Buf buf) {
    return UdpSendFuture<Buf>(m_state, std::move(buf), std::nullopt);
}

// ---------------------------------------------------------------------------
// recv_from / recv
// ---------------------------------------------------------------------------

/**
 * @brief Future returned by `UdpSocket::recv_from()` (`WithSender = true`, yields
 * `{n, buf, sender}`) and `UdpSocket::recv()` (`WithSender = false`, yields `{n, buf}`).
 *
 * Hand-written for the same reason as `UdpSendFuture`: when a datagram is already
 * queued, the first `poll()` reads it with no coroutine frame. Otherwise it waits
 * for read readiness and retries.
 *
 * Race note: overlapping receives on one socket aren't supported (see udp_socket.h);
 * the second would replace the first's waker. If one happened anyway, a reader that
 * finds the socket emptied by the other just sees EAGAIN and waits again.
 */
template<ByteBuffer Buf, bool WithSender>
class UdpRecvFuture {
    using State = detail::SocketState;
public:
    using OutputType = std::conditional_t<WithSender,
        std::tuple<std::size_t, Buf, SocketAddress>, std::pair<std::size_t, Buf>>;

    UdpRecvFuture(std::shared_ptr<State> state, Buf buf)
        : m_state(std::move(state)), m_buf(std::move(buf)) {}

    PollResult<OutputType> poll(detail::Context& ctx) {
        SocketAddress sender;
        auto result = m_state->reg.poll_io(IoDirection::Read, ctx, [&] {
            // Connected recv(): the OS already filtered to our peer, so the sender
            // isn't decoded.
            return detail::sys::udp_try_recv(m_state->fd,
                reinterpret_cast<std::byte*>(std::ranges::data(m_buf)),
                std::ranges::size(m_buf), WithSender ? &sender : nullptr);
        });
        if (!result) return PollPending;
        if (!*result)
            return PollError(detail::socket_error(result->error(),
                WithSender ? "UdpSocket::recv_from" : "UdpSocket::recv"));
        if constexpr (WithSender) return OutputType{**result, std::move(m_buf), sender};
        else                      return OutputType{**result, std::move(m_buf)};
    }

private:
    std::shared_ptr<State> m_state;
    Buf                    m_buf;
};

template<ByteBuffer Buf>
UdpRecvFuture<Buf, true> UdpSocket::recv_from(Buf buf) {
    return UdpRecvFuture<Buf, true>(m_state, std::move(buf));
}

template<ByteBuffer Buf>
UdpRecvFuture<Buf, false> UdpSocket::recv(Buf buf) {
    return UdpRecvFuture<Buf, false>(m_state, std::move(buf));
}

// ---------------------------------------------------------------------------
// recv_segments_from
// ---------------------------------------------------------------------------

/**
 * @brief Future returned by `UdpSocket::recv_segments_from()`.
 *
 * Same as `UdpRecvFuture`, but reads with recvmsg() so the UDP_GRO segment size
 * comes back too.
 */
template<ByteBuffer Buf>
class UdpRecvSegmentsFuture {
    using State = detail::SocketState;
public:
    using OutputType = UdpSegments<Buf>;

    UdpRecvSegmentsFuture(std::shared_ptr<State> state, Buf buf)
        : m_state(std::move(state)), m_buf(std::move(buf)) {}

    PollResult<OutputType> poll(detail::Context& ctx) {
        SocketAddress sender;
        std::size_t segment_size = 0;
        auto result = m_state->reg.poll_io(IoDirection::Read, ctx, [&] {
            return detail::sys::udp_try_recv_segments(m_state->fd,
                reinterpret_cast<std::byte*>(std::ranges::data(m_buf)),
                std::ranges::size(m_buf), sender, segment_size);
        });
        if (!result) return PollPending;
        if (!*result)
            return PollError(detail::socket_error(result->error(), "UdpSocket::recv_segments_from"));
        return OutputType{**result, std::move(m_buf), sender, segment_size};
    }

private:
    std::shared_ptr<State> m_state;
    Buf                    m_buf;
};

template<ByteBuffer Buf>
UdpRecvSegmentsFuture<Buf> UdpSocket::recv_segments_from(Buf buf) {
    return UdpRecvSegmentsFuture<Buf>(m_state, std::move(buf));
}

} // namespace coro
