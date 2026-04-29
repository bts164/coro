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

    /// @brief Submit a task for scheduling. Sets `scheduling_state` to `Notified`.
    virtual void schedule(std::shared_ptr<detail::TaskBase> task) = 0;

    /// @brief Route a task to the appropriate ready queue.
    ///
    /// Called by `TaskBase::wake()` after winning the `Idle → Notified` CAS.
    /// Implementations check the calling thread's identity:
    /// - Same thread as the owning worker → local queue, no lock.
    /// - Any other thread → mutex-protected injection queue + condvar signal.
    virtual void enqueue(std::shared_ptr<detail::TaskBase> task) = 0;

    /// @brief Block the calling thread until `state.terminated` is true.
    ///
    /// `SingleThreadedExecutor` drives its own internal poll loop — it cannot block
    /// because it is the polling thread.
    /// Multi-threaded executors call `state.wait_until_done()`, blocking on `state.cv`.
    /// Because `terminated` is always set and `cv` notified under `state.mutex`, there
    /// is no lost-wakeup window.
    virtual void wait_for_completion(detail::TaskStateBase& state) = 0;
};

} // namespace coro
