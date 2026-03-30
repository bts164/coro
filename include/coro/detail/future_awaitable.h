#pragma once

#include <coro/detail/context.h>
#include <coro/future.h>
#include <coro/detail/poll_result.h>
#include <coroutine>
#include <functional>
#include <type_traits>

namespace coro {

/**
 * @brief C++20 awaitable adapter that bridges any @ref Future into a coroutine `co_await` expression.
 *
 * Only reachable via `await_transform` in `CoroPromiseBase` — not exposed as a standalone
 * `operator co_await`. Users never construct this directly.
 *
 * ### Spurious-wake correctness
 *
 * A waker may fire even when the inner future is still Pending (spurious wake). To guard
 * against this, `await_suspend()` registers a *re-poll hook* (`m_poll_hook`) with the
 * promise. Before the outer `Coro::poll()` resumes the coroutine it calls this hook:
 *
 * - If the inner future returns Pending again, the hook re-registers the waker and the
 *   coroutine is **not** resumed — the spurious wake is absorbed transparently.
 * - If the inner future returns Ready or Error, the hook caches the result, returns `true`,
 *   and the outer `poll()` resumes the coroutine. `await_resume()` can therefore safely
 *   call `value()` without checking for Pending.
 *
 * ### Context pointer
 *
 * Stores `Context**` (pointer-to-pointer) rather than `Context*` so that the hook lambda
 * always reads the *current* `Context` even if the outer `poll()` was called with a
 * different context object on a subsequent scheduling tick.
 *
 * @tparam F A type satisfying @ref Future.
 */
template<Future F>
class FutureAwaitable {
public:
    explicit FutureAwaitable(F future, detail::Context** ctx_ptr, std::function<bool()>* poll_hook)
        : m_future(std::move(future)), m_ctx_ptr(ctx_ptr), m_poll_hook(poll_hook) {}

    /**
     * @brief Polls the inner future once eagerly before suspending.
     * @return `true` if the future completed synchronously (skips `await_suspend`).
     */
    bool await_ready() {
        m_result = m_future.poll(**m_ctx_ptr);
        return !m_result.isPending();
    }

    /**
     * @brief Called only when `await_ready()` returned `false`.
     * Registers the re-poll hook with the promise so the outer `poll()` can verify
     * the future is ready before resuming this coroutine.
     */
    void await_suspend(std::coroutine_handle<>) {
        *m_poll_hook = [this]() -> bool {
            m_result = m_future.poll(**m_ctx_ptr);
            return !m_result.isPending();
        };
    }

    /**
     * @brief Returns the future's result to the `co_await` expression.
     *
     * Guaranteed to be called only when `m_result` is Ready or Error (never Pending).
     * Rethrows stored exceptions; moves the value out for non-void futures.
     */
    typename F::OutputType await_resume() {
        m_result.rethrowIfError();
        if constexpr (!std::is_void_v<typename F::OutputType>)
            return std::move(m_result).value();
    }

private:
    F                                   m_future;
    detail::Context**                   m_ctx_ptr;   ///< Pointer to promise.m_ctx — always current.
    std::function<bool()>*              m_poll_hook; ///< Written to promise.m_poll_current on suspend.
    PollResult<typename F::OutputType>  m_result{PollPending};
};

} // namespace coro
