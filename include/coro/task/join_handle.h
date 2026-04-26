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
 * `CoroutineScope` is notified and the parent coroutine waits for the child to drain before
 * completing.
 *
 * **Detaching:** `std::move(handle).detach()` lets the task run to completion without
 * cancellation. The result is discarded and the caller loses all ability to synchronize.
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
                        std::shared_ptr<detail::TaskBase> task = nullptr)
        : m_state(std::move(state)), m_task(std::move(task)) {}

    JoinHandle(const JoinHandle&)            = delete;
    JoinHandle& operator=(const JoinHandle&) = delete;

    JoinHandle(JoinHandle&&) noexcept            = default;
    JoinHandle& operator=(JoinHandle&&) noexcept = default;

    /// @brief Marks the task for cancellation and wakes it so it sees cancelled on its next poll.
    /// Satisfies the Cancellable concept — the enclosing Coro<T> cancel protocol calls this,
    /// then polls the JoinHandle until it returns non-Pending, ensuring the inner task fully
    /// drains (and its frame locals are destroyed) before the outer frame is torn down.
    ///
    /// Uses the same CAS loop as TaskWaker::wake() to handle all scheduling states:
    /// - Idle           → Notified: task is parked; enqueue it so it sees cancelled on next poll.
    /// - Running        → RunningAndNotified: task is mid-poll; worker re-enqueues after poll.
    /// - Notified / RunningAndNotified / Done: already queued or finished — no-op.
    ///
    /// EDGE CASE: if m_task is null (handle created without a TaskBase — see join_handle.h
    /// header comment) or owning_executor is null (task never scheduled), cancellation is
    /// degraded: cancelled=true is set but the task is not actively woken. It will remain
    /// parked until naturally woken by whatever it awaits, at which point it will observe
    /// cancelled=true and self-terminate. This is safe but not instantaneous.
    void cancel() noexcept {
        if (!m_state) return;
        m_state->cancelled.store(true, std::memory_order_relaxed);
        if (!m_task || !m_task->owning_executor) return;

        auto expected = detail::SchedulingState::Idle;
        while (true) {
            switch (expected) {
            case detail::SchedulingState::Idle:
                if (m_task->scheduling_state.compare_exchange_strong(
                        expected, detail::SchedulingState::Notified,
                        std::memory_order_acq_rel, std::memory_order_relaxed))
                {
                    m_task->owning_executor->enqueue(m_task);
                    return;
                }
                break;
            case detail::SchedulingState::Running:
                if (m_task->scheduling_state.compare_exchange_strong(
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
    ~JoinHandle() {
        if (!m_state) return;  // detached via detach()
        if (m_cancelOnDestroy) {
            cancel();
        }
        if (detail::t_current_coro) {
            auto state = m_state;  // capture shared_ptr by value
            detail::t_current_coro->add_child(
                [state]() { return state->is_complete(); },
                [state](std::shared_ptr<detail::Waker> w) {
                    std::lock_guard lock(state->mutex);
                    state->scope_waker = std::move(w);
                }
            );
        }
    }

    /// @brief Configures whether dropping this handle cancels the task (default: `true`).
    JoinHandle& cancelOnDestroy(bool b = true) &{
        m_cancelOnDestroy = b;
        return *this;
    }

    JoinHandle&& cancelOnDestroy(bool b = true) && {
        m_cancelOnDestroy = b;
        return std::forward<JoinHandle>(*this);
    }

    JoinHandle& detach() & {
        m_state.reset();
        m_task.reset();
        return *this;
    }

    JoinHandle&& detach() && {
        m_state.reset();
        m_task.reset();
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
        // Not ready yet — store waker so the Task can wake us on completion.
        m_state->join_waker = ctx.getWaker();
        return PollPending;
    }

private:
    bool m_cancelOnDestroy = true;
    std::shared_ptr<detail::TaskState<T>> m_state;
    // Direct reference to the TaskBase for cancel(). Holds the same TaskImpl<F>
    // allocation as m_state (aliased shared_ptr). Null only when the JoinHandle is
    // in a moved-from or detached state, or in the rare case where a JoinHandle is
    // constructed from a TaskState alone (e.g. legacy code). See cancel() for the
    // degraded-cancellation behavior when null.
    std::shared_ptr<detail::TaskBase> m_task;
};

} // namespace coro
