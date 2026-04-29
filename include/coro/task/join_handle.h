#pragma once

#include <coro/detail/context.h>
#include <coro/future.h>
#include <coro/detail/poll_result.h>
#include <coro/detail/task_state.h>
#include <coro/detail/task.h>
#include <coro/runtime/executor.h>
#include <coro/detail/coro_scope.h>
#include <memory>
#include <mutex>
#include <utility>

namespace coro {

/**
 * @brief Owned handle to a spawned task. Returned by `spawn(...).submit()`. Satisfies `Future<T>`.
 *
 * **Awaiting:** `co_await handle` suspends the caller until the task completes and returns its value.
 *
 * **Dropping:** destroying a `JoinHandle<T>` without `co_await`-ing it marks the task for
 * cancellation. If destroyed inside a coroutine's `poll()` call the enclosing
 * `CoroutineScope` takes ownership of the child's `OwnedTask` and waits for the child to
 * drain before the parent coroutine completes.
 *
 * **Detaching:** `std::move(handle).detach()` lets the task run to completion without
 * cancellation. The task anchors its own lifetime via `TaskStateBase::self_owned` until
 * it reaches a terminal state.
 *
 * **Ownership:** `m_owned` is the sole persistent strong reference to the task. Wakers
 * stored by futures awaiting this task use `weak_ptr<Waker>` and do not contribute to
 * the reference count. See doc/task_ownership.md for the ownership model.
 *
 * `JoinHandle` is `[[nodiscard]]` — silently discarding it would cancel the task immediately,
 * which is almost always unintentional.
 *
 * @tparam T The value type produced by the spawned task.
 */
template<typename T>
class [[nodiscard]] JoinHandle {
public:
    using OutputType = T;

    explicit JoinHandle(std::shared_ptr<detail::TaskState<T>> state,
                        detail::OwnedTask owned = {})
        : m_state(std::move(state)), m_owned(std::move(owned)) {}

    JoinHandle(const JoinHandle&)            = delete;
    JoinHandle& operator=(const JoinHandle&) = delete;

    JoinHandle(JoinHandle&&) noexcept            = default;
    JoinHandle& operator=(JoinHandle&&) noexcept = default;

    /// @brief Marks the task for cancellation and wakes it so it sees cancelled on its next poll.
    /// Satisfies the Cancellable concept — the enclosing Coro<T> cancel protocol calls this,
    /// then polls the JoinHandle until it returns non-Pending, ensuring the inner task fully
    /// drains (and its frame locals are destroyed) before the outer frame is torn down.
    ///
    /// Uses the same CAS loop as TaskBase::wake() to handle all scheduling states:
    /// - Idle           → Notified: task is parked; enqueue it so it sees cancelled on next poll.
    /// - Running        → RunningAndNotified: task is mid-poll; worker re-enqueues after poll.
    /// - Notified / RunningAndNotified / Done: already queued or finished — no-op.
    ///
    /// EDGE CASE: if m_owned is empty (handle moved-from or detached) or owning_executor is
    /// null (task never scheduled), cancellation is degraded: cancelled=true is set but the
    /// task is not actively woken. It will remain parked until naturally woken by whatever
    /// it awaits, at which point it will observe cancelled=true and self-terminate.
    void cancel() noexcept {
        if (!m_state) return;
        m_state->cancelled.store(true, std::memory_order_relaxed);
        auto task = m_owned.get();
        if (!task || !task->owning_executor) return;

        auto expected = detail::SchedulingState::Idle;
        while (true) {
            switch (expected) {
            case detail::SchedulingState::Idle:
                if (task->scheduling_state.compare_exchange_strong(
                        expected, detail::SchedulingState::Notified,
                        std::memory_order_acq_rel, std::memory_order_relaxed))
                {
                    task->owning_executor->enqueue(task);
                    return;
                }
                break;
            case detail::SchedulingState::Running:
                if (task->scheduling_state.compare_exchange_strong(
                        expected, detail::SchedulingState::RunningAndNotified,
                        std::memory_order_acq_rel, std::memory_order_relaxed))
                {
                    return;
                }
                break;
            case detail::SchedulingState::Notified:
            case detail::SchedulingState::RunningAndNotified:
            case detail::SchedulingState::Done:
                return;
            default:
                std::abort();
            }
        }
    }

    /// @brief Cancels the task (if cancelOnDestroy) and registers with the enclosing scope.
    ///
    /// If inside a coroutine poll (t_current_coro != null), transfers OwnedTask ownership
    /// to the scope so the parent waits for the child to drain.
    ///
    /// If not inside a coroutine and not cancelling (cancelOnDestroy=false), and the task
    /// is scheduled (has an executor), set self_owned on the task's state so it anchors
    /// its own lifetime until completion — equivalent to an implicit detach().
    /// Unscheduled tasks (owning_executor==null) are freed immediately when m_owned drops.
    ~JoinHandle() {
        if (!m_state) return;  // moved-from or already cleaned up
        if (m_cancelOnDestroy) {
            cancel();  // wakes the task → executor gets a temp ref before m_owned drops
        }
        if (detail::t_current_coro && m_owned) {
            // Inside a coroutine: transfer ownership to scope for structured concurrency.
            detail::t_current_coro->add_child(std::move(m_owned));
        } else if (!m_cancelOnDestroy && m_owned) {
            // Outside a scope, not cancelling: if the task is scheduled, set self_owned
            // so it can survive to completion while parked (no waker holds a strong ref).
            auto task = m_owned.get();
            if (task && task->owning_executor) {
                std::shared_ptr<void> self_ref = m_owned.get();
                {
                    std::lock_guard lock(m_state->mutex);
                    if (!m_state->terminated) {
                        m_state->self_owned = std::move(self_ref);
                    }
                }
            }
            // m_owned destroyed after lock released (or immediately if not scheduled)
        }
        // m_state and m_owned (if not transferred to scope) destroyed here
    }

    /// @brief Configures whether dropping this handle cancels the task (default: `true`).
    JoinHandle& cancelOnDestroy(bool b = true) & {
        m_cancelOnDestroy = b;
        return *this;
    }

    JoinHandle&& cancelOnDestroy(bool b = true) && {
        m_cancelOnDestroy = b;
        return std::forward<JoinHandle>(*this);
    }

    /// @brief Lets the task run without cancellation; the caller surrenders all synchronization.
    ///
    /// Moves the `OwnedTask` into `TaskStateBase::self_owned` so the task anchors its own
    /// lifetime until it reaches a terminal state (mark_done/setResult/setException clears
    /// self_owned under the state mutex). The task is freed when the executor drops its
    /// final temporary reference after the terminal method fires.
    ///
    /// Sets self_owned under m_state->mutex before releasing m_owned to close any window
    /// where neither the caller's OwnedTask nor self_owned holds a strong reference.
    JoinHandle& detach() & {
        if (m_state && m_owned) {
            std::shared_ptr<void> self_ref = m_owned.get();
            {
                std::lock_guard lock(m_state->mutex);
                if (!m_state->terminated) {
                    m_state->self_owned = std::move(self_ref);
                }
            }
            // m_owned destroyed after lock released
        }
        m_state.reset();
        m_owned = {};
        return *this;
    }

    JoinHandle&& detach() && {
        detach();
        return std::forward<JoinHandle>(*this);
    }

    PollResult<T> poll(detail::Context& ctx) {
        std::lock_guard lock(m_state->mutex);
        // Check terminated first: covers normal completion, exception, and cancellation
        // (mark_done sets terminated without setting result, so checking result alone
        // would incorrectly leave a cancelled void task as Pending indefinitely).
        if (m_state->terminated) {
            if (m_state->exception)
                return PollError(m_state->exception);
            if constexpr (std::is_void_v<T>) {
                if (m_state->result) {
                    m_state->result = false;
                    return PollReady;
                }
            } else {
                if (m_state->result.has_value())
                    return std::move(*m_state->result);
            }
            return PollDropped; // task was cancelled before producing a result
        }
        // Not ready yet — store weak waker so the Task can wake us on completion.
        // weak_ptr avoids contributing to ownership cycles (see doc/task_ownership.md).
        m_state->join_waker = ctx.get_weak_waker();
        return PollPending;
    }

private:
    bool m_cancelOnDestroy = true;
    // Category 2 (doc/task_ownership.md): aliased shared_ptr into the same TaskImpl
    // allocation as m_owned. Provides typed access to the result and waker slot.
    // Does not independently anchor the task lifetime — m_owned does that.
    std::shared_ptr<detail::TaskState<T>> m_state;
    // Category 1 (doc/task_ownership.md): sole persistent strong reference to the task.
    // Move-only to enforce the single-owner invariant. Null after detach() or move-from.
    detail::OwnedTask m_owned;
};

} // namespace coro
