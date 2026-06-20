#pragma once

// Internal — not part of the public API.
// Shared ref-counted state between executor-held tasks and their handles.
//   TaskState<T>       — shared between TaskImpl<F>      and JoinHandle<T>
//   StreamTaskState<T> — shared between StreamTaskImpl<S> and StreamHandle<T>

#include <atomic>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <coro/detail/mutex.h>
#include <coro/detail/rc.h>
#include <coro/detail/relaxed_state.h>

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
 *   poll() Pending + no concurrent wake : Running → Idle  (CAS; executor's owned map holds the only persistent ref)
 *   poll() Pending + concurrent wake    : Running → RunningAndNotified (CAS in wake())
 *   post-poll RunningAndNotified detected: RunningAndNotified → Notified (CAS; re-enqueue)
 *   wake() while Idle    : Idle → Notified  (CAS in TaskBase::wake(); calls enqueue())
 *   wake() while Running : Running → RunningAndNotified  (CAS; worker re-enqueues)
 *   poll() returns Ready : Running → Done  (store; terminal)
 */
enum class SchedulingState : uint8_t {
    Idle               = 0,  ///< Suspended; executor's owned map is the sole persistent strong ref
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
    mutable detail::Mutex   mutex;
    detail::CondVar         cv;
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
 * A single `waker` slot covers both awaiter cases:
 * - Set by `JoinHandle::poll()` when the handle is being `co_await`-ed.
 * - Set by `CoroutineScope::set_drain_waker()` when the handle was dropped and the scope
 *   is waiting for the child to drain.
 *
 * These two cases are mutually exclusive: if the JoinHandle is being awaited it has not
 * been dropped; if the handle was dropped the scope tracks the child and the JoinHandle
 * can no longer call poll(). Only one waiter exists at any point in the task's lifetime.
 *
 * Using `weak_ptr<Waker>` (rather than `shared_ptr<Waker>`) breaks the ownership cycles
 * documented in doc/shared_ptr_cycles.md. Task lifetime is anchored by the executor's
 * owned map, not by waker clones. Firing a stored waker becomes `lock() + wake()`; if
 * the task has been freed before the waker fires, lock() returns null and is a no-op.
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
    RelaxedState<bool>   cancelled{false};        ///< Set by `JoinHandle` destructor; checked by `Task::poll()`.
    // Category 4 (doc/task_ownership.md): notification-only, no lifetime ownership.
    // weak_ptr so that storing a waker derived from ctx.getWaker() (which IS the task)
    // does not create a shared_ptr cycle back to the owning TaskImpl.
    // At most one waiter exists: either a JoinHandle awaiter or a CoroutineScope drain loop.
    // Fire via: if (auto w = waker.lock()) w->wake();
    Weak<Waker>          waker;                   ///< Wakes the sole waiter (JoinHandle or CoroutineScope) on completion.
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
        Weak<Waker> wk;
        {
            std::lock_guard lock(mutex);
            terminated = true;
            cv.notify_all();
            wk = std::move(waker);
        }
        if (auto w = wk.lock()) w->wake();
    }

    /// @brief Stores the successful result and signals completion. Fires the completion waker.
    void setResult() requires std::is_void_v<T> {
        Weak<Waker> wk;
        {
            std::lock_guard lock(mutex);
            result = true;
            terminated = true;
            cv.notify_all();
            wk = std::move(waker);
        }
        if (auto w = wk.lock()) w->wake();
    }

    /// @brief Stores the successful result and signals completion. Fires the completion waker.
    template<std::convertible_to<T> U> requires (!std::is_void_v<T>)
    void setResult(U &&value) {
        Weak<Waker> wk;
        {
            std::lock_guard lock(mutex);
            result = std::forward<U>(value);
            terminated = true;
            cv.notify_all();
            wk = std::move(waker);
        }
        if (auto w = wk.lock()) w->wake();
    }

    /// @brief Stores an unhandled exception and signals completion. Fires the completion waker.
    void setException(std::exception_ptr e) {
        Weak<Waker> wk;
        {
            std::lock_guard lock(mutex);
            exception = std::move(e);
            terminated = true;
            cv.notify_all();
            wk = std::move(waker);
        }
        if (auto w = wk.lock()) w->wake();
    }
};

/**
 * @brief Shared state between StreamTaskImpl (producer) and StreamHandle (consumer).
 *
 * Co-allocated with StreamTaskImpl via make_shared. StreamHandle holds an aliased
 * shared_ptr<StreamTaskState<T>> into the same allocation — exactly parallel to how
 * JoinHandle holds shared_ptr<TaskState<T>>.
 *
 * Fields:
 *   buffer/capacity/closed/exception — bounded queue machinery for StreamHandle::poll_next()
 *   producer_waker — woken when buffer has space (backpressure recovery)
 *   consumer_waker — woken when buffer has items or stream closes
 *   terminated/scope_waker — task completion tracking for CoroutineScope drain
 */
template<typename T>
struct StreamTaskState {
    mutable detail::Mutex  mutex;
    std::queue<T>          buffer;
    std::size_t            capacity;
    bool                   closed      = false;  ///< stream exhausted, errored, or cancelled
    bool                   terminated  = false;  ///< task reached terminal state (poll() returned true)
    std::exception_ptr     exception;
    Weak<Waker>            producer_waker;  ///< woken when buffer has space
    Weak<Waker>            consumer_waker;  ///< woken when buffer has items or stream closes
    Weak<Waker>            scope_waker;     ///< woken when task terminates (CoroutineScope drain)

    explicit StreamTaskState(std::size_t cap) : capacity(cap) {}

    /// @brief Signals task completion. Fires scope_waker so CoroutineScope drain proceeds.
    void mark_done() {
        Weak<Waker> wk;
        {
            std::lock_guard lock(mutex);
            terminated = true;
            wk = std::move(scope_waker);
        }
        if (auto w = wk.lock()) w->wake();
    }
};

} // namespace coro::detail
