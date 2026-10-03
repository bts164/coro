#pragma once

#include <coro/detail/task.h>
#include <coro/detail/task_state.h>
#include <coro/detail/rc.h>
#include <memory>

namespace coro {

/**
 * @brief Abstract scheduling interface. Accepts type-erased tasks and decides when to poll them.
 *
 * Does not own threads or the I/O reactor — those are owned by @ref Runtime.
 *
 * Concrete implementations:
 * - @ref CurrentThreadExecutor — runs all tasks on the calling thread (`Runtime(1)`, and the Pico port).
 * - @ref WorkStealingExecutor — multi-threaded, per-worker queues with stealing (`Runtime(n)`).
 * - @ref WorkSharingExecutor — multi-threaded, single shared queue.
 */
class Executor {
public:
    virtual ~Executor();

    /// @brief Submit a task for scheduling. Sets `scheduling_state` to `Notified`.
    virtual void schedule(detail::Rc<detail::TaskBase> task) = 0;

    /// @brief Route a task to the appropriate ready queue.
    ///
    /// Called by `TaskBase::wake()` after winning the `Idle → Notified` CAS.
    /// Implementations check the calling thread's identity:
    /// - Same thread as the owning worker → local queue, no lock.
    /// - Any other thread → mutex-protected injection queue + condvar signal.
    virtual void enqueue(detail::Rc<detail::TaskBase> task) = 0;

    /// @brief Block the calling thread until `state.terminated` is true.
    ///
    /// `CurrentThreadExecutor` drives its own internal poll loop — it cannot block
    /// because it is the polling thread.
    /// Multi-threaded executors call `state.wait_until_done()`, blocking on `state.cv`.
    /// Because `terminated` is always set and `cv` notified under `state.mutex`, there
    /// is no lost-wakeup window.
    virtual void wait_for_completion(detail::TaskStateBase& state) = 0;

    /// @brief True if this executor's threads turn the Runtime's @ref IoDriver.
    ///
    /// Driver-backed I/O primitives (e.g. `UdpSocket`) require it: on an executor
    /// that never turns the driver, a wait for readiness would never be woken.
    virtual bool turns_io_driver() const noexcept { return false; }
};

} // namespace coro
