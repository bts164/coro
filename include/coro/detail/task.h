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

/**
 * @brief Type-erased unit of async work owned and driven by the @ref Executor.
 *
 * `Task` wraps any type satisfying @ref Future behind a virtual `PollableBase` interface,
 * erasing the concrete future type so the executor can store heterogeneous tasks in a
 * single queue.
 *
 * Two construction modes:
 * - **Fire-and-forget** — no `TaskState`; result is discarded on completion.
 * - **Result-bearing** — holds a `shared_ptr<TaskState<T>>`; on completion writes the
 *   result (or exception) into the state and fires the join/scope wakers.
 *
 * **Cancellation:** if `TaskState::cancelled` is set (by `JoinHandle` destructor) before
 * `poll()` is called, `Task` calls `mark_done()` and returns `true` (complete) immediately
 * without polling the inner future.
 *
 * Users never construct `Task` directly — `SpawnBuilder::submit()` and
 * `Runtime::block_on()` create them internally.
 */
class Task {
public:
    /// @brief Fire-and-forget constructor. Result is discarded on completion.
    template<Future F>
    explicit Task(F future)
        : m_impl(std::make_unique<Pollable<F>>(std::move(future), nullptr)) {}

    /// @brief Result-bearing constructor. Writes outcome into `state` on completion.
    template<Future F>
    explicit Task(F future, std::shared_ptr<TaskState<typename F::OutputType>> state)
        : m_impl(std::make_unique<Pollable<F>>(std::move(future), std::move(state))) {}

    Task(const Task&)            = delete;
    Task& operator=(const Task&) = delete;

    // std::atomic is not movable, so we must provide explicit move operations
    // that load/store the atomic value rather than defaulting.
    Task(Task&& other) noexcept
        : scheduling_state(other.scheduling_state.load(std::memory_order_relaxed))
        , m_impl(std::move(other.m_impl))
    {}

    Task& operator=(Task&& other) noexcept {
        if (this != &other) {
            scheduling_state.store(
                other.scheduling_state.load(std::memory_order_relaxed),
                std::memory_order_relaxed);
            m_impl = std::move(other.m_impl);
        }
        return *this;
    }

    /**
     * @brief Atomic scheduling state. Managed exclusively by the executor and TaskWaker.
     *
     * Starts at Idle. schedule() sets it to Notified before the first enqueue.
     * After that all transitions are CAS operations — see SchedulingState for the
     * full transition table.
     */
    std::atomic<SchedulingState> scheduling_state{SchedulingState::Idle};

    /**
     * @brief Index of the worker that last ran this task, or -1 if unknown.
     *
     * Written by the WorkStealingExecutor worker loop after each poll() call,
     * while the task is in the Running state. Read by enqueue() to route
     * re-enqueued tasks back to the same worker's local queue (task affinity).
     * The Running → Idle CAS provides the happens-before edge that makes a
     * plain int safe here.
     */
    int last_worker_index = -1;

    /**
     * @brief Advances the inner future by one step.
     * @return `true` if the task has reached a terminal state (Ready, Error, or Dropped);
     *         `false` if still Pending and should be moved to the Suspended map.
     */
    bool poll(Context& ctx) { return m_impl->poll(ctx); }

private:
    /// @brief Non-template virtual base for type-erased polling.
    struct PollableBase {
        virtual ~PollableBase() = default;
        virtual bool poll(Context& ctx) = 0;
    };

    /// @brief Concrete template implementation holding the future and optional state.
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
