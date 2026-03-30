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

// JoinHandle<T> — returned by spawn(). Satisfies Future<T>.
//
// The caller co_awaits the handle to retrieve the spawned task's result.
// Dropping a JoinHandle without awaiting it cancels the task.
// Calling detach() lets the task run to completion without cancellation;
// the result is discarded and the caller loses all ability to synchronize.
//
// JoinHandle and the executor-held Task share a TaskState<T> via shared_ptr.
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

    // Cancels the task if not yet completed.
    // If destroyed inside a coroutine poll() call, registers with the enclosing
    // CoroutineScope so the parent waits for this child to finish before completing.
    ~JoinHandle() {
        if (!m_state) return;  // detached via detach()
        m_state->cancelled.store(true, std::memory_order_relaxed);
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

    // Detaches without cancelling. The task runs to completion; the result is discarded.
    // Consumes the JoinHandle — the caller can no longer synchronize or cancel.
    void detach() && {
        m_state.reset();
    }

    PollResult<T> poll(detail::Context& ctx) {
        std::lock_guard lock(m_state->mutex);
        if (m_state->exception)
            return PollError(m_state->exception);
        if (m_state->result.has_value())
            return std::move(*m_state->result);
        // Not ready yet — store waker so the Task can wake us on completion.
        m_state->join_waker = ctx.getWaker();
        return PollPending;
    }

private:
    std::shared_ptr<detail::TaskState<T>> m_state;
};


// JoinHandle<void> — specialization for tasks with no return value.
template<>
class [[nodiscard]] JoinHandle<void> {
public:
    using OutputType = void;

    explicit JoinHandle(std::shared_ptr<detail::TaskState<void>> state)
        : m_state(std::move(state)) {}

    JoinHandle(const JoinHandle&)            = delete;
    JoinHandle& operator=(const JoinHandle&) = delete;

    JoinHandle(JoinHandle&&) noexcept            = default;
    JoinHandle& operator=(JoinHandle&&) noexcept = default;

    ~JoinHandle() {
        if (!m_state) return;  // detached via detach()
        if (m_cancelOnDestroy) {
            m_state->cancelled.store(true, std::memory_order_relaxed);
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

    PollResult<void> poll(detail::Context& ctx) {
        std::lock_guard lock(m_state->mutex);
        if (m_state->exception)
            return PollError(m_state->exception);
        if (m_state->done)
            return PollReady;
        m_state->join_waker = ctx.getWaker();
        return PollPending;
    }

private:
    bool m_cancelOnDestroy = true;
    std::shared_ptr<detail::TaskState<void>> m_state;
};

} // namespace coro
