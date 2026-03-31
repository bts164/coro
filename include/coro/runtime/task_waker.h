#pragma once

// Internal — not part of the public API.
// Shared TaskWaker used by both SingleThreadedExecutor and WorkSharingExecutor.

#include <coro/runtime/executor.h>
#include <coro/detail/task.h>
#include <coro/detail/task_state.h>
#include <coro/detail/waker.h>
#include <memory>

namespace coro {

/**
 * @brief Concrete Waker implementation used by both executors.
 *
 * Holds a shared_ptr<Task> (keeping the task alive while suspended) and an
 * Executor* (to route the re-enqueue). Uses CAS on Task::scheduling_state so
 * that concurrent wake() calls are safe without any mutex.
 *
 * Multiple clones of the same TaskWaker may exist simultaneously (e.g. one
 * stored by a timer callback, one by a channel). Only the first clone to win
 * the Idle → Notified CAS calls enqueue(); the rest are no-ops.
 */
struct TaskWaker final : detail::Waker {
    std::shared_ptr<detail::Task> task;
    Executor*                     executor;

    void wake() override {
        // Fast path: task is Idle — first waker to fire transitions it to Notified
        // and hands ownership to the queue.
        auto expected = detail::SchedulingState::Idle;
        if (task->scheduling_state.compare_exchange_strong(
                expected, detail::SchedulingState::Notified,
                std::memory_order_acq_rel,
                std::memory_order_relaxed))
        {
            executor->enqueue(task);
            return;
        }

        // Task is Running (inside poll()). Mark it RunningAndNotified so the
        // worker re-enqueues it after poll() returns instead of parking.
        expected = detail::SchedulingState::Running;
        task->scheduling_state.compare_exchange_strong(
            expected, detail::SchedulingState::RunningAndNotified,
            std::memory_order_acq_rel,
            std::memory_order_relaxed);
        // If neither CAS succeeds the task is already Notified or
        // RunningAndNotified — another waker won the race, no-op.
    }

    std::shared_ptr<detail::Waker> clone() override {
        return std::make_shared<TaskWaker>(*this);
    }
};

} // namespace coro
