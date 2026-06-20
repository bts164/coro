#pragma once

// One-shot bridge between an lwIP callback and an awaiting coroutine.
//
// Analogous to UvCallbackResult / UvFuture in the libuv backend. Designed for
// lwIP NO_SYS mode where all callbacks fire synchronously on the executor thread
// during cyw43_arch_poll() / sys_check_timeouts(). No mutex is required.
//
// CAUTION: if using CYW43_ARCH_THREADSAFE_BACKGROUND, lwIP callbacks can arrive
// from an IRQ or second core. In that case protect waker and value with a
// critical_section_t rather than relying on single-threaded ordering.

#include <coro/detail/context.h>
#include <coro/detail/poll_result.h>
#include <coro/detail/rc.h>
#include <coro/future.h>
#include <memory>
#include <optional>
#include <tuple>

namespace coro {

// ---------------------------------------------------------------------------
// LwipCallbackResult<Args...>
// ---------------------------------------------------------------------------

/**
 * @brief Shared state between an lwIP callback and a @ref LwipFuture.
 *
 * Allocate on the coroutine frame (stack), store its address in a callback
 * closure via tcp_arg or a lambda capture, start the lwIP operation, then
 * `co_await lwip_wait(result)`. The callback calls `complete(args...)` to
 * store the result and wake the awaiting coroutine.
 *
 * The object must outlive the callback. `co_await lwip_wait(result)` keeps
 * the coroutine suspended (and therefore the frame alive) until `complete()`
 * fires, so a stack-allocated LwipCallbackResult is safe for a direct co_await.
 */
template<typename... Args>
struct LwipCallbackResult {
    detail::Rc<detail::Waker>          waker;
    std::optional<std::tuple<Args...>> value;

    void complete(Args... args) {
        value.emplace(std::forward<Args>(args)...);
        if (waker) {
            auto w = std::move(waker);
            w->wake();
        }
    }
};

// ---------------------------------------------------------------------------
// LwipFuture<Args...>
// ---------------------------------------------------------------------------

template<typename... Args>
class LwipFuture {
public:
    using OutputType = std::tuple<Args...>;

    explicit LwipFuture(LwipCallbackResult<Args...>& result)
        : m_result(&result) {}

    LwipFuture(LwipFuture&& other) noexcept
        : m_result(std::exchange(other.m_result, nullptr)) {}
    LwipFuture& operator=(LwipFuture&& other) noexcept {
        m_result = std::exchange(other.m_result, nullptr);
        return *this;
    }
    LwipFuture(const LwipFuture&)            = delete;
    LwipFuture& operator=(const LwipFuture&) = delete;

    PollResult<OutputType> poll(detail::Context& ctx) {
        if (m_result->value.has_value())
            return std::move(*m_result->value);
        m_result->waker = ctx.getWaker();
        return PollPending;
    }

private:
    LwipCallbackResult<Args...>* m_result = nullptr;
};

template<typename... Args>
LwipFuture<Args...> lwip_wait(LwipCallbackResult<Args...>& result) {
    return LwipFuture<Args...>(result);
}

} // namespace coro
