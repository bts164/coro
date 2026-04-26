#pragma once

// Internal — not part of the public API.
// Type-erased, heap-allocated unit of work held by the executor.
// Users never construct a TaskBase or TaskImpl directly — spawn() creates them internally.

#include <coro/detail/context.h>
#include <coro/future.h>
#include <coro/detail/task_state.h>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

// Forward-declared to avoid a circular include with executor.h (which includes task.h).
// TaskBase stores a raw Executor* so JoinHandle::cancel() can enqueue the task directly
// without going through self_waker.
namespace coro { class Executor; }

namespace coro::detail {

/**
 * @brief Non-template abstract base for type-erased tasks held by the executor.
 *
 * `TaskBase` carries the scheduling machinery (atomic CAS state, queue membership)
 * without depending on the future type `F` or result type `T`. The executor queues
 * and `TaskWaker` hold `shared_ptr<TaskBase>`.
 *
 * The concrete subclass `TaskImpl<F>` inherits both `TaskBase` and
 * `TaskState<F::OutputType>`, combining the executor interface, the result/cancellation
 * state, and the future itself into a single `make_shared` allocation.
 *
 * **Not movable:** `TaskBase` objects are always heap-allocated via `make_shared` and
 * accessed through `shared_ptr`. Moving the object itself is never required.
 */
class TaskBase {
public:
    TaskBase()                             = default;
    TaskBase(const TaskBase&)              = delete;
    TaskBase& operator=(const TaskBase&)   = delete;
    TaskBase(TaskBase&&)                   = delete;
    TaskBase& operator=(TaskBase&&)        = delete;
    virtual ~TaskBase()                    = default;

    /**
     * @brief Atomic scheduling state. Managed exclusively by the executor and TaskWaker.
     *
     * Starts at `Idle`. `schedule()` sets it to `Notified` before the first enqueue.
     * All subsequent transitions are CAS operations — see `SchedulingState` for the
     * full transition table.
     */
    std::atomic<SchedulingState> scheduling_state{SchedulingState::Idle};

    /// @brief Optional human-readable name set via SpawnBuilder::name(). Empty if unset.
    std::string name;

    /**
     * @brief Index of the worker that last ran this task, or -1 if unknown.
     *
     * Written by the WorkSharingExecutor worker loop after each poll() call.
     * Read by enqueue() to route re-enqueued tasks back to the same worker's local
     * queue (task affinity). The Running → Idle CAS provides the happens-before
     * edge that makes a plain int safe here.
     */
    int last_worker_index = -1;

    /**
     * @brief The executor that first scheduled this task. Set by Executor::schedule()
     * before the task is ever enqueued. Never changes after that.
     *
     * Used by JoinHandle::cancel() to re-enqueue a sleeping (Idle) task after setting
     * cancelled=true, without needing self_waker. Raw pointer is safe because the
     * executor always outlives the tasks it owns.
     *
     * EDGE CASE: null if the task was constructed but never scheduled (not possible
     * through any public spawn path, but defensively guarded in JoinHandle::cancel()).
     */
    Executor* owning_executor = nullptr;

    /**
     * @brief Advances the inner future by one step.
     * @return `true` if the task has reached a terminal state (Ready, Error, or Dropped);
     *         `false` if still Pending and should be moved to the Suspended state.
     */
    virtual bool poll(Context& ctx) = 0;

    /// @brief The task currently being polled on this thread, or nullptr if outside a poll.
    /// Set by the executor before each poll() call and cleared afterward.
    static thread_local TaskBase* current;

    /// @brief Returns the name of the currently-polling task, or "" if none.
    static std::string_view current_name() {
        return current ? std::string_view(current->name) : std::string_view{};
    }
};

/**
 * @brief Concrete, type-erased task. One `make_shared` allocation combines the executor
 * interface, the result/cancellation state, and the future.
 *
 * Inherits `TaskBase` (executor queue / `TaskWaker`) and `TaskState<F::OutputType>`
 * (`JoinHandle`). `spawn()` creates one instance via `make_shared<TaskImpl<F>>()` and
 * produces two aliased `shared_ptr`s from it:
 * - `shared_ptr<TaskBase>` for the executor and `TaskWaker` (`Idle` ownership)
 * - `shared_ptr<TaskState<T>>` for the `JoinHandle`
 *
 * Both alias the same `TaskImpl<F>` allocation and share the same reference count.
 *
 * **Cancellation:** if `TaskState::cancelled` is set before `poll()` is called and `F`
 * satisfies `Cancellable`, `TaskImpl` calls `m_future.cancel()` once and then polls
 * until the future drains (running the 4-step cancel protocol). For non-`Cancellable`
 * futures the task is marked done immediately.
 *
 * @tparam F Any type satisfying @ref Future.
 */
template<Future F>
class TaskImpl : public TaskBase, public TaskState<typename F::OutputType> {
public:
    using OutputType = typename F::OutputType;

    explicit TaskImpl(F future)
        : m_future(std::move(future)) {}

    bool poll(Context& ctx) override {
        if (m_completed) return true;

        const bool cancelled = this->cancelled.load(std::memory_order_relaxed);

        if (cancelled) {
            if constexpr (Cancellable<F>) {
                // Cooperative cancel: call cancel() once, then poll until the future
                // drains (Coro<T>/CoroStream<T> will run the 4-step cancel protocol).
                if (!m_cancel_requested) {
                    m_future.cancel();
                    m_cancel_requested = true;
                }
            } else {
                // Non-Cancellable (leaf) future: mark done immediately.
                this->mark_done();
                return true;
            }
        }

        auto result = m_future.poll(ctx);
        if (result.isPending()) return false;

        m_completed = true;

        if (cancelled || result.isDropped()) {
            this->mark_done();
        } else if (result.isError()) {
            this->setException(result.error());
        } else {
            if constexpr (std::is_void_v<OutputType>)
                this->setResult();
            else
                this->setResult(std::move(result).value());
        }
        return true;
    }

private:
    F    m_future;
    bool m_completed        = false;
    bool m_cancel_requested = false;
};

} // namespace coro::detail
