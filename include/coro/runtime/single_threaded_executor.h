#pragma once

#include <coro/runtime/executor.h>
#include <coro/detail/task.h>
#include <coro/detail/task_state.h>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>

namespace coro {

/**
 * @brief Single-threaded @ref Executor implementation.
 *
 * All tasks run on the thread that calls `wait_for_completion()` (i.e. the
 * thread running `Runtime::block_on()`). Deterministic and allocation-light;
 * the primary executor for testing and single-threaded applications.
 *
 * **Task lifecycle states** (tracked via Task::scheduling_state):
 * | State              | Location              | Description                          |
 * |--------------------|-----------------------|--------------------------------------|
 * | Notified           | `m_ready` queue       | Waiting to be polled                 |
 * | Running            | (on the call stack)   | Currently inside `poll()`            |
 * | RunningAndNotified | (on the call stack)   | Inside `poll()`, wake() already fired|
 * | Idle               | owned by TaskWaker    | Waiting for an external wakeup       |
 * | Done               | freed                 | `poll()` returned a terminal result  |
 *
 * **External wakeups:** `TimerService` and other external threads call `enqueue()`
 * via `TaskWaker::wake()`. External calls are routed to `m_incoming_wakes` (the
 * injection queue), protected by `m_remote_mutex`. `wait_for_completion()` blocks
 * on `m_remote_cv` when the ready queue is empty rather than returning prematurely.
 */
class SingleThreadedExecutor : public Executor {
public:
    SingleThreadedExecutor();
    ~SingleThreadedExecutor() override;

    void schedule(std::unique_ptr<detail::Task> task) override;

    /// @brief Route a newly-notified task to the appropriate queue.
    /// Local thread (poll thread) → directly to m_ready, no lock.
    /// Any other thread → m_incoming_wakes + m_remote_cv signal.
    void enqueue(std::shared_ptr<detail::Task> task) override;

    /// @brief Drains the injection queue, then polls all ready tasks once.
    /// @return `true` if at least one task was polled.
    bool poll_ready_tasks() override;

    /// @brief Blocks until `state.terminated`. Drives the poll loop internally.
    void wait_for_completion(detail::TaskStateBase& state) override;

    /// @brief Returns `true` if the ready queue and injection queue are both empty.
    bool empty() const;

private:
    std::queue<std::shared_ptr<detail::Task>> m_ready;

    // Injection queue — remote enqueue() calls land here.
    std::deque<std::shared_ptr<detail::Task>> m_incoming_wakes;
    mutable std::mutex                        m_remote_mutex;
    std::condition_variable                   m_remote_cv;

    // Identity of the thread currently running wait_for_completion().
    // Used by enqueue() to distinguish local vs. remote wakeups.
    std::thread::id m_poll_thread_id;
};

} // namespace coro
