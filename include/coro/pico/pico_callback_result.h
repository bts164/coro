#pragma once
#ifndef CORO_PICO
#error "pico_callback_result.h requires CORO_PICO to be defined. This header is only for Raspberry Pi Pico builds."
#endif

// Pico-specific one-shot bridge between an lwIP callback and an awaiting coroutine.
//
// Analogous to UvCallbackResult / UvFuture in the libuv backend, but with no mutex.
// All lwIP callbacks fire synchronously on the same thread as the executor when using
// CYW43_ARCH_POLL mode (callbacks arrive during cyw43_arch_poll()).
//
// CAUTION: if using CYW43_ARCH_THREADSAFE_BACKGROUND, lwIP callbacks can arrive from
// an IRQ or second core. In that case, protect waker and value with a critical_section_t
// rather than relying on single-threaded ordering.

#include <coro/detail/context.h>
#include <coro/detail/poll_result.h>
#include <coro/future.h>
#include <memory>
#include <optional>
#include <tuple>

namespace coro {

// ---------------------------------------------------------------------------
// PicoCallbackResult<Args...>
// ---------------------------------------------------------------------------

/**
 * @brief Shared state between an lwIP callback and a @ref PicoFuture.
 *
 * Allocate on the coroutine frame (stack), store its address in a callback
 * closure, then `co_await pico_wait(result)`. The callback calls `complete(args...)`
 * to store the result and wake the awaiting coroutine.
 *
 * ### Lifetime
 * The object must outlive the callback. `co_await pico_wait(result)` keeps the
 * coroutine suspended (and therefore the frame alive) until `complete()` fires,
 * so a stack-allocated `PicoCallbackResult` is safe for a direct `co_await`.
 *
 * @tparam Args  Argument types forwarded to `complete()`. May be empty for a
 *               bare signal with no payload.
 */
template<typename... Args>
struct PicoCallbackResult {
    std::shared_ptr<detail::Waker>     waker;  // set once by PicoFuture::poll()
    std::optional<std::tuple<Args...>> value;  // set once by complete()

    /// Called from the lwIP callback on the executor thread. Not thread-safe —
    /// must only be called from the CYW43_ARCH_POLL execution context.
    void complete(Args... args) {
        value.emplace(std::forward<Args>(args)...);
        if (waker) {
            auto w = std::move(waker);
            w->wake();
        }
    }
};

// ---------------------------------------------------------------------------
// PicoFuture<Args...>
// ---------------------------------------------------------------------------

/**
 * @brief One-shot Future that completes when the associated lwIP callback fires.
 *
 * Pair with @ref PicoCallbackResult: declare one on the coroutine frame, pass
 * its address to the callback (via tcp_arg or closure), start the lwIP operation,
 * then `co_await pico_wait(result)`.
 *
 * @tparam Args  Must match the `Args` of the paired `PicoCallbackResult`.
 */
template<typename... Args>
class PicoFuture {
public:
    using OutputType = std::tuple<Args...>;

    explicit PicoFuture(PicoCallbackResult<Args...>& result)
        : m_result(&result) {}

    PicoFuture(PicoFuture&& other) noexcept
        : m_result(std::exchange(other.m_result, nullptr)) {}
    PicoFuture& operator=(PicoFuture&& other) noexcept {
        m_result = std::exchange(other.m_result, nullptr);
        return *this;
    }
    PicoFuture(const PicoFuture&)            = delete;
    PicoFuture& operator=(const PicoFuture&) = delete;

    PollResult<OutputType> poll(detail::Context& ctx) {
        if (m_result->value.has_value())
            return std::move(*m_result->value);
        m_result->waker = ctx.getWaker();
        return PollPending;
    }

private:
    PicoCallbackResult<Args...>* m_result = nullptr;
};

/// Factory function — preferred spelling at call sites.
template<typename... Args>
PicoFuture<Args...> pico_wait(PicoCallbackResult<Args...>& result) {
    return PicoFuture<Args...>(result);
}

} // namespace coro
