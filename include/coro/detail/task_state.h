#pragma once

// Internal — not part of the public API.
// Shared ref-counted state between an executor-held Task and its JoinHandle.

#include <atomic>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>

#include <coro/detail/waker.h>

namespace coro::detail {

/**
 * @brief Shared state between a @ref Task held by the executor and its @ref JoinHandle.
 *
 * Both ends hold a `shared_ptr<TaskState<T>>`. The executor writes the result (or exception)
 * when the task completes; the `JoinHandle` reads it and wakes the waiting coroutine.
 *
 * Two wakers are stored:
 * - `join_waker` — set by `JoinHandle::poll()`; fired when the awaiting coroutine should
 *   be rescheduled to retrieve the result.
 * - `scope_waker` — set by `CoroutineScope::set_drain_waker()`; fired so the parent
 *   coroutine knows a dropped child has finished draining.
 *
 * All mutable fields (except `cancelled`) are protected by `mutex`.
 * `cancelled` is an atomic so `Task::poll()` can check it without holding the lock.
 *
 * @tparam T The value type produced by the task.
 */
template<typename T>
struct TaskState {
    mutable std::mutex     mutex;
    std::atomic<bool>      cancelled{false};    ///< Set by `JoinHandle` destructor; checked by `Task::poll()`.
    std::shared_ptr<Waker> join_waker;          ///< Wakes the `JoinHandle` awaiter on completion.
    std::shared_ptr<Waker> scope_waker;         ///< Wakes the parent `CoroutineScope` on completion.
    std::optional<T>       result;              ///< Set by `setResult()` on successful completion.
    std::exception_ptr     exception;           ///< Set by `setException()` on fault.
    bool                   terminated{false};   ///< True once the task has reached any terminal state.

    /// @brief Returns `true` if the task has reached a terminal state (success, error, or cancelled).
    bool is_complete() const {
        std::lock_guard lock(mutex);
        return terminated;
    }

    /// @brief Signals terminal state without a result. Used by `Task` on cancellation.
    /// Fires both `join_waker` and `scope_waker`.
    void mark_done() {
        std::shared_ptr<Waker> join_wk, scope_wk;
        {
            std::lock_guard lock(mutex);
            terminated = true;
            join_wk = std::move(join_waker);
            scope_wk = std::move(scope_waker);
        }
        if (join_wk)  join_wk->wake();
        if (scope_wk) scope_wk->wake();
    }

    /// @brief Stores the successful result and signals completion. Fires both wakers.
    void setResult(T value) {
        std::shared_ptr<Waker> join_wk, scope_wk;
        {
            std::lock_guard lock(mutex);
            result = std::move(value);
            terminated = true;
            join_wk = std::move(join_waker);
            scope_wk = std::move(scope_waker);
        }
        if (join_wk)  join_wk->wake();
        if (scope_wk) scope_wk->wake();
    }

    /// @brief Stores an unhandled exception and signals completion. Fires both wakers.
    void setException(std::exception_ptr e) {
        std::shared_ptr<Waker> join_wk, scope_wk;
        {
            std::lock_guard lock(mutex);
            exception = std::move(e);
            terminated = true;
            join_wk = std::move(join_waker);
            scope_wk = std::move(scope_waker);
        }
        if (join_wk)  join_wk->wake();
        if (scope_wk) scope_wk->wake();
    }
};

/**
 * @brief `TaskState` specialization for tasks with no return value (`void`).
 *
 * Identical to the primary template except `result` is replaced by a `done` flag,
 * and `setDone()` replaces `setResult()`.
 */
template<>
struct TaskState<void> {
    mutable std::mutex     mutex;
    std::atomic<bool>      cancelled{false};
    std::shared_ptr<Waker> join_waker;
    std::shared_ptr<Waker> scope_waker;
    bool                   done{false};         ///< Set by `setDone()` on successful completion.
    std::exception_ptr     exception;
    bool                   terminated{false};

    /// @brief Returns `true` if the task has reached a terminal state.
    bool is_complete() const {
        std::lock_guard lock(mutex);
        return terminated;
    }

    /// @brief Signals terminal state without a result. Used by `Task` on cancellation.
    void mark_done() {
        std::shared_ptr<Waker> join_wk, scope_wk;
        {
            std::lock_guard lock(mutex);
            terminated = true;
            join_wk = std::move(join_waker);
            scope_wk = std::move(scope_waker);
        }
        if (join_wk)  join_wk->wake();
        if (scope_wk) scope_wk->wake();
    }

    /// @brief Signals successful completion. Fires both wakers.
    void setDone() {
        std::shared_ptr<Waker> join_wk, scope_wk;
        {
            std::lock_guard lock(mutex);
            done = true;
            terminated = true;
            join_wk = std::move(join_waker);
            scope_wk = std::move(scope_waker);
        }
        if (join_wk)  join_wk->wake();
        if (scope_wk) scope_wk->wake();
    }

    /// @brief Stores an unhandled exception and signals completion. Fires both wakers.
    void setException(std::exception_ptr e) {
        std::shared_ptr<Waker> join_wk, scope_wk;
        {
            std::lock_guard lock(mutex);
            exception = std::move(e);
            terminated = true;
            join_wk = std::move(join_waker);
            scope_wk = std::move(scope_waker);
        }
        if (join_wk)  join_wk->wake();
        if (scope_wk) scope_wk->wake();
    }
};

} // namespace coro::detail
