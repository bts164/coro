// Template implementation for PollStream
// This file is included by poll_stream.h and should not be compiled separately

#include <cerrno>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <coro/io/poll_stream.h>

namespace coro {

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------

template<typename T, Decoder DecoderT>
PollStream<T, DecoderT>::PollStream(int fd, DecoderT decoder,
                                    std::size_t pkt_buf_cap, std::size_t byte_buf_cap,
                                    IoService* io_service)
    : m_state(std::make_shared<State>(fd, std::move(decoder), pkt_buf_cap, byte_buf_cap))
    , m_io_service(io_service)
{
    // Initialize uv_poll_t handle
    int result = uv_poll_init(io_service->loop(), &m_state->poll_handle, fd);
    if (result < 0) {
        throw std::system_error(-result, std::system_category(), "uv_poll_init failed");
    }
}

template<typename T, Decoder DecoderT>
PollStream<T, DecoderT>::PollStream(PollStream&& other) noexcept
    : m_state(std::move(other.m_state))
    , m_io_service(other.m_io_service)
{
}

template<typename T, Decoder DecoderT>
PollStream<T, DecoderT>& PollStream<T, DecoderT>::operator=(PollStream&& other) noexcept {
    if (this != &other) {
        close();
        m_state = std::move(other.m_state);
        m_io_service = other.m_io_service;
    }
    return *this;
}

template<typename T, Decoder DecoderT>
PollStream<T, DecoderT>::~PollStream() {
    close();
}

// ---------------------------------------------------------------------------
// Static Factory
// ---------------------------------------------------------------------------

template<typename T, Decoder DecoderT>
PollStream<T, DecoderT> PollStream<T, DecoderT>::open(
    int fd,
    DecoderT decoder,
    std::size_t packet_buffer_capacity,
    std::size_t byte_buffer_capacity,
    IoService* io_service)
{
    // Use current thread's IoService if not specified
    if (!io_service) {
        io_service = &current_io_service();
    }
    return PollStream(fd, std::move(decoder), packet_buffer_capacity, byte_buffer_capacity, io_service);
}

// ---------------------------------------------------------------------------
// Stream Interface
// ---------------------------------------------------------------------------

template<typename T, Decoder DecoderT>
PollResult<std::optional<T>> PollStream<T, DecoderT>::poll_next(detail::Context& ctx) {
    // Check for errors first
    if (m_state->error) {
        return PollError(std::exchange(m_state->error, nullptr));
    }

    // HOT PATH: try to pop from packet buffer without suspending
    if (auto packet = m_state->packet_buffer.pop()) {
        // Got a buffered packet - return immediately

        // Re-enable polling if we were in backpressure
        if (!m_state->polling && !m_state->eof) {
            int result = uv_poll_start(&m_state->poll_handle, UV_READABLE, poll_cb);
            if (result == 0) {
                m_state->polling = true;
            }
        }

        return std::move(packet);
    }

    // Packet buffer empty - check if stream is done
    if (m_state->eof && m_state->byte_buffer.readable_bytes() == 0) {
        return std::optional<T>(std::nullopt); // Stream exhausted
    }

    // Packet buffer empty but more data may arrive - register waker and suspend
    m_state->waker.store(
        ctx.getWaker(),
        std::memory_order_release
    );

    // Ensure polling is active
    if (!m_state->polling && !m_state->eof) {
        int result = uv_poll_start(&m_state->poll_handle, UV_READABLE, poll_cb);
        if (result == 0) {
            m_state->polling = true;
        }
    }

    return PollPending;
}

template<typename T, Decoder DecoderT>
void PollStream<T, DecoderT>::close() {
    if (!m_state) return;

    // Stop polling
    if (m_state->polling) {
        uv_poll_stop(&m_state->poll_handle);
        m_state->polling = false;
    }

    // Close handle (async)
    uv_close(reinterpret_cast<uv_handle_t*>(&m_state->poll_handle), close_cb);
}

// ---------------------------------------------------------------------------
// libuv Callbacks
// ---------------------------------------------------------------------------

template<typename T, Decoder DecoderT>
void PollStream<T, DecoderT>::poll_cb(uv_poll_t* handle, int status, int events) {
    auto* state = static_cast<State*>(handle->data);

    // uv_poll error (e.g., fd closed externally)
    if (status < 0) {
        state->error = std::make_exception_ptr(
            std::system_error(-status, std::system_category(), "uv_poll error")
        );
        uv_poll_stop(&state->poll_handle);
        state->polling = false;
        wake_consumer(state);
        return;
    }

    if (events & UV_READABLE) {
        // PHASE 1: GREEDY READ - fill byte buffer with raw data
        while (state->byte_buffer.writable_bytes() > 0) {
            auto writable_span = state->byte_buffer.writable_span();
            if (writable_span.empty()) break;

            ssize_t n = ::read(state->fd, writable_span.data(), writable_span.size());

            if (n > 0) {
                // Got data - advance byte buffer write pointer
                state->byte_buffer.commit_write(n);
            }
            else if (n == 0) {
                // EOF - stream exhausted
                state->eof = true;
                uv_poll_stop(&state->poll_handle);
                state->polling = false;
                break;
            }
            else if (n == -1) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    // No more data available right now - normal exit
                    break;
                }
                // Real error
                state->error = std::make_exception_ptr(
                    std::system_error(errno, std::system_category(), "read failed")
                );
                uv_poll_stop(&state->poll_handle);
                state->polling = false;
                break;
            }
        }

        // PHASE 2: GREEDY DECODE - extract complete packets from byte buffer
        try {
            while (!state->packet_buffer.full()) {
                auto readable_span = state->byte_buffer.readable_span();
                if (readable_span.empty()) break;

                std::size_t consumed = 0;
                auto packet = state->decoder.decode(readable_span, consumed);

                // ALWAYS consume bytes that were read, even if packet incomplete
                // This handles wrap-around in circular buffer correctly
                if (consumed > 0) {
                    state->byte_buffer.consume(consumed);
                }

                if (packet) {
                    // Got a complete packet - move to packet buffer
                    (void)state->packet_buffer.push(std::move(*packet));
                } else {
                    // Need more bytes - continue if decoder made progress
                    // (handles wrap-around case where packet spans multiple readable_span() calls)
                    if (consumed == 0) {
                        break;  // No progress, need more data from fd
                    }
                    // else: decoder consumed some bytes, try again (might have more data after wrap)
                }
            }
        } catch (const std::exception& e) {
            // Decoder threw - framing error (e.g., invalid footer magic)
            state->error = std::current_exception();
            uv_poll_stop(&state->poll_handle);
            state->polling = false;
            wake_consumer(state);
            return;
        }

        // Apply backpressure if packet buffer full
        if (state->packet_buffer.full() && state->polling) {
            uv_poll_stop(&state->poll_handle);
            state->polling = false;
        }

        // Also apply backpressure if byte buffer full (can't read more until decoded)
        if (state->byte_buffer.writable_bytes() == 0 && state->polling) {
            uv_poll_stop(&state->poll_handle);
            state->polling = false;
        }

        // Wake the consumer task (SINGLE wake for potentially many packets)
        wake_consumer(state);
    }
}

template<typename T, Decoder DecoderT>
void PollStream<T, DecoderT>::close_cb(uv_handle_t* handle) {
    // Cleanup after handle closed
    // The State is held by shared_ptr, so it will be destroyed when last ref drops
    // Nothing to do here
}

template<typename T, Decoder DecoderT>
void PollStream<T, DecoderT>::wake_consumer(State* state) {
    auto waker = state->waker.exchange(nullptr, std::memory_order_acq_rel);
    if (waker) {
        waker->wake();
    }
}

// ---------------------------------------------------------------------------
// Explicit Template Instantiations
// ---------------------------------------------------------------------------
//
// Note: PollStream is a template and will be instantiated in user code
// when they provide their own Decoder. No explicit instantiations needed
// here in the library.
//
// For examples of concrete decoders, see test/pcie_decoder.h

} // namespace coro
