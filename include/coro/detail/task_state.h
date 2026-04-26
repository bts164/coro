#pragma once

// Internal — not part of the public API.
// Shared ref-counted state between an executor-held Task and its JoinHandle.

#include <atomic>
#include <condition_variable>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>

#include <coro/detail/waker.h>

namespace coro::detail {

/**
 * @brief Atomic scheduling lifecycle state for a Task.
 *
 * The explicit atomic field (rather than implicitly tracking state via which
 * data structure holds the task) allows lock-free CAS transitions in TaskWaker
 * and the executor worker loops.
 *
 * Transitions:
 *   schedule()           : Idle → Notified  (set explicitly before first enqueue)
 *   worker dequeues      : Notified → Running  (CAS; failure = bug)
 *   poll() Pending + no concurrent wake : Running → Idle  (CAS; waker owns shared_ptr)
 *   poll() Pending + concurrent wake    : Running → RunningAndNotified (CAS in wake())
 *   post-poll RunningAndNotified detected: RunningAndNotified → Notified (CAS; re-enqueue)
 *   wake() while Idle    : Idle → Notified  (CAS in TaskWaker::wake(); calls enqueue())
 *   wake() while Running : Running → RunningAndNotified  (CAS; worker re-enqueues)
 *   poll() returns Ready : Running → Done  (store; terminal)
 */
enum class SchedulingState : uint8_t {
    Idle               = 0,  ///< Suspended; waker holds the only shared_ptr<Task> ref
    Running            = 1,  ///< Inside poll(); owned by the worker
    Notified           = 2,  ///< In a ready queue; waiting to be polled
    RunningAndNotified = 3,  ///< Inside poll() AND wake() fired; worker re-enqueues after poll
    Done               = 4,  ///< poll() returned Ready; terminal — task will be destroyed
};

/**
 * @brief Non-template base for @ref TaskState.
 *
 * Holds the fields required for `wait_for_completion()` to block the calling
 * thread until the task reaches a terminal state. Separated from the template
 * so the executor interface can accept either specialisation without templates.
 *
 * `mutex`, `cv`, and `terminated` are always accessed together: `terminated`
 * is set inside `mutex`, and `cv.notify_all()` is called in the same critical
 * section, so `wait_until_done()` can never miss the wakeup.
 */
struct TaskStateBase {
    mutable std::mutex      mutex;
    std::condition_variable cv;
    bool                    terminated{false};

    // RACE CONDITION NOTE: this is safe because every code path that sets
    // `terminated = true` also calls `cv.notify_all()` *in the same critical
    // section* (under `mutex`). This guarantees no lost wakeup: the waiter
    // holds `mutex` while evaluating the predicate, so either it sees
    // `terminated == true` before sleeping, or the notifier hasn't acquired
    // the lock yet and will call notify_all() after the waiter enters wait().
    //
    // When implementing a new executor's wait_for_completion(), use this
    // function rather than rolling your own condvar loop. The key invariant to
    // preserve in all terminal methods (setResult, setDone, setException,
    // mark_done) is: set `terminated = true` AND call `cv.notify_all()` while
    // holding `mutex`. Breaking this invariant reintroduces the lost wakeup.
    void wait_until_done() {
        std::unique_lock lock(mutex);
        cv.wait(lock, [this]{ return terminated; });
    }
};


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
struct TaskState : TaskStateBase {
    using ResultType = std::conditional_t<std::is_void_v<T>, bool, std::optional<T>>;
    static constexpr ResultType init_result() {
        if constexpr (std::is_void_v<T>) {
            return false;
        } else {
            return std::nullopt;
        }
    }    
    std::atomic<bool>      cancelled{false};        ///< Set by `JoinHandle` destructor; checked by `Task::poll()`.
    std::shared_ptr<Waker> join_waker;              ///< Wakes the `JoinHandle` awaiter on completion.
    std::shared_ptr<Waker> scope_waker;             ///< Wakes the parent `CoroutineScope` on completion.
    ResultType             result = init_result();  ///< Set by `setResult()` on successful completion.
    std::exception_ptr     exception;               ///< Set by `setException()` on fault.

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
            cv.notify_all();
            join_wk   = std::move(join_waker);
            scope_wk  = std::move(scope_waker);
        }
        if (join_wk)  join_wk->wake();
        if (scope_wk) scope_wk->wake();
    }

    /// @brief Stores the successful result and signals completion. Fires both wakers.
    void setResult() requires std::is_void_v<T> {
        std::shared_ptr<Waker> join_wk, scope_wk;
        {
            std::lock_guard lock(mutex);
            result = true;
            terminated = true;
            cv.notify_all();
            join_wk   = std::move(join_waker);
            scope_wk  = std::move(scope_waker);
        }
        if (join_wk)  join_wk->wake();
        if (scope_wk) scope_wk->wake();
    }

    /// @brief Stores the successful result and signals completion. Fires both wakers.
    template<std::convertible_to<T> U> requires (!std::is_void_v<T>)
    void setResult(U &&value) {
        std::shared_ptr<Waker> join_wk, scope_wk;
        {
            std::lock_guard lock(mutex);
            result = std::forward<U>(value);
            terminated = true;
            cv.notify_all();
            join_wk   = std::move(join_waker);
            scope_wk  = std::move(scope_waker);
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
            cv.notify_all();
            join_wk   = std::move(join_waker);
            scope_wk  = std::move(scope_waker);
        }
        if (join_wk)  join_wk->wake();
        if (scope_wk) scope_wk->wake();
    }
};

} // namespace coro::detail
