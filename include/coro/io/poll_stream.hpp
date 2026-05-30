#pragma once

// Template implementation for PollStream — included by poll_stream.h

#include <cerrno>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <coro/io/poll_stream.h>

namespace coro {

// ---------------------------------------------------------------------------
// LibuvWaker::wake()
//
// Drives the decoder coroutine inline on the libuv I/O thread. Called from
// poll_cb after bytes are written into byte_source, and from ensure_polling_impl
// to drain buffered bytes after backpressure is released.
// ---------------------------------------------------------------------------

template<typename T>
void PollStream<T>::LibuvWaker::wake() {
    State& state = *owner;

    detail::Context ctx(std::make_shared<LibuvWaker>(owner));

    bool should_notify = false;

    while (true) {
        // Block mode: stop driving if the packet buffer is full
        if (state.backpressure_mode == BackpressureMode::Block) {
            std::lock_guard lock(state.mutex);
            if (state.packet_buffer.full()) {
                if (state.polling) {
                    uv_poll_stop(&state.poll_handle);
                    state.polling = false;
                }
                break;
            }
        }

        auto result = state.decoder_next->poll(ctx);

        if (result.isReady()) {
            auto opt = std::move(result).value();
            if (opt.has_value()) {
                {
                    std::lock_guard lock(state.mutex);
                    if (state.backpressure_mode == BackpressureMode::Overrun &&
                        state.packet_buffer.full()) {
                        state.packet_buffer.pop();
                        ++state.overrun_count;
                    }
                    state.packet_buffer.push(std::move(*opt));
                }
                should_notify = true;
                continue;
            } else {
                // Decoder called co_return — stream exhausted
                std::lock_guard lock(state.mutex);
                state.eof = true;
                uv_poll_stop(&state.poll_handle);
                state.polling = false;
                should_notify = true;
                break;
            }
        } else if (result.isPending()) {
            // Decoder needs more bytes. Also stop polling if byte buffer is full.
            if (state.backpressure_mode == BackpressureMode::Block) {
                std::lock_guard lock(state.mutex);
                if (state.byte_source.writable_bytes() == 0 && state.polling) {
                    uv_poll_stop(&state.poll_handle);
                    state.polling = false;
                }
            }
            break;
        } else {
            // PollError: decoder threw or fd error propagated through ByteSource
            std::lock_guard lock(state.mutex);
            state.error = result.error();
            uv_poll_stop(&state.poll_handle);
            state.polling = false;
            should_notify = true;
            break;
        }
    }

    if (should_notify) {
        std::shared_ptr<detail::Waker> w;
        {
            std::lock_guard lock(state.mutex);
            w = std::exchange(state.waker, nullptr);
        }
        if (w) w->wake();
    }
}

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------

template<typename T>
PollStream<T>::PollStream(std::shared_ptr<State> state, SingleThreadedUvExecutor* uv_exec)
    : m_state(std::move(state))
    , m_uv_exec(uv_exec)
{}

template<typename T>
PollStream<T>::PollStream(PollStream&& other) noexcept
    : m_state(std::move(other.m_state))
    , m_uv_exec(other.m_uv_exec)
{}

template<typename T>
PollStream<T>& PollStream<T>::operator=(PollStream&& other) noexcept {
    if (this != &other) {
        close();
        m_state   = std::move(other.m_state);
        m_uv_exec = other.m_uv_exec;
    }
    return *this;
}

template<typename T>
PollStream<T>::~PollStream() {
    close();
}

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------

template<typename T>
template<typename DecoderFactory, typename... Args>
    requires(!std::same_as<std::remove_cvref_t<DecoderFactory>, PollStreamOptions>)
PollStream<T> PollStream<T>::open(int fd, DecoderFactory decoder_factory, Args&&... decoder_args)
{
    return PollStream<T>::open(fd, PollStreamOptions{},
                               decoder_factory, std::forward<Args>(decoder_args)...);
}

template<typename T>
template<typename DecoderFactory, typename... Args>
PollStream<T> PollStream<T>::open(int fd, PollStreamOptions options, DecoderFactory decoder_factory, Args&&... decoder_args)
{
    auto* uv_exec = &current_uv_executor();
    auto state = std::make_shared<State>(fd, options);

    // Two-phase init: State has a stable heap address so byte_source is stable.
    // Forward decoder_args to the factory alongside the ByteSource reference.
    state->decoder.emplace(
        decoder_factory(state->byte_source, std::forward<Args>(decoder_args)...));
    state->decoder_next.emplace(*state->decoder);

    return PollStream(std::move(state), uv_exec);
}

// ---------------------------------------------------------------------------
// poll_next
// ---------------------------------------------------------------------------

template<typename T>
PollResult<std::optional<T>> PollStream<T>::poll_next(detail::Context& ctx) {
    std::unique_lock lock(m_state->mutex);

    // Overrun notification: delivered before the next surviving packet.
    if (m_state->backpressure_mode == BackpressureMode::Overrun &&
        m_state->overrun_count > 0) {
        return PollError(std::make_exception_ptr(
            PollStreamOverrunError{std::exchange(m_state->overrun_count, 0)}
        ));
    }

    // Hot path: deliver buffered packets before errors or EOF.
    if (auto packet = m_state->packet_buffer.pop()) {
        if (m_state->backpressure_mode == BackpressureMode::Block &&
            !m_state->polling && !m_state->closing) {
            lock.unlock();
            with_context(*m_uv_exec, ensure_polling_impl(m_state)).detach();
        }
        return std::move(packet);
    }

    if (m_state->error)
        return PollError(std::exchange(m_state->error, nullptr));

    if (m_state->eof)
        return std::optional<T>(std::nullopt);

    m_state->waker = ctx.getWaker();

    if (!m_state->polling && !m_state->closing) {
        lock.unlock();
        with_context(*m_uv_exec, ensure_polling_impl(m_state)).detach();
        return PollPending;
    }

    return PollPending;
}

// ---------------------------------------------------------------------------
// close
// ---------------------------------------------------------------------------

template<typename T>
void PollStream<T>::close() {
    if (!m_state || !m_uv_exec) return;

    {
        std::lock_guard lock(m_state->mutex);
        if (m_state->closing) return;
        m_state->closing = true;
    }

    with_context(*m_uv_exec, close_impl(m_state)).detach();
}

// ---------------------------------------------------------------------------
// poll_cb
// ---------------------------------------------------------------------------

template<typename T>
void PollStream<T>::poll_cb(uv_poll_t* handle, int status, int events) {
    auto* state = static_cast<State*>(handle->data);

    // uv_poll-level error (e.g. fd closed externally): notify consumer directly.
    if (status < 0) {
        std::shared_ptr<detail::Waker> w;
        {
            std::lock_guard lock(state->mutex);
            state->error = std::make_exception_ptr(
                std::system_error(-status, std::system_category(), "uv_poll error"));
            uv_poll_stop(&state->poll_handle);
            state->polling = false;
            w = std::exchange(state->waker, nullptr);
        }
        if (w) w->wake();
        return;
    }

    if (!(events & UV_READABLE)) return;

    // Phase 1: greedy read — byte_source is libuv-thread-only, no lock needed.
    while (state->byte_source.writable_bytes() > 0) {
        auto span = state->byte_source.writable_span();
        if (span.empty()) break;

        ssize_t n = ::read(state->fd, span.data(), span.size());
        if (n > 0) {
            state->byte_source.commit_write(static_cast<std::size_t>(n));
        } else if (n == 0) {
            state->byte_source.set_eof();
            break;
        } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
            state->byte_source.set_error(std::make_exception_ptr(
                std::system_error(errno, std::system_category(), "read failed")));
            break;
        } else {
            break; // EAGAIN — no more data right now
        }
    }

    // Phase 2: drive decoder with all buffered bytes. LibuvWaker::wake() handles
    // packet production, backpressure gating, and consumer notification.
    // It runs even after a Phase 1 error so bytes already buffered are decoded first.
    state->libuv_waker.wake();
}

// ---------------------------------------------------------------------------
// ensure_polling_impl
// ---------------------------------------------------------------------------

template<typename T>
Coro<void> PollStream<T>::ensure_polling_impl(std::shared_ptr<State> state) {
    {
        std::lock_guard lock(state->mutex);
        if (state->closing || state->polling || state->error) co_return;
    }

    // fd EOF seen: drain any bytes still in the accumulation buffer via wake().
    if (state->byte_source.fd_eof()) {
        state->libuv_waker.wake();
        co_return;
    }

    if (state->eof) co_return;

    uv_loop_t* loop = current_uv_executor().loop();

    if (!state->initialized) {
        int r = uv_poll_init(loop, &state->poll_handle, state->fd);
        if (r < 0) {
            std::lock_guard lock(state->mutex);
            state->error = std::make_exception_ptr(
                std::system_error(-r, std::system_category(), "uv_poll_init failed"));
            if (auto w = std::exchange(state->waker, nullptr)) w->wake();
            co_return;
        }
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

// ---------------------------------------------------------------------------
// close_impl
// ---------------------------------------------------------------------------

template<typename T>
Coro<void> PollStream<T>::close_impl(std::shared_ptr<State> state) {
    if (!state->initialized) co_return;

    if (state->polling) {
        uv_poll_stop(&state->poll_handle);
        state->polling = false;
    }

    UvCallbackResult<int> result;
    auto* handle = reinterpret_cast<uv_handle_t*>(&state->poll_handle);
    handle->data = &result;
    uv_close(handle, [](uv_handle_t* h) {
        static_cast<UvCallbackResult<int>*>(h->data)->complete(0);
    });
    auto [ignored] = co_await wait(result);
    (void)ignored;
}

} // namespace coro
