#pragma once

#include <coro/context.h>
#include <coro/future.h>
#include <coro/poll_result.h>
#include <coro/task_state.h>
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
    ~JoinHandle() {
        if (m_state)
            m_state->cancelled.store(true, std::memory_order_relaxed);
    }

    // Detaches without cancelling. The task runs to completion; the result is discarded.
    // Consumes the JoinHandle — the caller can no longer synchronize or cancel.
    void detach() && {
        m_state.reset();
    }

    PollResult<T> poll(Context& ctx) {
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
        if (m_state)
            m_state->cancelled.store(true, std::memory_order_relaxed);
    }

    void detach() && {
        m_state.reset();
    }

    PollResult<void> poll(Context& ctx) {
        std::lock_guard lock(m_state->mutex);
        if (m_state->exception)
            return PollError(m_state->exception);
        if (m_state->done)
            return PollReady;
        m_state->join_waker = ctx.getWaker();
        return PollPending;
    }

private:
    std::shared_ptr<detail::TaskState<void>> m_state;
};

} // namespace coro
