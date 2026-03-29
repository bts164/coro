#pragma once

#include <coro/context.h>
#include <coro/future_awaitable.h>
#include <coro/poll_result.h>
#include <coro/stream.h>
#include <coroutine>
#include <exception>
#include <functional>
#include <optional>
#include <utility>

namespace coro {

namespace detail {

// Mixin that provides return_void() when HasFinalValue = false.
template<typename T, bool HasFinalValue>
struct CoroStreamReturn {
    void return_void() noexcept {}
};

// Mixin that provides return_value(T) when HasFinalValue = true.
// The final value is stored in m_final_value and emitted as the last stream item.
template<typename T>
struct CoroStreamReturn<T, true> {
    void return_value(T value) { m_final_value = std::move(value); }
    std::optional<T> m_final_value;
};

} // namespace detail


// CoroStream<T, HasFinalValue> — coroutine return type for async generators.
// Satisfies Stream<T>. Use co_yield to emit items; co_await Futures internally.
//
// When HasFinalValue = false (default): co_return (void) signals exhaustion.
// When HasFinalValue = true:            co_return value emits one final item of
//                                       type T before exhaustion.
template<typename T, bool HasFinalValue = false>
class CoroStream {
public:
    using ItemType = T;

    struct promise_type : detail::CoroStreamReturn<T, HasFinalValue> {
        CoroStream get_return_object() {
            return CoroStream(std::coroutine_handle<promise_type>::from_promise(*this));
        }

        // Start suspended — generator does not run until first polled.
        std::suspend_always initial_suspend() noexcept { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }

        void unhandled_exception() noexcept { m_exception = std::current_exception(); }

        // Stores the yielded value and suspends.
        std::suspend_always yield_value(T value) {
            m_yielded = std::move(value);
            return {};
        }

        // Only Future<> types may be co_await-ed inside CoroStream.
        // Clears m_poll_current and passes &m_poll_current for spurious-wake protection
        // (same mechanism as CoroPromiseBase — see coro.h for full explanation).
        template<Future F>
        FutureAwaitable<std::remove_cvref_t<F>> await_transform(F&& future) {
            m_poll_current = nullptr;
            return FutureAwaitable<std::remove_cvref_t<F>>(
                std::forward<F>(future), &m_ctx, &m_poll_current);
        }

        template<typename U> requires (!Future<std::remove_cvref_t<U>>)
        void await_transform(U&&) = delete;

        Context*              m_ctx         = nullptr;
        std::optional<T>      m_yielded;
        std::exception_ptr    m_exception;
        // Non-null only while coroutine is suspended at a co_await (not at co_yield).
        std::function<bool()> m_poll_current;
    };

    explicit CoroStream(std::coroutine_handle<promise_type> handle) : m_handle(handle) {}

    CoroStream(const CoroStream&)            = delete;
    CoroStream& operator=(const CoroStream&) = delete;

    CoroStream(CoroStream&& other) noexcept : m_handle(std::exchange(other.m_handle, {})) {}

    CoroStream& operator=(CoroStream&& other) noexcept {
        if (this != &other) {
            if (m_handle) m_handle.destroy();
            m_handle = std::exchange(other.m_handle, {});
        }
        return *this;
    }

    ~CoroStream() {
        if (m_handle) m_handle.destroy();
    }

    PollResult<std::optional<T>> poll_next(Context& ctx) {
        if (!m_handle)
            return std::optional<T>(std::nullopt);

        // Already done from a previous poll — emit unemitted final value or signal exhaustion.
        if (m_handle.done()) {
            if constexpr (HasFinalValue) {
                auto& p = m_handle.promise();
                if (p.m_final_value.has_value()) {
                    auto val = std::move(*p.m_final_value);
                    p.m_final_value.reset();
                    return std::optional<T>(std::move(val));
                }
            }
            return std::optional<T>(std::nullopt);
        }

        auto& promise = m_handle.promise();
        promise.m_ctx = &ctx;

        // Spurious-wake guard: if suspended at a co_await, re-poll the inner future
        // before resuming. m_poll_current is null when suspended at co_yield, so
        // yield-point resumes are unaffected.
        if (promise.m_poll_current) {
            if (!promise.m_poll_current())
                return PollPending;
            promise.m_poll_current = nullptr;
        }

        m_handle.resume();

        if (promise.m_exception)
            return PollError(promise.m_exception);

        if (promise.m_yielded.has_value()) {
            auto val = std::move(*promise.m_yielded);
            promise.m_yielded.reset();
            return std::optional<T>(std::move(val));
        }

        if (m_handle.done()) {
            if constexpr (HasFinalValue) {
                if (promise.m_final_value.has_value()) {
                    auto val = std::move(*promise.m_final_value);
                    promise.m_final_value.reset();
                    return std::optional<T>(std::move(val));
                }
            }
            return std::optional<T>(std::nullopt);
        }

        return PollPending;
    }

private:
    std::coroutine_handle<promise_type> m_handle;
};

} // namespace coro
