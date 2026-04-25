#pragma once

#include <coro/detail/context.h>
#include <coro/future.h>
#include <coro/detail/poll_result.h>
#include <coro/detail/task_state.h>
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

    explicit JoinHandle(std::shared_ptr<detail::TaskState<T>> state)
        : m_state(std::move(state)) {}

    JoinHandle(const JoinHandle&)            = delete;
    JoinHandle& operator=(const JoinHandle&) = delete;

    JoinHandle(JoinHandle&&) noexcept            = default;
    JoinHandle& operator=(JoinHandle&&) noexcept = default;

    /// @brief Marks the task for cancellation and wakes it so it sees cancelled on its next poll.
    /// Satisfies the Cancellable concept — the enclosing Coro<T> cancel protocol calls this,
    /// then polls the JoinHandle until it returns non-Pending, ensuring the inner task fully
    /// drains (and its frame locals are destroyed) before the outer frame is torn down.
    void cancel() noexcept {
        if (!m_state) return;
        m_state->cancelled.store(true, std::memory_order_relaxed);
        std::shared_ptr<detail::Waker> waker;
        {
            std::lock_guard lock(m_state->mutex);
            waker = std::move(m_state->self_waker);
        }
        if (waker) waker->wake();
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
        return *this;
    }

    JoinHandle&& detach() && {
        m_state.reset();
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
};

} // namespace coro
