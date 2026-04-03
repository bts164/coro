#pragma once

#include <coro/runtime/executor.h>
#include <coro/detail/task.h>
#include <coro/detail/task_state.h>
#include <coro/detail/work_stealing_deque.h>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace coro {

class Runtime;

/**
 * @brief Multi-threaded @ref Executor with per-worker local queues.
 *
 * N worker threads each own a @ref WorkStealingDeque for lock-free local
 * wakeups. Cross-thread (remote) wakeups go to a shared injection queue
 * protected by `m_mutex`.
 *
 * **Task lifecycle states** (tracked via Task::scheduling_state):
 * | State              | Location                   | Description                          |
 * |--------------------|----------------------------|--------------------------------------|
 * | Notified           | local queue or inj. queue  | Waiting to be polled                 |
 * | Running            | (worker call stack)        | Currently inside `poll()`            |
 * | RunningAndNotified | (worker call stack)        | Inside `poll()`, wake() already fired|
 * | Idle               | owned by TaskWaker         | Waiting for an external wakeup       |
 * | Done               | freed                      | `poll()` returned a terminal result  |
 *
 * **Suspension:** there is no `m_suspended` map. A task in Idle is kept alive
 * solely by the `shared_ptr<Task>` stored in its TaskWaker.
 *
 * **Thread-locals:** each worker sets `t_current_runtime` and
 * `t_current_timer_service` at startup, and a per-executor worker index
 * (`t_worker_index`) used by `enqueue()` to identify the local queue.
 */
class WorkSharingExecutor : public Executor {
public:
    /// @param runtime     Back-pointer to the owning Runtime.
    /// @param num_threads Number of worker threads to create (default: hardware concurrency).
    WorkSharingExecutor(Runtime* runtime, std::size_t num_threads = std::thread::hardware_concurrency());
    ~WorkSharingExecutor() override;

    WorkSharingExecutor(const WorkSharingExecutor&)            = delete;
    WorkSharingExecutor& operator=(const WorkSharingExecutor&) = delete;

    /// @brief Enqueues the task and routes it to the injection queue (or local
    /// queue if called from a worker thread), then wakes a worker if needed.
    void schedule(std::unique_ptr<detail::Task> task) override;

    /// @brief Route a newly-notified task to the appropriate queue.
    /// Worker thread → local queue, no lock. Any other thread → injection queue.
    void enqueue(std::shared_ptr<detail::Task> task) override;

    /// @brief Returns `true` if the injection queue is non-empty.
    /// Workers drive execution autonomously; `block_on` does not call this.
    bool poll_ready_tasks() override;

    /// @brief Delegates to `state.wait_until_done()`.
    void wait_for_completion(detail::TaskStateBase& state) override;

private:
    void worker_loop(int worker_index);

    // Per-worker local queues — no lock for the owning worker.
    std::vector<detail::WorkStealingDeque<std::shared_ptr<detail::Task>>> m_local_queues;

    // Injection queue — remote enqueue() calls and initial schedule() land here.
    std::deque<std::shared_ptr<detail::Task>> m_injection_queue;
    std::mutex               m_mutex;   ///< Guards m_injection_queue and m_stop.
    std::condition_variable  m_cv;
    bool                     m_stop{false};

    std::vector<std::thread> m_workers;
    Runtime*                 m_runtime;
};

} // namespace coro
