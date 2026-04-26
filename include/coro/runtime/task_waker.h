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
    std::shared_ptr<detail::TaskBase> task;
    Executor*                         executor;

    void wake() override {
        // Loop so that a CAS failure retries with the freshly-observed state.
        // compare_exchange_strong updates `expected` on failure, so each iteration
        // starts from the actual current state. A CAS failure in Idle or Running
        // breaks out of the switch and loops; all terminal states return directly.
        auto expected = detail::SchedulingState::Idle;
        while (true) {
            switch (expected) {
            case detail::SchedulingState::Idle:
                // Common path: task is suspended. Transition to Notified and enqueue.
                if (task->scheduling_state.compare_exchange_strong(
                        expected, detail::SchedulingState::Notified,
                        std::memory_order_acq_rel,
                        std::memory_order_relaxed))
                {
                    executor->enqueue(task);
                    return;
                }
                break; // expected updated by CAS; retry with the new state

            case detail::SchedulingState::Running:
                // Task is mid-poll. Mark it RunningAndNotified so the worker
                // re-enqueues it after poll() returns instead of parking.
                if (task->scheduling_state.compare_exchange_strong(
                        expected, detail::SchedulingState::RunningAndNotified,
                        std::memory_order_acq_rel,
                        std::memory_order_relaxed))
                {
                    return;
                }
                break; // expected updated by CAS; retry with the new state

            case detail::SchedulingState::Notified:
            case detail::SchedulingState::RunningAndNotified:
                // Already queued or already flagged for re-enqueue — no-op.
                return;

            case detail::SchedulingState::Done:
                // Task completed before this wake() arrived — no-op.
                return;

            default:
                std::abort(); // unknown state — bug in executor
            }
        }
    }

    std::shared_ptr<detail::Waker> clone() override {
        return std::make_shared<TaskWaker>(*this);
    }
};

} // namespace coro
