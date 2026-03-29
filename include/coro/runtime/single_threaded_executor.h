#pragma once

#include <coro/executor.h>
#include <coro/task.h>
#include <coro/waker.h>
#include <memory>
#include <queue>
#include <unordered_map>

namespace coro {

// SingleThreadedExecutor — first concrete Executor implementation.
// All tasks run on a single thread (the one that calls poll_ready_tasks / Runtime::block_on).
//
// Task lifecycle:
//   Scheduled  — in m_ready, waiting to be polled
//   Running    — currently inside poll(); tracked via m_running_task_key
//   Suspended  — in m_suspended, waiting for an external waker
//   Complete   — poll() returned true; freed
//
// Self-wake during poll(): wake_task() detects the Running state via m_running_task_key
// and sets m_running_task_woken. After poll() returns the task is re-enqueued rather than
// moved to m_suspended. This mirrors the RUNNING | SCHEDULED state in Tokio's task header,
// but without atomics since only one thread ever touches the executor.
class SingleThreadedExecutor : public Executor {
public:
    SingleThreadedExecutor();
    ~SingleThreadedExecutor() override;

    void schedule(std::unique_ptr<detail::Task> task) override;

    // Processes all tasks currently in the ready queue (not ones enqueued during this pass).
    // Returns true if at least one task was polled.
    bool poll_ready_tasks() override;

    // True if the ready queue is empty (suspended tasks may still exist).
    bool empty() const;

    // Called by the TaskWaker. Handles three cases:
    //  - key == m_running_task_key: self-wake, set m_running_task_woken flag
    //  - key in m_suspended: move to m_ready
    //  - otherwise: already scheduled or complete, no-op
    void wake_task(detail::Task* key);

private:
    std::shared_ptr<Waker> make_waker(detail::Task* key);

    std::queue<std::shared_ptr<detail::Task>>                         m_ready;
    std::unordered_map<detail::Task*, std::shared_ptr<detail::Task>>  m_suspended;

    detail::Task* m_running_task_key   = nullptr;  // non-null while a task is being polled
    bool          m_running_task_woken = false;     // set by wake_task() during Running state
};

} // namespace coro
