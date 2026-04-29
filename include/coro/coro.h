#pragma once

#include <coro/detail/context.h>
#include <coro/future.h>
#include <coro/detail/future_awaitable.h>
#include <coro/detail/poll_result.h>
#include <coro/detail/coro_scope.h>
#include <coroutine>
#include <exception>
#include <functional>
#include <optional>
#include <utility>

namespace coro {

namespace detail {

/// @brief Shared promise base for `Coro<T>` and `Coro<void>`.
/// Handles suspend points, exception storage, context propagation, and `await_transform`.
struct CoroPromiseBase : public PollablePromise {
    std::suspend_always initial_suspend() noexcept { return {}; }
    std::suspend_always final_suspend() noexcept { return {}; }
    void unhandled_exception() noexcept { m_exception = std::current_exception(); }

    // Only Future<> types may be co_await-ed inside Coro or CoroStream.
    // Clears m_poll_current and m_cancel_current so stale hooks from previous co_awaits
    // are never mistakenly called. Passes both to FutureAwaitable so await_suspend() can
    // register the re-poll and (for Cancellable futures) cancel hooks.
    // Lvalue overload: future is moved into FutureAwaitable (handle is consumed).
    template<Future F>
    FutureAwaitable<F> await_transform(F& future) {
        return FutureAwaitable<F>(std::move(future), this);
    }

    // Rvalue overload: future is forwarded (moved) into FutureAwaitable.
    template<Future F>
    FutureAwaitable<F> await_transform(F&& future) {
        m_poll_current   = nullptr;
        m_cancel_current = nullptr;
        return FutureAwaitable<F>(std::move(future), this);
    }

    // Non-Future types are rejected at compile time.
    template<typename U> requires (!Future<std::remove_cvref_t<U>>)
    void await_transform(U&&) = delete;

    std::exception_ptr    m_exception;
};

template<typename T>
struct CoroPromiseResult : CoroPromiseBase
{
    template<std::convertible_to<T> U> requires (!std::is_void_v<T>)
    void return_value(U &&value) { m_value = std::forward<U>(value); }
    std::optional<T> m_value;
};

template<>
struct CoroPromiseResult<void> : CoroPromiseBase
{
    void return_void() { m_value = true; }
    bool m_value = false;
};

} // namespace detail


/**
 * @brief Coroutine return type for async functions that produce a value of type `T`.
 *
 * Satisfies @ref Future<T>. The coroutine starts suspended; the executor drives it
 * by repeatedly calling `poll()` until it returns a non-`Pending` result.
 *
 * Supports cooperative cancellation via `cancel()`. Once cancelled, `poll()`:
 * 1. If a Cancellable future is awaited: cancels it and polls until it returns non-Pending.
 * 2. Calls `handle.destroy()` to free the frame (LIFO dtors; JoinHandle dtors register
 *    children with the scope).
 * 3. Drains all pending scope children, then returns `PollDropped`.
 *
 * Non-Cancellable awaited futures are leaf futures (no child references) and are dropped
 * immediately as part of the LIFO teardown in step 2.
 *
 * @tparam T The value type produced on successful completion.
 */
template<typename T>
class Coro {
public:
    using OutputType = T;

    struct promise_type : detail::CoroPromiseResult<T> {
        Coro get_return_object() {
            return Coro(std::coroutine_handle<promise_type>::from_promise(*this));
        }
    };

    explicit Coro(std::coroutine_handle<promise_type> handle)
        : m_handle(handle) {}

    Coro(const Coro&)            = delete;
    Coro& operator=(const Coro&) = delete;

    Coro(Coro&& other) noexcept
        : m_handle(std::exchange(other.m_handle, {}))
        , m_scope(std::move(other.m_scope))
        , m_cancelled(other.m_cancelled)
        , m_cancel_draining(other.m_cancel_draining) {}

    Coro& operator=(Coro&& other) noexcept {
        if (this != &other) {
            if (m_handle) {
                detail::CurrentCoroGuard guard(&m_scope);
                m_handle.destroy();
            }
            m_handle          = std::exchange(other.m_handle, {});
            m_scope           = std::move(other.m_scope);
            m_cancelled       = other.m_cancelled;
            m_cancel_draining = other.m_cancel_draining;
        }
        return *this;
    }

    ~Coro() {
        if (!m_handle) return;
        // Set t_current_coro so any JoinHandle destructors in the frame register
        // their children with this scope before the frame memory is freed.
        detail::CurrentCoroGuard guard(&m_scope);
        m_handle.destroy();
    }

    /// @brief Marks this coroutine for cancellation. Takes effect on the next `poll()` call.
    void cancel() noexcept { m_cancelled = true; }

    /// @brief Advances the coroutine toward completion.
    /// @param ctx Carries the waker used to reschedule this task when it is ready to progress.
    /// @return `PollPending`, `PollReady(T)`, `PollError`, or `PollDropped` (if cancelled and drained).
    PollResult<T> poll(detail::Context& ctx) {
        struct Destroy
        {
            ~Destroy() { std::exchange(self->m_handle, nullptr).destroy(); }
            Coro *self;
        };

        if (m_cancelled) {
            if (m_handle && !m_handle.done()) {
                auto& promise = m_handle.promise();
                promise.m_ctx = &ctx;

                // Step 1: if a Cancellable future is awaited, cancel it and wait for
                // it to drain before destroying the frame. Non-Cancellable futures are
                // leaf futures — they are dropped immediately in handle.destroy() below.
                if (!m_cancel_draining && promise.m_cancel_current) {
                    promise.m_cancel_current->cancel();
                    promise.m_cancel_current = nullptr;
                    m_cancel_draining = true;
                }
                if (m_cancel_draining) {
                    if (promise.m_poll_current && !promise.m_poll_current->poll()) {
                        return PollPending;  // awaited Cancellable future still draining
                    }
                    promise.m_poll_current = nullptr;
                    m_cancel_draining = false;
                }

                // Step 2: destroy the frame. Runs LIFO dtors; JoinHandle dtors fire
                // and register children with our scope.
                {
                    detail::CurrentCoroGuard guard(&m_scope);
                    m_handle.destroy();
                    m_handle = {};
                }
            }

            // Step 3: drain all scope children (pre-existing + newly registered above).
            if (m_scope.set_drain_waker(ctx.get_weak_waker()))
                return PollPending;
            return PollDropped;
        }

        if (!m_handle)
            return PollPending;

        // Frame already ran to completion but children were still draining — re-check now.
        if (m_handle.done()) {
            if (m_scope.set_drain_waker(ctx.get_weak_waker()))
                return PollPending;
            Destroy cleanup(this);
            auto& p = m_handle.promise();
            if (p.m_exception)
                return PollError(p.m_exception);
            if constexpr (std::is_void_v<T>) {
                p.m_value = false;
                return PollReady;
            } else {
                return std::move(*p.m_value);
            }
        }

        auto& promise = m_handle.promise();
        promise.m_ctx = &ctx;

        // Spurious-wake guard: if the coroutine is suspended at a co_await, re-poll the
        // inner future with the fresh Context before resuming. If the inner future is
        // still Pending, the waker is re-registered and we return Pending without
        // disturbing the coroutine. Only resume when the future is confirmed ready.
        if (promise.m_poll_current) {
            if (!promise.m_poll_current->poll())
                return PollPending;
            promise.m_poll_current = nullptr;
        }

        {
            detail::CurrentCoroGuard guard(&m_scope);
            m_handle.resume();
        }

        if (m_handle.done()) {
            // Wait for any children spawned during this execution before completing.
            if (m_scope.set_drain_waker(ctx.get_weak_waker()))
                return PollPending;

            Destroy cleanup(this);

            if (promise.m_exception)
                return PollError(promise.m_exception);
            if constexpr (std::is_void_v<T>) {
                promise.m_value = false;
                return PollReady;
            } else {
                return std::move(*promise.m_value);
            }
        }

        // Coroutine suspended — check if pending children need the waker updated.
        // (Children drain in parallel with the coroutine's own suspension.)
        if (m_scope.has_pending())
            m_scope.set_drain_waker(ctx.get_weak_waker());

        return PollPending;
    }

private:
    std::coroutine_handle<promise_type> m_handle;
    detail::CoroutineScope              m_scope;
    bool                                m_cancelled       = false;
    // True while waiting for an awaited Cancellable future to drain during cancellation.
    // Persists across poll() calls so re-entries continue polling rather than re-cancelling.
    bool                                m_cancel_draining = false;
};

} // namespace coro
