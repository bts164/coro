#pragma once

#include <coro/runtime/executor.h>
#include <coro/detail/task.h>
#include <coro/detail/task_state.h>
#include <coro/detail/waker.h>
#include <memory>
#include <queue>
#include <unordered_map>

namespace coro {

/**
 * @brief Single-threaded @ref Executor implementation.
 *
 * All tasks run on the thread that calls `poll_ready_tasks()` (i.e. the thread running
 * `Runtime::block_on()`). Deterministic and allocation-light; the primary executor for
 * testing and single-threaded applications.
 *
 * **Task lifecycle states:**
 * | State     | Location            | Description                                  |
 * |-----------|---------------------|----------------------------------------------|
 * | Scheduled | `m_ready` queue     | Waiting to be polled                         |
 * | Running   | (on the call stack) | Currently inside `poll()`                    |
 * | Suspended | `m_suspended` map   | Waiting for an external `Waker::wake()` call |
 * | Complete  | freed               | `poll()` returned a terminal result          |
 *
 * **Self-wake during `poll()`:** if a task's waker fires while the task is still Running,
 * `wake_task()` sets `m_running_task_woken` instead of moving to Suspended. After `poll()`
 * returns the task is re-enqueued automatically.
 */
class SingleThreadedExecutor : public Executor {
public:
    SingleThreadedExecutor();
    ~SingleThreadedExecutor() override;

    void schedule(std::unique_ptr<detail::Task> task) override;

    /// @brief Processes all tasks currently in the ready queue.
    /// Tasks enqueued *during* this pass are deferred to the next call.
    /// @return `true` if at least one task was polled.
    bool poll_ready_tasks() override;

    /// @brief Spins on `poll_ready_tasks()` until `state.terminated` is true.
    void wait_for_completion(detail::TaskStateBase& state) override;

    /// @brief Returns `true` if the ready queue is empty.
    /// @note Suspended tasks may still exist even when this returns `true`.
    bool empty() const;

    /// @brief Called by a `TaskWaker` to reschedule a task.
    /// Handles self-wake (task is currently Running), normal wake (Suspended → ready), and no-op.
    void wake_task(detail::Task* key);

private:
    std::shared_ptr<detail::Waker> make_waker(detail::Task* key);

    std::queue<std::shared_ptr<detail::Task>>                         m_ready;
    std::unordered_map<detail::Task*, std::shared_ptr<detail::Task>>  m_suspended;

    detail::Task* m_running_task_key   = nullptr;  // non-null while a task is being polled
    bool          m_running_task_woken = false;     // set by wake_task() during Running state
};

} // namespace coro
