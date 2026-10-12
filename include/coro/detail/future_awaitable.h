#pragma once

#include <coro/detail/context.h>
#include <coro/future.h>
#include <coro/detail/poll_result.h>
#include <coroutine>
#include <functional>
#include <type_traits>

namespace coro {

class PollableFuture;
struct PollablePromise
{
    detail::Context*      m_ctx         = nullptr;
    // Set by FutureAwaitable::await_suspend(); cleared by Coro::poll() after it returns
    // true (or at the start of the next await_transform). Non-null only while the
    // coroutine is suspended at a co_await expression.
    PollableFuture*       m_poll_current = nullptr;
    // Set by FutureAwaitable::await_suspend() only for Cancellable futures.
    // Called by Coro::poll() when cancelled to cooperatively cancel the awaited future
    // so it drains before the outer frame is torn down.
    PollableFuture*       m_cancel_current = nullptr;
};

class PollableFuture {
public:
    virtual bool poll() noexcept = 0;
    virtual void cancel() noexcept = 0;

protected:
    PollableFuture() = default;
    PollableFuture(PollableFuture &&pollable) = default;
    PollableFuture& operator=(PollableFuture &&) = delete;
    ~PollableFuture() = default;
};

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
class FutureAwaitable : private PollableFuture {
    using BaseF = std::remove_cvref_t<F>;
public:
    // cancel_hook is optional (null for non-Cancellable futures). When provided,
    // await_suspend() installs a cancel callback so the outer Coro/CoroStream can
    // cooperatively cancel the awaited future before destroying the frame.
    explicit FutureAwaitable(F future, PollablePromise *promise)
        : m_future(std::forward<F>(future))
        , m_promise(promise) {}

    // Poll once eagerly. If the future completes synchronously, skip suspension entirely.
    //
    // Each call takes one unit of the task's budget. With none left the future is not
    // polled at all: the task wakes itself and suspends, and poll() below makes the
    // first poll of the future on a later turn of the task, once it can take its unit
    // there. Not polling means nothing has been taken from a socket or channel that a
    // cancel could then lose.
    // See doc/design/task_budget.md.
    bool await_ready() noexcept{
        detail::Context& ctx = *m_promise->m_ctx;
        if (!ctx.try_consume_budget()) {
            ctx.yield_to_executor();
            m_deferred = true;
            return false;
        }
        try {
            m_result = m_future.poll(*m_promise->m_ctx);
        } catch (...) {
            m_result = PollError(std::current_exception());
        }
        return !m_result.isPending();
    }

    // Called only when await_ready() returned false.
    // Registers the re-poll hook with the promise so that the outer poll()
    // can verify the future is ready before resuming this coroutine.
    // Also registers the cancel hook if the awaited future satisfies Cancellable.
    void await_suspend(std::coroutine_handle<>) noexcept{
        m_promise->m_poll_current = this;
        if constexpr (Cancellable<BaseF>) {
            m_promise->m_cancel_current = this;
        }
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

    void cancel() noexcept override {
        if constexpr (Cancellable<BaseF>) {
            m_future.cancel();
        }
    }

    // The re-poll before resume.
    //
    // A future that has been polled before is polled again here whatever the budget,
    // and takes nothing from it: the coroutine is suspended on it, so skipping the
    // poll would leave it with no waker registered.
    //
    // A future that await_ready() deferred has never been polled and has nothing
    // registered, so its first poll is charged here as it would have been there, and
    // refused in the same way.
    //
    // NOTE: this charge is a failsafe, not what the budget rests on. A future that
    // polls several others is meant to stop when the budget is spent
    // (Context::yield_if_budget_spent(), as join() and select() do), and while every
    // such future does, a free first poll here would cost about one extra await per
    // task poll and no more. The charge is for one that does not: its branches would
    // each get an await for free on the next turn and start children that are
    // deferred in turn, so the free polls would multiply on every turn and the budget
    // would bound nothing. With the charge that is held to `budget` awaits per poll.
    bool poll() noexcept override {
        if (m_deferred) {
            detail::Context& ctx = *m_promise->m_ctx;
            if (!ctx.try_consume_budget()) {
                ctx.yield_to_executor();
                return false;
            }
            m_deferred = false;
        }
        try {
            m_result = m_future.poll(*m_promise->m_ctx);
        } catch (...) {
            m_result = PollError(std::current_exception());
        }
        return !m_result.isPending();
    }

private:
    F                                       m_future;   // reference for lvalue path, value for rvalue path
    PollablePromise*                        m_promise = nullptr;
    PollResult<typename BaseF::OutputType>  m_result{PollPending};
    // True from the await_ready() that found the budget spent until the first poll.
    // Only the failsafe in poll() reads it.
    bool                                    m_deferred = false;
};

} // namespace coro
