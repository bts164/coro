#pragma once

// Internal — not part of the public API.
// Type-erased, heap-allocated unit of work held by the executor.
// Users never construct a Task directly — spawn() creates one internally.

#include <coro/detail/context.h>
#include <coro/future.h>
#include <coro/detail/task_state.h>
#include <memory>
#include <type_traits>
#include <utility>

namespace coro::detail {

class Task {
public:
    // Fire-and-forget: result is discarded.
    template<Future F>
    explicit Task(F future)
        : m_impl(std::make_unique<Pollable<F>>(std::move(future), nullptr)) {}

    // Result-bearing: result is written to state on completion.
    template<Future F>
    explicit Task(F future, std::shared_ptr<TaskState<typename F::OutputType>> state)
        : m_impl(std::make_unique<Pollable<F>>(std::move(future), std::move(state))) {}

    Task(const Task&)            = delete;
    Task& operator=(const Task&) = delete;

    Task(Task&&) noexcept            = default;
    Task& operator=(Task&&) noexcept = default;

    // Returns true if the task completed (Ready or Error), false if still Pending.
    // The executor uses this to decide whether to suspend or discard the task.
    bool poll(Context& ctx) { return m_impl->poll(ctx); }

private:
    struct PollableBase {
        virtual ~PollableBase() = default;
        virtual bool poll(Context& ctx) = 0;
    };

    template<Future F>
    struct Pollable : PollableBase {
        using OutputType = typename F::OutputType;
        using StatePtr   = std::shared_ptr<TaskState<OutputType>>;

        Pollable(F f, StatePtr state)
            : m_future(std::move(f)), m_state(std::move(state)) {}

        bool poll(Context& ctx) override {
            if (m_completed) return true;
            if (m_state && m_state->cancelled.load(std::memory_order_relaxed)) {
                m_state->mark_done();
                return true;
            }

            auto result = m_future.poll(ctx);
            if (result.isPending()) return false;

            if (result.isDropped()) {
                if (m_state) m_state->mark_done();
                return true;
            }

            m_completed = true;
            if (!m_state) return true;

            if (result.isError()) {
                m_state->setException(result.error());
            } else {
                if constexpr (std::is_void_v<OutputType>)
                    m_state->setDone();
                else
                    m_state->setResult(std::move(result).value());
            }
            return true;
        }

        F         m_future;
        StatePtr  m_state;
        bool      m_completed = false;
    };

    std::unique_ptr<PollableBase> m_impl;
};

} // namespace coro::detail
