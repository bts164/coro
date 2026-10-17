#pragma once

#include <coro/detail/waker.h>
#include <coro/detail/rc.h>
#include <cstdint>
#include <memory>

namespace coro::detail {

/**
 * @brief Scheduling context passed to every `poll()` / `poll_next()` call.
 *
 * `Context` carries the per-poll `Waker` that a suspended future must store and
 * call when it becomes ready to make progress.
 *
 * Leaf futures (I/O, timers, channels) call `getWaker()`, store the result, and call
 * `wake()` from their completion callback to move the task back into the ready queue.
 *
 * It also carries the task's budget for this poll: how many `co_await`s the task may
 * still pass before it must yield to the executor. An executor sets it when it builds
 * the `Context`; a `Context` built anywhere else (`blocking_wait()`, a test) has no
 * limit. See doc/design/task_budget.md.
 *
 * Concrete executor implementations may subclass `Context` to carry additional
 * scheduler-specific state without changing the `poll()` signature.
 */
class Context {
public:
    /// The budget of a `Context` that never asks its task to yield.
    static constexpr std::uint32_t kUnlimitedBudget = UINT32_MAX;

    /// @brief A `Context` that holds a reference to `waker` for as long as it lives.
    /// `getWaker()` returns `waker` itself. For `blocking_wait()` and tests.
    explicit Context(Rc<Waker> waker, std::uint32_t budget = kUnlimitedBudget);

    /// @brief A `Context` that does not own `waker`: `getWaker()` and `get_weak_waker()`
    /// call `waker.clone()` and `waker.weak_clone()` only when a future asks for one.
    ///
    /// For executors. Most polls never ask, so this saves a reference count increment
    /// and decrement per poll. The caller must keep `waker` alive until the `Context`
    /// is destroyed; an executor does, since it holds the task for the whole poll.
    explicit Context(Waker& waker, std::uint32_t budget = kUnlimitedBudget);

    virtual ~Context();

    /// @brief Takes one unit of the task's budget. Returns false when none is left:
    /// the caller should then call yield_to_executor() and return pending without
    /// doing its work.
    ///
    /// `co_await` and `fiber_await()` call this for every future, so most futures
    /// never need to. A hand-written future that loops inside its own `poll()` can
    /// call it once per item to bound the loop.
    bool try_consume_budget() noexcept {
        if (m_budget == 0) return false;
        if (m_budget != kUnlimitedBudget) --m_budget;
        return true;
    }

    /// @brief For a future that polls several others (`join()`, `select()`): call
    /// before polling each branch, and do not poll the branch if it returns true.
    ///
    /// Returns false while budget is left, taking none of it: the branch pays for
    /// its own awaits. With none left it makes sure the task has yielded and returns
    /// true. The yield is the point: a branch that is skipped may have been the one
    /// this poll was woken for, so the task must be polled again. Returning pending
    /// without it would lose that wake.
    ///
    /// A future with one child has no need of this; the child makes its own check.
    bool yield_if_budget_spent() {
        if (m_budget != 0) return false;
        if (!m_yielded) yield_to_executor();
        return true;
    }

    /// @brief Wakes the task and marks this poll as yielded, so that the executor
    /// polls its other tasks and turns the I/O driver before this one runs again.
    /// The caller must then return pending.
    void yield_to_executor();

    /// @brief True once yield_to_executor() has been called in this poll. Read by
    /// the executor after the task's `poll()` returns.
    bool yielded() const noexcept { return m_yielded; }

    /// @brief The budget left in this poll. Read by an executor after `poll()`
    /// returns, to pass what is left on to a task it runs next in the same turn
    /// (the work-stealing executor's LIFO slot).
    std::uint32_t remaining_budget() const noexcept { return m_budget; }

    /// @brief Returns the waker for the current task. Store this and call `wake()` when ready.
    /// For leaf futures that store the waker for later notification, prefer get_weak_waker()
    /// to avoid contributing to ownership cycles. getWaker() is appropriate for temporary use
    /// (e.g., passing directly to a child future's poll() call or to Context construction).
    Rc<Waker> getWaker() const;

    /// @brief Returns a weak waker for persistent storage in futures that need to notify
    /// a task without anchoring its lifetime. Use this when storing a waker in a field
    /// (scope_waker, join_waker, etc.) rather than using it immediately. Firing is
    /// `if (auto w = stored_waker.lock()) w->wake();`.
    Weak<Waker>   get_weak_waker() const;

private:
    Rc<Waker>     m_owner;   ///< Empty for a non-owning `Context`.
    Waker*        m_waker;   ///< `m_owner.get()`, or the non-owned waker.
    // Both are touched only by the thread polling the task, during that poll.
    std::uint32_t m_budget;
    bool          m_yielded = false;
};

} // namespace coro::detail
