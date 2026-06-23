#pragma once

#include <coro/detail/context.h>
#include <coro/detail/poll_result.h>
#include <coro/detail/waker.h>
#include <coro/io/byte_source.h>
#include <coro/io/decoder_stream.h>
#include <coro/io/ring_buffer.h>
#include <coro/runtime/single_threaded_uv_executor.h>
#include <coro/runtime/uv_future.h>
#include <coro/stream.h>
#include <coro/task/spawn_on.h>
#include <coro/coro.h>
#include <uv.h>
#include <cstddef>
#include <exception>
#include <format>
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
class PollStreamOverrunError : public std::exception {
public:
    PollStreamOverrunError(std::size_t m)
        : m_what(std::format("PollStreamOverrunError({})", m)), m_missed(m) {}
    /// @brief Number of packets dropped since the last overrun notification.
    std::size_t missed() const noexcept { return m_missed; }
    char const* what() const noexcept override { return m_what.c_str(); }
private:
    std::string m_what;
    std::size_t m_missed;
};

/**
 * @brief Construction options for PollStream.
 */
struct PollStreamOptions {
    /// Number of decoded packets to hold in the packet ring buffer (default 64).
    std::size_t      packet_buffer_capacity = 64;
    /// Raw byte accumulation buffer capacity in bytes (default 256 KB).
    std::size_t      byte_buffer_capacity   = 256 * 1024;
    /// What to do when the packet buffer fills up.
    BackpressureMode backpressure           = BackpressureMode::Block;
};

// ---------------------------------------------------------------------------
// PollStream<T>
// ---------------------------------------------------------------------------

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
 * - Two-level buffering (byte accumulation buffer + decoded packet buffer)
 * - Pluggable DecoderStream coroutine for any framing protocol
 * - Configurable backpressure policy (Block or Overrun) via PollStreamOptions
 * - Decoder runs inline on the libuv I/O thread — no executor scheduling per packet
 *
 * Thread safety: Not thread-safe. One consumer task per stream.
 *
 * @tparam T Decoded packet type
 */
template<typename T>
class PollStream {
public:
    using ItemType = T;

    /**
     * @brief Open a pollable file descriptor as a Stream (default options).
     *
     * @param fd              Open file descriptor (must support poll/epoll semantics).
     * @param decoder_factory Callable with signature
     *                        `DecoderStream<T>(ByteSource&, decoder_args...)`.
     *                        Called once; the ByteSource reference is valid for the
     *                        lifetime of the PollStream.
     * @param decoder_args    Extra arguments forwarded verbatim to decoder_factory
     *                        after the ByteSource reference:
     * @code
     * DecoderStream<Pkt> my_decoder(ByteSource& src, Config cfg) { ... }
     *
     * auto stream = PollStream<Pkt>::open(fd, my_decoder, cfg);
     * @endcode
     *
     * The caller retains ownership of the fd. Close it after the stream is destroyed.
     */
    template<typename DecoderFactory, typename... Args>
        requires(!std::same_as<std::remove_cvref_t<DecoderFactory>, PollStreamOptions>)
    [[nodiscard]] static PollStream open(int fd, DecoderFactory decoder_factory, Args&&... decoder_args);

    /**
     * @brief Open a pollable file descriptor as a Stream with explicit options.
     *
     * @param fd              Open file descriptor (must support poll/epoll semantics).
     * @param options         Buffer sizes and backpressure policy.
     * @param decoder_factory Callable with signature
     *                        `DecoderStream<T>(ByteSource&, decoder_args...)`.
     * @param decoder_args    Extra arguments forwarded to decoder_factory.
     * @code
     * auto stream = PollStream<Pkt>::open(fd,
     *     PollStreamOptions{.backpressure = BackpressureMode::Overrun},
     *     my_decoder, cfg);
     * @endcode
     */
    template<typename DecoderFactory, typename... Args>
    [[nodiscard]] static PollStream open(int fd, PollStreamOptions options, DecoderFactory decoder_factory, Args&&... decoder_args);

    /**
     * @brief Stream concept interface — poll for the next decoded packet.
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
     * @brief Close the fd and stop polling. Called automatically by destructor.
     */
    void close();

    PollStream(PollStream&&) noexcept;
    PollStream& operator=(PollStream&&) noexcept;
    PollStream(const PollStream&)            = delete;
    PollStream& operator=(const PollStream&) = delete;
    ~PollStream();

private:
    struct State;

    /// @brief Custom Waker that drives the decoder coroutine inline on the
    /// libuv I/O thread. wake() is called from poll_cb; it polls decoder_next
    /// until the decoder suspends (needs more bytes), the packet buffer fills,
    /// or the decoder exhausts. Holds a raw pointer to State — valid for the
    /// lifetime of State since LibuvWaker is owned by State.
    struct LibuvWaker : detail::Waker {
        State* owner;
        explicit LibuvWaker(State* s) : owner(s) {}
        void wake() override;
        detail::Rc<detail::Waker> clone() override {
            return detail::make_rc<LibuvWaker>(owner);
        }
    };

    std::shared_ptr<State>    m_state;
    SingleThreadedUvExecutor* m_uv_exec = nullptr;

    explicit PollStream(std::shared_ptr<State> state, SingleThreadedUvExecutor* uv_exec);

    static void poll_cb(uv_poll_t* handle, int status, int events);
    static Coro<void> ensure_polling_impl(std::shared_ptr<State> state);
    static Coro<void> close_impl(std::shared_ptr<State> state);
};

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

template<typename T>
struct PollStream<T>::State {
    uv_poll_t                                   poll_handle;
    int                                         fd          = -1;
    ByteSource                                  byte_source;
    LibuvWaker                                  libuv_waker{this};
    std::optional<DecoderStream<T>>             decoder;
    std::optional<NextFuture<DecoderStream<T>>> decoder_next;

    RingBuffer<T>                               packet_buffer;
    BackpressureMode                            backpressure_mode;

    std::mutex                                  mutex;
    std::size_t                                 overrun_count{0};
    std::shared_ptr<detail::Waker>              waker;
    std::exception_ptr                          error;
    bool                                        eof         = false;
    bool                                        polling     = false;
    bool                                        closing     = false;
    bool                                        initialized = false;

    State(int fd_, PollStreamOptions opts)
        : fd(fd_)
        , byte_source(opts.byte_buffer_capacity)
        , packet_buffer(opts.packet_buffer_capacity)
        , backpressure_mode(opts.backpressure)
    {
        poll_handle.data = this;
    }
};

} // namespace coro

#include "poll_stream.hpp"
