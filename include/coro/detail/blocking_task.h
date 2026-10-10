#pragma once

// Desktop only. The non-template base of a spawn_blocking() task.
// See doc/design/spawn_blocking.md ("Why BlockingTaskBase exists").

#include <coro/detail/blocking_cancel.h>
#include <coro/detail/task.h>
#include <coro/detail/task_state.h>

namespace coro::detail {

/**
 * @brief What `blocking_wait()`, the pool and its `ParkingExecutor` need from a blocking
 * task without knowing the callable's type.
 *
 * A blocking task is never polled. It reuses `TaskBase` for its waker
 * (`TaskBase::wake()`, unchanged), its `scheduling_state` and `cancel_task()`, with the
 * states meaning:
 *
 * | State                | Meaning                                                      |
 * |----------------------|--------------------------------------------------------------|
 * | `Notified`           | Queued in the pool, or woken while parked and about to resume |
 * | `Running`            | Thread is in the callable, or polling inside `blocking_wait`  |
 * | `RunningAndNotified` | A wake arrived while `Running`                                |
 * | `Idle`               | Thread is parked in `blocking_wait`                           |
 * | `Done`               | The callable has returned or unwound                          |
 */
struct BlockingTaskBase : TaskBase {
    /// Runs the callable to its end, once, on the calling pool thread, and publishes
    /// the outcome. Skips the callable if the task was cancelled while queued.
    virtual void run() = 0;

    /// `TaskState<T>::cancelled`.
    virtual bool is_cancelled() const noexcept = 0;

    /// The mutex and condition variable park() waits on (the task's `TaskState<T>`).
    virtual TaskStateBase& park_state() noexcept = 0;

    /// Blocks the task's own thread until the task is woken (`Running → Idle`, then
    /// waits for `Notified`). Returns at once if a wake arrived since the last park.
    /// A return means "poll again": the wake may be stale or a cancellation.
    void park();

    /// Wakes the thread in park(). Called by `ParkingExecutor::enqueue()`, i.e. by
    /// `wake()` after it has moved the task `Idle → Notified`.
    void unpark();

    /// True if a cancellation point on the task's thread should throw now.
    bool cancel_pending() const noexcept { return shield_depth == 0 && is_cancelled(); }

    /// Number of live `BlockingCancelShield`s. Touched only by the task's own thread.
    int shield_depth = 0;

    /// Intrusive membership in the pool's list of live (queued or running) tasks.
    /// Read and written only under the pool's mutex.
    BlockingTaskBase* live_prev = nullptr;
    BlockingTaskBase* live_next = nullptr;

    /// Never called: a blocking task is run, not polled. Aborts.
    bool poll(Context& ctx) final;
};

} // namespace coro::detail
