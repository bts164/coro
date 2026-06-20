#pragma once

#include <coro/runtime/executor.h>
#include <coro/task/join_handle.h>
#include <coro/detail/task.h>
#include <coro/detail/rc.h>
#include <memory>
#include <string>
#include <utility>

namespace coro {

/**
 * @brief Builder for configuring a background task before spawning it.
 *
 * Returned by `Runtime::build_task()` and the free `build_task()` function.
 * Configure the task with optional setters, then call `spawn(future)` or
 * `spawn(stream)` to enqueue it and receive a @ref JoinHandle or @ref StreamHandle.
 *
 * `[[nodiscard]]` — discarding the builder silently drops the configuration without
 * spawning anything.
 */
class [[nodiscard]] SpawnBuilder {
public:
    explicit SpawnBuilder(Executor* executor) : m_executor(executor) {}

    /// @brief Sets an optional name for the task (for diagnostics).
    SpawnBuilder& name(std::string n) {
        m_name = std::move(n);
        return *this;
    }

    /// @brief Sets the bounded queue capacity for stream tasks (default: 64 items).
    SpawnBuilder& buffer(std::size_t size) {
        m_buffer_size = size;
        return *this;
    }

    /// @brief Enqueues `future` as a background task and returns a @ref JoinHandle.
    template<Future F>
    JoinHandle<typename F::OutputType> spawn(F future) {
        using T = typename F::OutputType;
        auto impl = detail::make_rc<detail::TaskImpl<F>>(std::move(future));
        impl->name = std::move(m_name);
        detail::Rc<detail::TaskState<T>> state = impl;
        detail::Weak<detail::TaskBase> task_ref{impl};
#ifdef CORO_PICO
        impl->set_self(task_ref);
#endif
        if (m_executor)
            m_executor->schedule(detail::Rc<detail::TaskBase>(std::move(impl)));
        return JoinHandle<T>(std::move(state), std::move(task_ref));
    }

    /// @brief Creates a StreamTaskImpl, schedules it, and returns a @ref StreamHandle.
    ///
    /// One make_shared allocation combines TaskBase, StreamTaskState<T>, and the stream.
    /// StreamHandle receives an aliased shared_ptr<StreamTaskState<T>> into that allocation.
    template<Stream S>
    StreamHandle<typename S::ItemType> spawn(S stream) {
        using T = typename S::ItemType;
        auto impl = detail::make_rc<detail::StreamTaskImpl<S>>(std::move(stream), m_buffer_size);
        impl->name = std::move(m_name);
        detail::Rc<detail::StreamTaskState<T>> state = impl;
        detail::Weak<detail::TaskBase> task_ref{impl};
#ifdef CORO_PICO
        impl->set_self(task_ref);
#endif
        if (m_executor)
            m_executor->schedule(detail::Rc<detail::TaskBase>(std::move(impl)));
        return StreamHandle<T>(std::move(state), std::move(task_ref));
    }

private:
    Executor*   m_executor;
    std::string m_name;
    std::size_t m_buffer_size = 64;
};

} // namespace coro
