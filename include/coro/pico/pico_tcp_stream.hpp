#pragma once

// Template implementations for PicoTcpStream::read<Buf> and PicoTcpStream::write<Buf>.
// Included at the bottom of pico_tcp_stream.h — not meant to be included directly.

#include <coro/pico/pico_tcp_stream.h>
#include <coro/detail/context.h>
#include <coro/detail/poll_result.h>
#include <algorithm>
#include <cstring>
#include <ranges>
#include <stdexcept>

namespace coro {

template<ByteBuffer Buf>
Coro<std::pair<std::size_t, Buf>> PicoTcpStream::read(Buf buf) {
    // DataReady suspends until rx_buf has data, EOF is signalled, or an error occurs.
    struct DataReady {
        using OutputType = void;
        std::shared_ptr<PicoTcpContext> ctx;

        PollResult<void> poll(detail::Context& cx) {
            if (!ctx->rx_buf.empty() || ctx->rx_eof || ctx->errored)
                return PollReady;
            // RACE CONDITION NOTE: safe because PicoExecutor is single-threaded and
            // on_recv() only fires from cyw43_arch_poll() on the same thread. There
            // is no window between the empty-check and waker registration.
            ctx->rx_waker = cx.getWaker();
            return PollPending;
        }
    };

    if (m_ctx->rx_buf.empty() && !m_ctx->rx_eof && !m_ctx->errored)
        co_await DataReady{m_ctx};

    if (m_ctx->errored)
        throw std::runtime_error("PicoTcpStream::read: connection error");

    // EOF — return 0 bytes with the original buffer
    if (m_ctx->rx_buf.empty()) {
        co_return std::pair<std::size_t, Buf>{0, std::move(buf)};
    }

    auto n = std::min(std::ranges::size(buf), m_ctx->rx_buf.size());
    std::memcpy(
        reinterpret_cast<uint8_t*>(std::ranges::data(buf)),
        m_ctx->rx_buf.data(),
        n);
    m_ctx->rx_buf.erase(m_ctx->rx_buf.begin(),
                        m_ctx->rx_buf.begin() + static_cast<std::ptrdiff_t>(n));

    co_return std::pair<std::size_t, Buf>{n, std::move(buf)};
}

template<ByteBuffer Buf>
Coro<Buf> PicoTcpStream::write(Buf buf) {
    // SpaceAvailable suspends while tcp_sndbuf() == 0 (send buffer full).
    // Woken by on_sent when remote ACKs data, freeing send buffer space.
    struct SpaceAvailable {
        using OutputType = void;
        std::shared_ptr<PicoTcpContext> ctx;

        PollResult<void> poll(detail::Context& cx) {
            if (ctx->errored || ctx->pcb == nullptr)
                return PollReady;  // caller will check errored and throw
            if (tcp_sndbuf(ctx->pcb) > 0)
                return PollReady;
            ctx->tx_waker = cx.getWaker();
            return PollPending;
        }
    };

    if (m_ctx->errored || m_ctx->pcb == nullptr)
        throw std::runtime_error("PicoTcpStream::write: connection error");

    const auto* src = reinterpret_cast<const uint8_t*>(
        static_cast<const void*>(std::ranges::data(buf)));
    std::size_t total  = std::ranges::size(buf);
    std::size_t offset = 0;

    while (offset < total) {
        if (m_ctx->errored || m_ctx->pcb == nullptr)
            throw std::runtime_error("PicoTcpStream::write: connection error");

        uint16_t avail = tcp_sndbuf(m_ctx->pcb);
        if (avail == 0) {
            // Flush what is already queued then wait for the remote to ACK some data.
            tcp_output(m_ctx->pcb);
            co_await SpaceAvailable{m_ctx};
            continue;
        }

        uint16_t chunk = static_cast<uint16_t>(
            std::min<std::size_t>(avail, total - offset));

        // TCP_WRITE_FLAG_COPY: lwIP copies the data into its own buffers immediately,
        // so src only needs to be valid until tcp_write returns.
        err_t err = tcp_write(m_ctx->pcb, src + offset, chunk, TCP_WRITE_FLAG_COPY);
        if (err != ERR_OK)
            throw std::runtime_error("PicoTcpStream::write: tcp_write failed");

        offset += chunk;
    }

    tcp_output(m_ctx->pcb);
    co_return std::move(buf);
}

} // namespace coro
