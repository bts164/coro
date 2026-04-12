#pragma once

#include <coro/detail/context.h>
#include <coro/detail/poll_result.h>
#include <coro/detail/waker.h>
#include <coro/io/circular_byte_buffer.h>
#include <coro/io/decoder_concept.h>
#include <coro/io/ring_buffer.h>
#include <coro/runtime/io_service.h>
#include <uv.h>
#include <cstddef>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>

namespace coro {

// ---------------------------------------------------------------------------
// Backpressure policy
// ---------------------------------------------------------------------------

/**
 * @brief Controls what PollStream does when its packet buffer is full.
 */
enum class BackpressureMode {
    /**
     * Stop reading from the fd (uv_poll_stop) until the consumer drains some
     * packets. This lets kernel socket/pipe buffers fill, which applies
     * backpressure to the producer. Default behaviour.
     */
    Block,

    /**
     * Keep reading from the fd unconditionally. When the packet buffer is full,
     * the oldest buffered packet is silently dropped and the new packet takes its
     * place. The number of dropped packets is accumulated and delivered to the
     * consumer as a non-fatal PollStreamOverrunError between successive packets.
     *
     * Use this when the fd source cannot tolerate backpressure (e.g. a DMA-backed
     * character device where buffer stall causes a hardware fault) and occasional
     * data loss is acceptable.
     */
    Overrun,
};

/**
 * @brief Delivered to the consumer (as a non-fatal PollError) when one or more
 * packets were dropped due to Overrun backpressure mode.
 *
 * The stream remains open after this error. The consumer may catch it and
 * continue calling next() to receive subsequent packets.
 */
struct PollStreamOverrunError {
    std::size_t missed; ///< Number of packets dropped since the last delivery
};

/**
 * @brief Construction options for PollStream.
 */
struct PollStreamOptions {
    /// Number of decoded packets to hold in the packet ring buffer (default 64).
    std::size_t packet_buffer_capacity = 64;

    /// Raw byte buffer capacity in bytes (default 256 KB).
    std::size_t byte_buffer_capacity = 256 * 1024;

    /// What to do when the packet buffer fills up.
    BackpressureMode backpressure = BackpressureMode::Block;
};

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
 * - Configurable backpressure policy (Block or Overrun) via PollStreamOptions
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
     * @param fd          Open file descriptor (must support poll/epoll semantics)
     * @param decoder     Decoder instance (moved into stream)
     * @param options     Buffer sizes and backpressure policy (default: Block, 64 pkts, 256 KB)
     * @param io_service  IoService managing the uv_loop (defaults to current thread's)
     * @return PollStream instance
     *
     * The caller retains ownership of the fd. Close it after the stream is destroyed.
     */
    [[nodiscard]] static PollStream open(
        int fd,
        DecoderT decoder,
        PollStreamOptions options = {},
        IoService* io_service = nullptr
    );

    /**
     * @brief Stream concept interface — poll for the next packet.
     *
     * @return Ready(some(T))  - next packet available
     *         Ready(nullopt)  - stream exhausted (EOF)
     *         Pending         - no packet ready; waker registered
     *         Error(fatal)    - I/O error or framing error; stream is faulted
     *         Error(non-fatal)- PollStreamOverrunError (Overrun mode only); stream
     *                           remains open, continue calling next() for subsequent packets
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
                       PollStreamOptions options,
                       IoService* io_service);

    // libuv callbacks (run on I/O thread)
    static void poll_cb(uv_poll_t* handle, int status, int events);
    static void close_cb(uv_handle_t* handle);
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

    BackpressureMode                     backpressure_mode;

    // mutex protects all mutable state shared between poll_cb (libuv I/O thread)
    // and poll_next (executor thread): packet_buffer, overrun_count, error, eof,
    // polling, and waker. This eliminates TOCTOU races between overrun_count checks
    // and packet_buffer pops in poll_next. waker->wake() must be called AFTER
    // releasing the mutex to avoid deadlock with multi-threaded executors.
    std::mutex                           mutex;
    std::size_t                          overrun_count{0};
    std::shared_ptr<detail::Waker>       waker;
    std::exception_ptr                   error;            // fatal error
    bool                                 eof     = false;  // EOF seen on fd
    bool                                 polling = false;  // uv_poll_start active?

    State(int fd_, DecoderT decoder_, PollStreamOptions opts)
        : fd(fd_)
        , byte_buffer(opts.byte_buffer_capacity)
        , packet_buffer(opts.packet_buffer_capacity)
        , decoder(std::move(decoder_))
        , backpressure_mode(opts.backpressure)
    {
        poll_handle.data = this;
    }
};

} // namespace coro

// Include template implementation
#include "poll_stream.hpp"
