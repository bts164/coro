#pragma once

#include <coro/detail/task.h>
#include <coro/detail/task_state.h>
#include <coro/detail/rc.h>
#include <functional>
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

    // --- Runtime shutdown. See doc/design/runtime_shutdown.md. ----------------------

    /// @brief Starts shutdown: cancels every task this executor owns, and from now on
    /// cancels each task passed to schedule() before it is first polled.
    ///
    /// Does not wait. The tasks drain as they are polled; has_tasks() turns false
    /// once the last one is done. Called once, by `Runtime::shutdown()`.
    virtual void begin_shutdown() = 0;

    /// @brief True while the executor owns a task that has not finished.
    virtual bool has_tasks() const = 0;

    /// @brief True if tasks only run while a thread is inside wait_for_completion()
    /// or run_until(), as on @ref CurrentThreadExecutor. The thread that shuts the
    /// runtime down must then run the drain itself.
    virtual bool runs_on_calling_thread() const noexcept { return false; }

    /// @brief Runs tasks on the calling thread until `done()` returns true. Only
    /// meaningful when runs_on_calling_thread(); the default does nothing.
    ///
    /// `done` is evaluated on the calling thread, between tasks, with no executor
    /// lock held.
    virtual void run_until(const std::function<bool()>& done) { (void)done; }

    /// @brief Makes a run_until() in progress on another thread evaluate `done()`
    /// again soon. Thread-safe. The default does nothing.
    virtual void recheck_run_until() noexcept {}
};

} // namespace coro
