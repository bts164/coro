#pragma once

#include <coro/detail/context.h>
#include <coro/future.h>
#include <coro/detail/future_awaitable.h>
#include <coro/detail/poll_result.h>
#include <coro/detail/coro_scope.h>
#include <coroutine>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <utility>

namespace coro {

namespace detail {

/// @brief Shared promise base for `Coro<T>` and `Coro<void>`.
/// Handles suspend points, exception storage, context propagation, and `await_transform`.
struct CoroPromiseBase {
    std::suspend_always initial_suspend() noexcept { return {}; }
    std::suspend_always final_suspend() noexcept { return {}; }
    void unhandled_exception() noexcept { m_exception = std::current_exception(); }

    // Only Future<> types may be co_await-ed inside Coro or CoroStream.
    // Clears m_poll_current so a stale hook from a previous co_await is never mistakenly
    // called. Passes &m_poll_current to FutureAwaitable so await_suspend() can register
    // the re-poll hook for spurious-wake protection.
    // Lvalue overload: future is moved into FutureAwaitable (handle is consumed).
    template<Future F>
    FutureAwaitable<F> await_transform(F& future) {
        m_poll_current = nullptr;
        return FutureAwaitable<F>(std::move(future), &m_ctx, &m_poll_current);
    }

    // Rvalue overload: future is forwarded (moved) into FutureAwaitable.
    template<Future F>
    FutureAwaitable<F> await_transform(F&& future) {
        m_poll_current = nullptr;
        return FutureAwaitable<F>(std::move(future), &m_ctx, &m_poll_current);
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


/**
 * @brief Coroutine return type for async functions that produce a value of type `T`.
 *
 * Satisfies @ref Future<T>. The coroutine starts suspended; the executor drives it
 * by repeatedly calling `poll()` until it returns a non-`Pending` result.
 *
 * Supports cooperative cancellation via `cancel()`. Once cancelled, `poll()` destroys
 * the coroutine frame (firing `JoinHandle` destructors in LIFO order), drains any
 * spawned children tracked by the implicit @ref CoroutineScope, then returns `PollDropped`.
 *
 * @tparam T The value type produced on successful completion.
 */
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

    explicit Coro(std::coroutine_handle<promise_type> handle)
        : m_handle(handle)
        , m_scope(std::make_unique<detail::CoroutineScope>()) {}

    Coro(const Coro&)            = delete;
    Coro& operator=(const Coro&) = delete;

    Coro(Coro&& other) noexcept
        : m_handle(std::exchange(other.m_handle, {}))
        , m_scope(std::move(other.m_scope))
        , m_cancelled(other.m_cancelled) {}

    Coro& operator=(Coro&& other) noexcept {
        if (this != &other) {
            if (m_handle) {
                detail::CurrentCoroGuard guard(m_scope.get());
                m_handle.destroy();
            }
            m_handle    = std::exchange(other.m_handle, {});
            m_scope     = std::move(other.m_scope);
            m_cancelled = other.m_cancelled;
        }
        return *this;
    }

    ~Coro() {
        if (m_handle) {
            // Set t_current_coro so any JoinHandle destructors in the frame register
            // their children with this scope before the frame memory is freed.
            detail::CurrentCoroGuard guard(m_scope.get());
            m_handle.destroy();
        }
    }

    /// @brief Marks this coroutine for cancellation. Takes effect on the next `poll()` call.
    void cancel() noexcept { m_cancelled = true; }

    /// @brief Advances the coroutine toward completion.
    /// @param ctx Carries the waker used to reschedule this task when it is ready to progress.
    /// @return `PollPending`, `PollReady(T)`, `PollError`, or `PollDropped` (if cancelled and drained).
    PollResult<T> poll(detail::Context& ctx) {
        // Cancelled path: destroy the frame (fires JoinHandle dtors → registers children),
        // then drain pending children before returning PollDropped.
        if (m_cancelled) {
            if (m_handle && !m_handle.done()) {
                detail::CurrentCoroGuard guard(m_scope.get());
                m_handle.destroy();
                m_handle = {};
            }
            if (m_scope->set_drain_waker(ctx.getWaker()->clone()))
                return PollPending;
            return PollDropped;
        }

        if (!m_handle)
            return PollPending;

        // Frame already ran to completion but children were still draining — re-check now.
        if (m_handle.done()) {
            if (m_scope->set_drain_waker(ctx.getWaker()->clone()))
                return PollPending;
            auto& p = m_handle.promise();
            if (p.m_exception)
                return PollError(p.m_exception);
            return std::move(*p.m_value);
        }

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

        {
            detail::CurrentCoroGuard guard(m_scope.get());
            m_handle.resume();
        }

        if (m_handle.done()) {
            // Wait for any children spawned during this execution before completing.
            if (m_scope->set_drain_waker(ctx.getWaker()->clone()))
                return PollPending;
            if (promise.m_exception)
                return PollError(promise.m_exception);
            return std::move(*promise.m_value);
        }

        // Coroutine suspended — check if pending children need the waker updated.
        // (Children drain in parallel with the coroutine's own suspension.)
        if (m_scope->has_pending())
            m_scope->set_drain_waker(ctx.getWaker()->clone());

        return PollPending;
    }

private:
    std::coroutine_handle<promise_type>      m_handle;
    std::unique_ptr<detail::CoroutineScope>  m_scope;
    bool                                     m_cancelled = false;
};


/**
 * @brief Coroutine return type for async functions that produce no value.
 *
 * Specialization of @ref Coro for `void`. Satisfies `Future<void>`.
 * Cancellation and implicit scope drain behave identically to the primary template.
 */
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

    explicit Coro(std::coroutine_handle<promise_type> handle)
        : m_handle(handle)
        , m_scope(std::make_unique<detail::CoroutineScope>()) {}

    Coro(const Coro&)            = delete;
    Coro& operator=(const Coro&) = delete;

    Coro(Coro&& other) noexcept
        : m_handle(std::exchange(other.m_handle, {}))
        , m_scope(std::move(other.m_scope))
        , m_cancelled(other.m_cancelled) {}

    Coro& operator=(Coro&& other) noexcept {
        if (this != &other) {
            if (m_handle) {
                detail::CurrentCoroGuard guard(m_scope.get());
                m_handle.destroy();
            }
            m_handle    = std::exchange(other.m_handle, {});
            m_scope     = std::move(other.m_scope);
            m_cancelled = other.m_cancelled;
        }
        return *this;
    }

    ~Coro() {
        if (m_handle) {
            // Set t_current_coro so any JoinHandle destructors in the frame register
            // their children with this scope before the frame memory is freed.
            detail::CurrentCoroGuard guard(m_scope.get());
            m_handle.destroy();
        }
    }

    /// @brief Marks this coroutine for cancellation. Takes effect on the next `poll()` call.
    void cancel() noexcept { m_cancelled = true; }

    /// @brief Advances the coroutine toward completion.
    /// @param ctx Carries the waker used to reschedule this task when it is ready to progress.
    /// @return `PollPending`, `PollReady`, `PollError`, or `PollDropped` (if cancelled and drained).
    PollResult<void> poll(detail::Context& ctx) {
        if (m_cancelled) {
            if (m_handle && !m_handle.done()) {
                detail::CurrentCoroGuard guard(m_scope.get());
                m_handle.destroy();
                m_handle = {};
            }
            if (m_scope->set_drain_waker(ctx.getWaker()->clone()))
                return PollPending;
            return PollDropped;
        }

        if (!m_handle)
            return PollPending;

        // Frame already ran to completion but children were still draining — re-check now.
        if (m_handle.done()) {
            if (m_scope->set_drain_waker(ctx.getWaker()->clone()))
                return PollPending;
            auto& p = m_handle.promise();
            if (p.m_exception)
                return PollError(p.m_exception);
            return PollReady;
        }

        auto& promise = m_handle.promise();
        promise.m_ctx = &ctx;

        if (promise.m_poll_current) {
            if (!promise.m_poll_current())
                return PollPending;
            promise.m_poll_current = nullptr;
        }

        {
            detail::CurrentCoroGuard guard(m_scope.get());
            m_handle.resume();
        }

        if (m_handle.done()) {
            if (m_scope->set_drain_waker(ctx.getWaker()->clone()))
                return PollPending;
            if (promise.m_exception)
                return PollError(promise.m_exception);
            return PollReady;
        }

        if (m_scope->has_pending())
            m_scope->set_drain_waker(ctx.getWaker()->clone());

        return PollPending;
    }

private:
    std::coroutine_handle<promise_type>      m_handle;
    std::unique_ptr<detail::CoroutineScope>  m_scope;
    bool                                     m_cancelled = false;
};

} // namespace coro
