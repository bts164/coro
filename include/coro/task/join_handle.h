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
 * @brief Owned handle to a spawned task. Returned by `spawn(...)`. Satisfies `Future<T>`.
 *
 * **Awaiting:** `co_await handle` suspends the caller until the task completes and returns its value.
 *
 * **Dropping:** destroying a `JoinHandle<T>` without `co_await`-ing it marks the task for
 * cancellation. If destroyed inside a coroutine's `poll()` call the enclosing
 * `CoroutineScope` records a `weak_ptr<TaskBase>` to the child and waits for it to drain
 * before the parent coroutine completes. Task lifetime is always anchored by the executor's
 * owned map — no ownership transfer is needed.
 *
 * **Detaching:** `std::move(handle).detach()` lets the task run to completion without
 * cancellation. The executor's owned map keeps it alive until it reaches a terminal state.
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
                        std::weak_ptr<detail::TaskBase> task_ref = {})
        : m_state(std::move(state)), m_task_ref(std::move(task_ref)) {}

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
    /// EDGE CASE: if m_task_ref is expired (handle moved-from or detached) or owning_executor is
    /// null (task never scheduled), cancellation is degraded: cancelled=true is set but the
    /// task is not actively woken. It will remain parked until naturally woken by whatever
    /// it awaits, at which point it will observe cancelled=true and self-terminate.
    void cancel() noexcept {
        if (!m_state) return;
        m_state->cancelled.store(true, std::memory_order_relaxed);
        auto task = m_task_ref.lock();
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

    /// @brief Cancels the task and returns the handle for the caller to co_await.
    ///
    /// Calls cancel() then moves the handle out so the caller can co_await it
    /// directly — no intermediate coroutine frame, no extra allocation.
    ///
    ///   co_await std::move(*g_handle).cancel_and_join();
    ///   g_handle = spawn(new_task());
    ///
    /// The caller suspends until the task has fully drained (frame and all scope
    /// children destroyed), then resumes. For JoinHandle<void>, the co_await
    /// completes silently whether the task finished normally or was cancelled.
    [[nodiscard]] JoinHandle cancel_and_join() && {
        cancel();
        return std::move(*this);
    }

    /// @brief Cancels the task (if cancelOnDestroy) and registers with the enclosing scope.
    ///
    /// If inside a coroutine poll (t_current_coro != null), records a weak_ptr to the child
    /// in the scope's pending list so the parent waits for the child to drain.
    /// The executor's owned map keeps the task alive — no ownership transfer is needed.
    ~JoinHandle() {
        if (!m_state) return;  // moved-from or already cleaned up
        if (m_cancelOnDestroy) {
            cancel();
        }
        if (detail::t_current_coro) {
            detail::t_current_coro->add_child(m_task_ref);
        }
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
    /// The executor's owned map already keeps the task alive until it reaches a terminal state.
    /// Detaching simply releases the JoinHandle's state reference and task_ref.
    JoinHandle& detach() & {
        m_state.reset();
        m_task_ref = {};
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
        m_state->waker = ctx.get_weak_waker();
        return PollPending;
    }

private:
    bool m_cancelOnDestroy = true;
    // Category 2 (doc/task_ownership.md): aliased shared_ptr into the same TaskImpl
    // allocation as m_task_ref. Provides typed access to the result and waker slot.
    std::shared_ptr<detail::TaskState<T>> m_state;
    // Category 4 (doc/task_ownership.md): notification/cancel only — no lifetime ownership.
    // The executor's owned map is the lifetime anchor (Category 1).
    std::weak_ptr<detail::TaskBase> m_task_ref;
};

} // namespace coro
