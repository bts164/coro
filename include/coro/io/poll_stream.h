#pragma once

#include <coro/detail/context.h>
#include <coro/detail/poll_result.h>
#include <coro/detail/waker.h>
#include <coro/io/circular_byte_buffer.h>
#include <coro/io/decoder_concept.h>
#include <coro/io/ring_buffer.h>
#include <coro/runtime/io_service.h>
#include <uv.h>
#include <atomic>
#include <cstddef>
#include <exception>
#include <memory>
#include <optional>

namespace coro {

/**
 * @brief Event-driven Stream for pollable file descriptors with framed protocols.
 *
 * PollStream provides an efficient Stream interface for reading structured data
 * from pollable file descriptors (character devices, pipes, sockets). It uses
 * uv_poll_t for event-driven I/O (no threadpool overhead) and maintains internal
 * buffers to amortize executor scheduling costs.
 *
 * Key features:
 * - Event-driven via uv_poll_t (epoll notification when data ready)
 * - Two-level buffering (byte buffer + packet buffer)
 * - Pluggable Decoder for any framing protocol
 * - Automatic backpressure when buffers fill
 * - Zero-copy payload support (for decoders implementing ZeroCopyDecoder)
 *
 * Thread safety: Not thread-safe. One consumer task per stream.
 *
 * @tparam T Decoded packet type
 * @tparam DecoderT Decoder implementing Decoder concept
 */
template<typename T, Decoder DecoderT>
class PollStream {
public:
    using ItemType = T;

    /**
     * @brief Open a pollable file descriptor as a Stream.
     *
     * @param fd Open file descriptor (must support poll/epoll semantics)
     * @param decoder Decoder instance (moved into stream)
     * @param packet_buffer_capacity Number of decoded packets to buffer (default 64)
     * @param byte_buffer_capacity Raw byte buffer size in bytes (default 256KB)
     * @param io_service IoService managing the uv_loop (defaults to current)
     * @return PollStream instance
     *
     * The caller retains ownership of the fd. Close it after the stream is destroyed.
     */
    [[nodiscard]] static PollStream open(
        int fd,
        DecoderT decoder,
        std::size_t packet_buffer_capacity = 64,
        std::size_t byte_buffer_capacity = 256 * 1024,
        IoService* io_service = nullptr
    );

    /**
     * @brief Stream concept interface — poll for the next packet.
     *
     * @return Ready(some(T)) - next packet available
     *         Ready(nullopt) - stream exhausted (EOF)
     *         Pending        - no packet ready; waker registered
     *         Error          - I/O error or framing error
     */
    PollResult<std::optional<T>> poll_next(detail::Context& ctx);

    /**
     * @brief Close the fd and stop polling.
     *
     * Called automatically by destructor.
     */
    void close();

    PollStream(PollStream&&) noexcept;
    PollStream& operator=(PollStream&&) noexcept;
    PollStream(const PollStream&) = delete;
    PollStream& operator=(const PollStream&) = delete;

    ~PollStream();

private:
    struct State; // forward declaration
    std::shared_ptr<State> m_state;
    IoService*             m_io_service;

    explicit PollStream(int fd, DecoderT decoder,
                       std::size_t pkt_buf_cap, std::size_t byte_buf_cap,
                       IoService* io_service);

    // libuv callbacks (run on I/O thread)
    static void poll_cb(uv_poll_t* handle, int status, int events);
    static void close_cb(uv_handle_t* handle);

    // Helper to wake consumer task
    static void wake_consumer(State* state);
};

// ---------------------------------------------------------------------------
// Internal State
// ---------------------------------------------------------------------------

template<typename T, Decoder DecoderT>
struct PollStream<T, DecoderT>::State {
    uv_poll_t                                   poll_handle;      // libuv poll handle
    int                                         fd = -1;          // file descriptor

    // Two-level buffering
    CircularByteBuffer                          byte_buffer;      // Raw bytes from fd
    RingBuffer<T>                               packet_buffer;    // Decoded packets

    DecoderT                                    decoder;          // Stateful frame decoder

    std::atomic<std::shared_ptr<detail::Waker>> waker{nullptr};  // Stored waker for wake()
    std::exception_ptr                          error;            // Captured error
    bool                                        eof = false;      // EOF seen on fd
    bool                                        polling = false;  // uv_poll_start active?

    State(int fd_, DecoderT decoder_,
          std::size_t pkt_cap, std::size_t byte_cap)
        : fd(fd_)
        , byte_buffer(byte_cap)
        , packet_buffer(pkt_cap)
        , decoder(std::move(decoder_))
    {
        poll_handle.data = this;
    }
};

} // namespace coro

// Include template implementation
#include "poll_stream.hpp"
