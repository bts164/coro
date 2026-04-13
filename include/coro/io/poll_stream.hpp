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
                                    PollStreamOptions options,
                                    IoService* io_service)
    : m_state(std::make_shared<State>(fd, std::move(decoder), options))
    , m_io_service(io_service)
{
    // uv_poll_init is deferred to EnsurePollingRequest::execute(), which runs on
    // the I/O thread. Calling uv_poll_init here would race with the event loop.
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
    PollStreamOptions options,
    IoService* io_service)
{
    // Use current thread's IoService if not specified
    if (!io_service) {
        io_service = &current_io_service();
    }
    return PollStream(fd, std::move(decoder), options, io_service);
}

// ---------------------------------------------------------------------------
// Stream Interface
// ---------------------------------------------------------------------------

template<typename T, Decoder DecoderT>
PollResult<std::optional<T>> PollStream<T, DecoderT>::poll_next(detail::Context& ctx) {
    std::unique_lock lock(m_state->mutex);

    // Overrun notification: delivered before the next surviving packet so the
    // consumer knows N packets were dropped immediately before it.
    if (m_state->backpressure_mode == BackpressureMode::Overrun &&
        m_state->overrun_count > 0) {
        return PollError(std::make_exception_ptr(
            PollStreamOverrunError{std::exchange(m_state->overrun_count, 0)}
        ));
    }

    // HOT PATH: deliver buffered packets before errors. Any packets decoded from
    // bytes that arrived before an error are returned to the consumer first; the
    // error surfaces only once the packet buffer is empty.
    if (auto packet = m_state->packet_buffer.pop()) {
        // In Block mode, request polling restart on the I/O thread now that
        // space has opened in the packet buffer.
        // In Overrun mode polling is never stopped, so no action needed.
        if (m_state->backpressure_mode == BackpressureMode::Block &&
            !m_state->polling && !m_state->closing) {
            m_io_service->submit(std::make_unique<EnsurePollingRequest>(m_state));
        }

        return std::move(packet);
    }

    // Packet buffer drained — now surface any pending error or EOF.
    if (m_state->error) {
        return PollError(std::exchange(m_state->error, nullptr));
    }

    if (m_state->eof && m_state->byte_buffer.readable_bytes() == 0) {
        return std::optional<T>(std::nullopt); // Stream exhausted
    }

    // Packet buffer empty but more data may arrive - register waker and suspend.
    m_state->waker = ctx.getWaker();

    // Ensure polling is active. The request is executed on the I/O thread;
    // uv_poll_start (and uv_poll_init on the first call) happen there safely.
    if (!m_state->polling && !m_state->closing) {
        m_io_service->submit(std::make_unique<EnsurePollingRequest>(m_state));
    }

    return PollPending;
}

template<typename T, Decoder DecoderT>
void PollStream<T, DecoderT>::close() {
    if (!m_state || !m_io_service) return;

    {
        std::lock_guard lock(m_state->mutex);
        if (m_state->closing) return; // already submitted
        m_state->closing = true;
    }

    // Submit CloseRequest to the I/O thread. It holds a shared_ptr<State> so
    // State (which contains the embedded poll_handle) stays alive until after
    // uv_close fires close_cb — no use-after-free even if ~PollStream() drops
    // m_state before the callback fires.
    m_io_service->submit(std::make_unique<CloseRequest>(m_state));
}

// ---------------------------------------------------------------------------
// libuv Callbacks
// ---------------------------------------------------------------------------

template<typename T, Decoder DecoderT>
void PollStream<T, DecoderT>::poll_cb(uv_poll_t* handle, int status, int events) {
    auto* state = static_cast<State*>(handle->data);

    std::shared_ptr<detail::Waker> waker_to_wake;

    // uv_poll error (e.g., fd closed externally) — brief lock to update shared state
    if (status < 0) {
        std::lock_guard lock(state->mutex);
        state->error = std::make_exception_ptr(
            std::system_error(-status, std::system_category(), "uv_poll error")
        );
        uv_poll_stop(&state->poll_handle);
        state->polling = false;
        waker_to_wake = std::exchange(state->waker, nullptr);
    }
    else if (events & UV_READABLE) {
        bool should_wake = false;

        // PHASE 1: GREEDY READ — no lock needed.
        // byte_buffer is only ever touched by poll_cb; poll_next never accesses it.
        // EOF and error paths take a brief lock to update the shared flags.
        while (state->byte_buffer.writable_bytes() > 0) {
            auto writable_span = state->byte_buffer.writable_span();
            if (writable_span.empty()) break;

            ssize_t n = ::read(state->fd, writable_span.data(), writable_span.size());

            if (n > 0) {
                state->byte_buffer.commit_write(n);
            }
            else if (n == 0) {
                std::lock_guard lock(state->mutex);
                state->eof = true;
                uv_poll_stop(&state->poll_handle);
                state->polling = false;
                should_wake = true;
                break;
            }
            else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                std::lock_guard lock(state->mutex);
                state->error = std::make_exception_ptr(
                    std::system_error(errno, std::system_category(), "read failed")
                );
                uv_poll_stop(&state->poll_handle);
                state->polling = false;
                should_wake = true;
                break;
            }
            else {
                break; // EAGAIN — no more data right now
            }
        }

        // PHASE 2: GREEDY DECODE — always runs, including after a Phase 1 error.
        // Any bytes already read into byte_buffer before the error are decoded into
        // packets and queued. poll_next delivers those packets to the consumer before
        // surfacing the error, so the consumer observes every packet whose bytes
        // arrived on the fd before the fault.
        //
        // byte_buffer is private to poll_cb — no lock needed for reads or consumes.
        // decoder.decode() may do large memcpy's — must not hold the lock during it.
        //
        // The lock is taken in two targeted spots per iteration:
        //   Block mode:   (1) pre-check full() before decode to avoid wasting work
        //                 (2) push() after decode
        //   Overrun mode: (1) pop() + ++overrun_count + push() as one atomic update
        //
        // In Block mode the pre-check is safe: poll_next only pops, so the buffer
        // can only gain room between the check and the push — never lose it.
        bool decode_error = false;
        try {
            while (true) {
                // Block mode: verify room exists before committing to a decode.
                if (state->backpressure_mode == BackpressureMode::Block) {
                    std::lock_guard lock(state->mutex);
                    if (state->packet_buffer.full()) break;
                }

                auto readable_span = state->byte_buffer.readable_span();
                if (readable_span.empty()) break;

                std::size_t consumed = 0;
                // No lock — decode may involve expensive memcpy of large payloads.
                auto packet = state->decoder.decode(readable_span, consumed);

                // ALWAYS consume decoded bytes, even on incomplete packet.
                // Handles wrap-around in the circular buffer correctly.
                if (consumed > 0) {
                    state->byte_buffer.consume(consumed);
                }

                if (packet) {
                    std::lock_guard lock(state->mutex);
                    if (state->backpressure_mode == BackpressureMode::Overrun) {
                        if (state->packet_buffer.full()) {
                            state->packet_buffer.pop(); // drop oldest
                            ++state->overrun_count;
                        }
                    }
                    state->packet_buffer.push(std::move(*packet));
                    should_wake = true;
                } else {
                    // Incomplete packet — continue only if decoder made progress
                    // (handles circular-buffer wrap where a packet spans two spans).
                    if (consumed == 0) break;
                }
            }
        } catch (...) {
            // Decoder threw — framing error (e.g. invalid footer magic).
            // Already-decoded packets remain in packet_buffer and will be delivered
            // to the consumer before this error, consistent with the Phase 1 policy.
            std::lock_guard lock(state->mutex);
            state->error = std::current_exception();
            uv_poll_stop(&state->poll_handle);
            state->polling = false;
            decode_error = true;
            should_wake = true;
        }

        // Final lock: apply Block-mode backpressure gate and extract waker only
        // if there is something new for the consumer to observe.
        if (should_wake) {
            std::lock_guard lock(state->mutex);
            if (!decode_error && state->backpressure_mode == BackpressureMode::Block) {
                if ((state->packet_buffer.full() ||
                     state->byte_buffer.writable_bytes() == 0) && state->polling) {
                    uv_poll_stop(&state->poll_handle);
                    state->polling = false;
                }
            }
            waker_to_wake = std::exchange(state->waker, nullptr);
        }
    }

    // Wake the consumer outside the mutex. On a multi-threaded executor the resumed
    // task may call poll_next immediately; releasing first prevents deadlock.
    if (waker_to_wake) {
        waker_to_wake->wake();
    }
}

template<typename T, Decoder DecoderT>
void PollStream<T, DecoderT>::close_cb(uv_handle_t* handle) {
    // Release the shared_ptr that close() parked in handle->data. When this
    // drops the last reference to State, State (and the embedded poll_handle)
    // are freed — safely, now that libuv is done with the handle.
    delete static_cast<std::shared_ptr<State>*>(handle->data);
}


// ---------------------------------------------------------------------------
// IoRequest implementations (execute() runs on the libuv I/O thread)
// ---------------------------------------------------------------------------

template<typename T, Decoder DecoderT>
void PollStream<T, DecoderT>::EnsurePollingRequest::execute(uv_loop_t* loop) {
    // Guard: bail out if the stream is closing, already polling, or has a fatal error.
    {
        std::lock_guard lock(state->mutex);
        if (state->closing || state->polling || state->error) return;
    }

    // EOF path: no more bytes can arrive from the fd, but byte_buffer may still hold
    // undecoded bytes (e.g. Block mode filled the packet buffer before all bytes were
    // decoded — the consumer drained a packet slot and re-submitted this request).
    // Run Phase 2 decode here to drain byte_buffer into packet_buffer, then wake.
    if (state->eof) {
        bool should_wake = false;
        try {
            while (true) {
                {
                    std::lock_guard lock(state->mutex);
                    if (state->packet_buffer.full()) break;
                }

                auto readable_span = state->byte_buffer.readable_span();
                if (readable_span.empty()) {
                    should_wake = true; // byte_buffer fully drained — signal EOF to consumer
                    break;
                }

                std::size_t consumed = 0;
                auto packet = state->decoder.decode(readable_span, consumed);

                if (consumed > 0) {
                    state->byte_buffer.consume(consumed);
                }

                if (packet) {
                    std::lock_guard lock(state->mutex);
                    state->packet_buffer.push(std::move(*packet));
                    should_wake = true;
                } else {
                    if (consumed == 0) {
                        // Incomplete packet at EOF — remaining bytes can never form a
                        // complete packet. Surface EOF to the consumer now.
                        should_wake = true;
                        break;
                    }
                }
            }
        } catch (...) {
            std::lock_guard lock(state->mutex);
            state->error = std::current_exception();
            should_wake = true;
        }

        if (should_wake) {
            std::shared_ptr<detail::Waker> w;
            {
                std::lock_guard lock(state->mutex);
                w = std::exchange(state->waker, nullptr);
            }
            if (w) w->wake();
        }
        return;
    }

    // First-time initialisation — uv_poll_init must run on the I/O thread.
    if (!state->initialized) {
        int r = uv_poll_init(loop, &state->poll_handle, state->fd);
        if (r < 0) {
            std::lock_guard lock(state->mutex);
            state->error = std::make_exception_ptr(
                std::system_error(-r, std::system_category(), "uv_poll_init failed"));
            if (auto w = std::exchange(state->waker, nullptr)) w->wake();
            return;
        }
        // poll_handle.data was already set to state.get() in the State constructor
        // and is preserved by uv_poll_init (libuv does not touch the data field).
        state->initialized = true;
    }

    int r = uv_poll_start(&state->poll_handle, UV_READABLE, poll_cb);
    {
        std::lock_guard lock(state->mutex);
        if (r == 0) {
            state->polling = true;
        } else {
            state->error = std::make_exception_ptr(
                std::system_error(-r, std::system_category(), "uv_poll_start failed"));
            if (auto w = std::exchange(state->waker, nullptr)) w->wake();
        }
    }
}

template<typename T, Decoder DecoderT>
void PollStream<T, DecoderT>::CloseRequest::execute(uv_loop_t*) {
    if (!state->initialized) {
        // uv_poll_init was never called — nothing for libuv to close.
        return;
    }

    if (state->polling) {
        uv_poll_stop(&state->poll_handle);
        state->polling = false;
    }

    // Park a shared_ptr in handle->data to keep State alive until close_cb fires.
    // uv_poll_stop above prevents any further poll_cb dispatches that read handle->data
    // as State*; it is safe to overwrite it here.
    auto* handle = reinterpret_cast<uv_handle_t*>(&state->poll_handle);
    handle->data  = new std::shared_ptr<State>(std::move(state));
    uv_close(handle, close_cb);
    // state is now moved-from (null); CloseRequest destruction is a no-op.
}

// ---------------------------------------------------------------------------
// Note: PollStream is a template and will be instantiated in user code
// when they provide their own Decoder. No explicit instantiations needed
// here in the library.
// ---------------------------------------------------------------------------

} // namespace coro
