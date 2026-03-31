#pragma once

#include <coro/runtime/executor.h>
#include <coro/detail/task.h>
#include <coro/detail/task_state.h>
#include <coro/detail/waker.h>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace coro {

class Runtime;

/**
 * @brief Multi-threaded @ref Executor with a single shared task queue.
 *
 * All N worker threads pull from and push to the same `m_queue`, protected by `m_mutex`.
 * Tasks run in parallel across threads; there is no per-thread affinity or work-stealing.
 *
 * **Task lifecycle states:**
 * | State     | Location            | Description                                        |
 * |-----------|---------------------|----------------------------------------------------|
 * | Ready     | `m_queue` deque     | Waiting to be dequeued by a worker                 |
 * | Running   | (worker call stack) | Currently inside `poll()`; tracked in `m_self_woken` |
 * | Suspended | `m_suspended` map   | Waiting for an external `Waker::wake()` call       |
 * | Done      | freed               | `poll()` returned a terminal result                |
 *
 * **Self-wake during `poll()`:** if `wake_task()` fires while a task is Running, the
 * `m_self_woken` flag for that task is set to `true`. After `poll()` returns the worker
 * checks the flag and re-enqueues the task rather than moving it to Suspended.
 *
 * **Thread-locals:** each worker thread sets `t_current_runtime` at startup so that
 * `coro::spawn()` and `JoinSet::spawn()` resolve to the owning `Runtime`.
 */
class WorkSharingExecutor : public Executor {
public:
    /// @param num_threads Number of worker threads to create.
    /// @param runtime Back-pointer to the owning Runtime, used to set `t_current_runtime`
    ///                on each worker thread.
    WorkSharingExecutor(std::size_t num_threads, Runtime* runtime);
    ~WorkSharingExecutor() override;

    WorkSharingExecutor(const WorkSharingExecutor&)            = delete;
    WorkSharingExecutor& operator=(const WorkSharingExecutor&) = delete;

    /// @brief Enqueues the task and wakes one worker.
    void schedule(std::unique_ptr<detail::Task> task) override;

    /// @brief Returns `true` if the ready queue is non-empty.
    /// @note Workers drive execution autonomously; `block_on` does not call this.
    bool poll_ready_tasks() override;

    /// @brief Delegates to `state.wait_until_done()`, blocking on `state.cv`.
    /// No executor-level condvar involved — the state owns the completion signal.
    void wait_for_completion(detail::TaskStateBase& state) override;

    /// @brief Called by a `TaskWaker` to reschedule a task.
    /// If the task is currently Running, sets its self-wake flag.
    /// Otherwise moves it from `m_suspended` to `m_queue` and wakes a worker.
    void wake_task(detail::Task* key);

private:
    /// @brief Main loop executed by each worker thread.
    void worker_loop();

    std::deque<std::shared_ptr<detail::Task>>                        m_queue;
    std::unordered_map<detail::Task*, std::shared_ptr<detail::Task>> m_suspended;
    std::unordered_map<detail::Task*, bool>                          m_self_woken;

    std::mutex               m_mutex;
    std::condition_variable  m_cv;
    bool                     m_stop{false};
    std::vector<std::thread> m_workers;

    Runtime* m_runtime;
};

} // namespace coro
