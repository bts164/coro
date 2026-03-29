#pragma once

#include <coro/context.h>
#include <coro/future.h>
#include <coro/future_awaitable.h>
#include <coro/poll_result.h>
#include <coroutine>
#include <exception>
#include <functional>
#include <optional>
#include <utility>

namespace coro {

namespace detail {

// Shared base for Coro<T> and Coro<void> promise_types.
// Handles suspend points, exception storage, context propagation, and await_transform.
struct CoroPromiseBase {
    std::suspend_always initial_suspend() noexcept { return {}; }
    std::suspend_always final_suspend() noexcept { return {}; }
    void unhandled_exception() noexcept { m_exception = std::current_exception(); }

    // Only types satisfying Future<> may be co_await-ed inside Coro or CoroStream.
    // Clears m_poll_current so a stale hook from a previous co_await is never mistakenly
    // called. Passes &m_poll_current to FutureAwaitable so await_suspend() can register
    // the re-poll hook for spurious-wake protection.
    template<Future F>
    FutureAwaitable<std::remove_cvref_t<F>> await_transform(F&& future) {
        m_poll_current = nullptr;
        return FutureAwaitable<std::remove_cvref_t<F>>(
            std::forward<F>(future), &m_ctx, &m_poll_current);
    }

    // Non-Future types are rejected at compile time.
    template<typename U> requires (!Future<std::remove_cvref_t<U>>)
    void await_transform(U&&) = delete;

    Context*              m_ctx         = nullptr;
    std::exception_ptr    m_exception;
    // Set by FutureAwaitable::await_suspend(); cleared by Coro::poll() after it returns
    // true (or at the start of the next await_transform). Non-null only while the
    // coroutine is suspended at a co_await expression.
    std::function<bool()> m_poll_current;
};

} // namespace detail


// Coro<T> — coroutine return type for async functions that produce a value.
// Satisfies Future<T>. The coroutine starts suspended; the executor drives it via poll().
template<typename T>
class Coro {
public:
    using OutputType = T;

    struct promise_type : detail::CoroPromiseBase {
        Coro get_return_object() {
            return Coro(std::coroutine_handle<promise_type>::from_promise(*this));
        }
        void return_value(T value) { m_value = std::move(value); }

        std::optional<T> m_value;
    };

    explicit Coro(std::coroutine_handle<promise_type> handle) : m_handle(handle) {}

    Coro(const Coro&)            = delete;
    Coro& operator=(const Coro&) = delete;

    Coro(Coro&& other) noexcept : m_handle(std::exchange(other.m_handle, {})) {}

    Coro& operator=(Coro&& other) noexcept {
        if (this != &other) {
            if (m_handle) m_handle.destroy();
            m_handle = std::exchange(other.m_handle, {});
        }
        return *this;
    }

    ~Coro() {
        if (m_handle) m_handle.destroy();
    }

    PollResult<T> poll(Context& ctx) {
        if (!m_handle || m_handle.done())
            return PollPending;

        auto& promise = m_handle.promise();
        promise.m_ctx = &ctx;

        // Spurious-wake guard: if the coroutine is suspended at a co_await, re-poll the
        // inner future with the fresh Context before resuming. If the inner future is
        // still Pending, the waker is re-registered and we return Pending without
        // disturbing the coroutine. Only resume when the future is confirmed ready.
        if (promise.m_poll_current) {
            if (!promise.m_poll_current())
                return PollPending;
            promise.m_poll_current = nullptr;
        }

        m_handle.resume();

        if (m_handle.done()) {
            if (promise.m_exception)
                return PollError(promise.m_exception);
            return std::move(*promise.m_value);
        }
        return PollPending;
    }

private:
    std::coroutine_handle<promise_type> m_handle;
};


// Coro<void> — coroutine return type for async functions that produce no value.
// Satisfies Future<void>.
template<>
class Coro<void> {
public:
    using OutputType = void;

    struct promise_type : detail::CoroPromiseBase {
        Coro get_return_object() {
            return Coro(std::coroutine_handle<promise_type>::from_promise(*this));
        }
        void return_void() noexcept {}
    };

    explicit Coro(std::coroutine_handle<promise_type> handle) : m_handle(handle) {}

    Coro(const Coro&)            = delete;
    Coro& operator=(const Coro&) = delete;

    Coro(Coro&& other) noexcept : m_handle(std::exchange(other.m_handle, {})) {}

    Coro& operator=(Coro&& other) noexcept {
        if (this != &other) {
            if (m_handle) m_handle.destroy();
            m_handle = std::exchange(other.m_handle, {});
        }
        return *this;
    }

    ~Coro() {
        if (m_handle) m_handle.destroy();
    }

    PollResult<void> poll(Context& ctx) {
        if (!m_handle || m_handle.done())
            return PollPending;

        auto& promise = m_handle.promise();
        promise.m_ctx = &ctx;

        if (promise.m_poll_current) {
            if (!promise.m_poll_current())
                return PollPending;
            promise.m_poll_current = nullptr;
        }

        m_handle.resume();

        if (m_handle.done()) {
            if (promise.m_exception)
                return PollError(promise.m_exception);
            return PollReady;
        }
        return PollPending;
    }

private:
    std::coroutine_handle<promise_type> m_handle;
};

} // namespace coro
