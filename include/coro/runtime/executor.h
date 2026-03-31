#pragma once

#include <coro/detail/task.h>
#include <coro/detail/task_state.h>
#include <memory>

namespace coro {

/**
 * @brief Abstract scheduling interface. Accepts type-erased tasks and decides when to poll them.
 *
 * Does not own threads or the I/O reactor — those are owned by @ref Runtime.
 *
 * Concrete implementations:
 * - @ref SingleThreadedExecutor — runs all tasks on the calling thread (deterministic, good for tests).
 * - @ref WorkSharingExecutor — multi-threaded, single shared queue.
 */
class Executor {
public:
    virtual ~Executor();

    /// @brief Submit a task for scheduling. The executor takes ownership.
    virtual void schedule(std::unique_ptr<detail::Task> task) = 0;

    /// @brief Poll all currently-ready tasks once.
    /// @return `true` if at least one task was processed; `false` if the ready queue was empty.
    virtual bool poll_ready_tasks() = 0;

    /// @brief Block the calling thread until `state.terminated` is true.
    ///
    /// `SingleThreadedExecutor` spins on `poll_ready_tasks()` — it cannot block because
    /// it is the polling thread.
    /// `WorkSharingExecutor` calls `state.wait_until_done()`, blocking on `state.cv`.
    /// Because `terminated` is always set and `cv` notified under `state.mutex`, there
    /// is no lost-wakeup window.
    virtual void wait_for_completion(detail::TaskStateBase& state) = 0;
};

} // namespace coro
