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
 * data structure holds the task) allows lock-free CAS transitions in
 * TaskBase::wake() and the executor worker loops.
 *
 * Transitions:
 *   schedule()           : Idle → Notified  (set explicitly before first enqueue)
 *   worker dequeues      : Notified → Running  (CAS; failure = bug)
 *   poll() Pending + no concurrent wake : Running → Idle  (CAS; OwnedTask in parent holds the only ref)
 *   poll() Pending + concurrent wake    : Running → RunningAndNotified (CAS in wake())
 *   post-poll RunningAndNotified detected: RunningAndNotified → Notified (CAS; re-enqueue)
 *   wake() while Idle    : Idle → Notified  (CAS in TaskBase::wake(); calls enqueue())
 *   wake() while Running : Running → RunningAndNotified  (CAS; worker re-enqueues)
 *   poll() returns Ready : Running → Done  (store; terminal)
 */
enum class SchedulingState : uint8_t {
    Idle               = 0,  ///< Suspended; OwnedTask in parent is the sole persistent strong ref
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

    // Self-reference for detached tasks. Set by JoinHandle::detach() before releasing
    // OwnedTask, cleared by all terminal methods (mark_done, setResult, setException)
    // under the same critical section that sets terminated=true. This ensures a detached
    // task anchors its own lifetime until it reaches a terminal state.
    //
    // Stored as shared_ptr<void> to avoid a circular type dependency between TaskStateBase
    // and OwnedTask/TaskBase. The underlying allocation is a TaskImpl<F>; the shared_ptr
    // carries the correct deleter regardless of the void type.
    std::shared_ptr<void>   self_owned;

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
 * A single `waker` slot covers both awaiter cases:
 * - Set by `JoinHandle::poll()` when the handle is being `co_await`-ed.
 * - Set by `CoroutineScope::set_drain_waker()` when the handle was dropped and the scope
 *   is waiting for the child to drain.
 *
 * These two cases are mutually exclusive: if the JoinHandle is being awaited it has not
 * been dropped, so the scope never holds the OwnedTask; if the handle was dropped the scope
 * owns the child and the JoinHandle can no longer call poll(). Only one waiter exists at
 * any point in the task's lifetime.
 *
 * Using `weak_ptr<Waker>` (rather than `shared_ptr<Waker>`) breaks the ownership cycles
 * documented in doc/shared_ptr_cycles.md. Task lifetime is anchored by OwnedTask (held
 * by the parent scope or JoinHandle), not by waker clones. Firing a stored waker becomes
 * `lock() + wake()`; if the task has been freed before the waker fires, lock() returns
 * null and the call is a silent no-op.
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
    std::atomic<bool>    cancelled{false};        ///< Set by `JoinHandle` destructor; checked by `Task::poll()`.
    // Category 4 (doc/task_ownership.md): notification-only, no lifetime ownership.
    // weak_ptr so that storing a waker derived from ctx.getWaker() (which IS the task)
    // does not create a shared_ptr cycle back to the owning TaskImpl.
    // At most one waiter exists: either a JoinHandle awaiter or a CoroutineScope drain loop.
    // Fire via: if (auto w = waker.lock()) w->wake();
    std::weak_ptr<Waker> waker;                   ///< Wakes the sole waiter (JoinHandle or CoroutineScope) on completion.
    ResultType           result = init_result();  ///< Set by `setResult()` on successful completion.
    std::exception_ptr   exception;               ///< Set by `setException()` on fault.

    /// @brief Returns `true` if the task has reached a terminal state (success, error, or cancelled).
    bool is_complete() const {
        std::lock_guard lock(mutex);
        return terminated;
    }

    /// @brief Signals terminal state without a result. Used by `Task` on cancellation.
    /// Fires both `join_waker` and `scope_waker`.
    void mark_done() {
        std::weak_ptr<Waker> wk;
        {
            std::lock_guard lock(mutex);
            terminated = true;
            cv.notify_all();
            wk = std::move(waker);
            self_owned.reset();  // release detached self-ref under the same lock
        }
        if (auto w = wk.lock()) w->wake();
    }

    /// @brief Stores the successful result and signals completion. Fires the completion waker.
    void setResult() requires std::is_void_v<T> {
        std::weak_ptr<Waker> wk;
        {
            std::lock_guard lock(mutex);
            result = true;
            terminated = true;
            cv.notify_all();
            wk = std::move(waker);
            self_owned.reset();
        }
        if (auto w = wk.lock()) w->wake();
    }

    /// @brief Stores the successful result and signals completion. Fires the completion waker.
    template<std::convertible_to<T> U> requires (!std::is_void_v<T>)
    void setResult(U &&value) {
        std::weak_ptr<Waker> wk;
        {
            std::lock_guard lock(mutex);
            result = std::forward<U>(value);
            terminated = true;
            cv.notify_all();
            wk = std::move(waker);
            self_owned.reset();
        }
        if (auto w = wk.lock()) w->wake();
    }

    /// @brief Stores an unhandled exception and signals completion. Fires the completion waker.
    void setException(std::exception_ptr e) {
        std::weak_ptr<Waker> wk;
        {
            std::lock_guard lock(mutex);
            exception = std::move(e);
            terminated = true;
            cv.notify_all();
            wk = std::move(waker);
            self_owned.reset();
        }
        if (auto w = wk.lock()) w->wake();
    }
};

} // namespace coro::detail
