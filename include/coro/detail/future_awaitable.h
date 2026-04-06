#pragma once

#include <coro/detail/context.h>
#include <coro/future.h>
#include <coro/detail/poll_result.h>
#include <coroutine>
#include <functional>
#include <type_traits>

namespace coro {

// Adapts any Future into a C++20 awaitable for use inside Coro and CoroStream coroutines.
// Only reachable via await_transform in promise_type — not exposed as operator co_await.
//
// Stores Context** (pointer to promise.m_ctx) rather than Context* so that the re-poll
// in await_suspend's hook always reads the current Context.
//
// Spurious-wake correctness:
//   await_suspend() registers a re-poll lambda (m_poll_hook) with the promise.
//   Coro::poll() / CoroStream::poll_next() call this hook BEFORE resuming the coroutine:
//     - If the inner future is still Pending, the hook re-registers the waker and the
//       coroutine is NOT resumed — spurious wake is absorbed transparently.
//     - If the inner future is Ready/Error, the hook caches the result and returns true;
//       the caller then resumes the coroutine. await_resume() can therefore assume
//       m_result is never Pending.
// F may be a value type (rvalue path) or an lvalue reference type (lvalue path).
// BaseF strips cv-ref qualifiers for member type lookups and PollResult storage.
template<typename F>
class FutureAwaitable {
    using BaseF = std::remove_cvref_t<F>;
public:
    explicit FutureAwaitable(F future, detail::Context** ctx_ptr, std::function<bool()>* poll_hook)
        : m_future(std::forward<F>(future)), m_ctx_ptr(ctx_ptr), m_poll_hook(poll_hook) {}

    // Poll once eagerly. If the future completes synchronously, skip suspension entirely.
    bool await_ready() noexcept{
        try {
            m_result = m_future.poll(**m_ctx_ptr);
        } catch (...) {
            m_result = PollError(std::current_exception());
        }
        return !m_result.isPending();
    }

    // Called only when await_ready() returned false.
    // Registers the re-poll hook with the promise so that the outer poll()
    // can verify the future is ready before resuming this coroutine.
    void await_suspend(std::coroutine_handle<>) noexcept{
        *m_poll_hook = [this]() noexcept -> bool {
            try {
                m_result = m_future.poll(**m_ctx_ptr);
            } catch (...) {
                m_result = PollError(std::current_exception());
            }
            return !m_result.isPending();
        };
    }

    // Called only after the outer poll() confirmed the inner future is non-Pending:
    //   - await_ready() returned true: m_result is Ready or Error.
    //   - await_suspend() path: poll() called the hook, got true, cleared it, then resumed.
    // In either case m_result is never Pending here.
    typename BaseF::OutputType await_resume() {
        m_result.rethrowIfError();
        if constexpr (!std::is_void_v<typename BaseF::OutputType>)
            return std::move(m_result).value();
    }

private:
    F                                       m_future;   // reference for lvalue path, value for rvalue path
    detail::Context**                       m_ctx_ptr;
    std::function<bool()>*                  m_poll_hook;
    PollResult<typename BaseF::OutputType>  m_result{PollPending};
};

} // namespace coro
