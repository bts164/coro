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
#include <unordered_set>
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
 * **I/O and timers:** an idle worker blocks in the runtime's IoDriver::turn() in
 * place of the condition variable, if no other worker already holds it. Remote
 * enqueues then also unpark the driver. Kept deliberately simple (it is mainly a
 * debugging aid): busy workers never turn the driver, so I/O and timers wait until a
 * worker goes idle. See doc/design/executor_design.md,
 * "I/O and timers: the driver handoff".
 *
 * **Task lifecycle states** (tracked via Task::scheduling_state):
 * | State              | Location                   | Description                          |
 * |--------------------|----------------------------|--------------------------------------|
 * | Notified           | local queue or inj. queue  | Waiting to be polled                 |
 * | Running            | (worker call stack)        | Currently inside `poll()`            |
 * | RunningAndNotified | (worker call stack)        | Inside `poll()`, wake() already fired|
 * | Idle               | kept alive by waker clone  | Waiting for an external wakeup       |
 * | Done               | freed                      | `poll()` returned a terminal result  |
 *
 * **Suspension:** there is no `m_suspended` map. A task in Idle is kept alive
 * solely by the waker clone(s) held by the leaf futures awaiting it.
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
    void schedule(std::shared_ptr<detail::TaskBase> task) override;

    /// @brief Route a newly-notified task to the appropriate queue.
    /// Worker thread → local queue, no lock. Any other thread → injection queue.
    void enqueue(std::shared_ptr<detail::TaskBase> task) override;

    /// @brief Delegates to `state.wait_until_done()`.
    void wait_for_completion(detail::TaskStateBase& state) override;

    /// True: an idle worker turns the runtime's IoDriver when no other worker holds it.
    bool turns_io_driver() const noexcept override { return true; }

private:
    void worker_loop(int worker_index);

    /// Called with an empty local queue. Pops the injection queue, else turns the
    /// driver if no other worker holds it, else waits on m_cv. Returns a task, or
    /// null on shutdown or when a driver turn queued tasks locally.
    std::shared_ptr<detail::TaskBase> wait_for_task(int worker_index);

    // Category 3 (see doc/task_ownership.md): temporary strong references held while
    // a task is Notified (in queue) or Running (local variable in worker loop).
    // Dropped when task parks (Running → Idle). Must be shared_ptr — no other strong
    // reference keeps a Notified task alive between enqueue and the worker's poll call.
    std::vector<detail::WorkStealingDeque<std::shared_ptr<detail::TaskBase>>> m_local_queues;

    // Injection queue — same category 3 reasoning as m_local_queues.
    std::deque<std::shared_ptr<detail::TaskBase>> m_injection_queue;
    std::mutex               m_mutex;   ///< Guards m_injection_queue, m_stop and m_driver_held.
    std::condition_variable  m_cv;
    bool                     m_stop{false};
    /// True while one idle worker is blocked in IoDriver::turn() in place of m_cv.
    /// At most one worker holds the driver; the others wait on m_cv. GUARDED BY m_mutex.
    bool                     m_driver_held{false};

    std::vector<std::thread> m_workers;
    Runtime*                 m_runtime;

    // Category 1 (doc/task_ownership.md): persistent lifetime anchor for every live task.
    // Inserted in schedule(), erased after poll() returns true (task reached terminal state).
    std::mutex                                                               m_owned_mutex;
    std::unordered_set<std::shared_ptr<detail::TaskBase>> m_owned_tasks;
};

} // namespace coro
