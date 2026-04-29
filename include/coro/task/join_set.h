#pragma once

#include <coro/detail/context.h>
#include <coro/detail/poll_result.h>
#include <coro/detail/task.h>
#include <coro/detail/task_state.h>
#include <coro/detail/waker.h>
#include <coro/future.h>
#include <coro/runtime/runtime.h>
#include <exception>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <type_traits>
#include <utility>
#include <vector>

namespace coro {

namespace detail {

// -----------------------------------------------------------------------
// JoinSetSharedState<T>
// -----------------------------------------------------------------------

/**
 * @brief Shared state for a JoinSet<T>.
 *
 * - `pending_handles` — strong references to tasks still running. Serves as
 *   the sole persistent lifetime anchor for each task while it is Idle (parked
 *   between executor polls). When a task completes, it removes itself from
 *   pending and moves to idle inside on_task_complete().
 *
 * - `idle_handles` — aliased shared_ptr<TaskState<T>> for tasks that have
 *   reached a terminal state. The consumer reads results directly from
 *   TaskState<T>, avoiding any extra result copy.
 *
 * JoinSetTask holds a weak_ptr<JoinSetSharedState> to break the cycle:
 *   JoinSetSharedState::pending_handles → TaskBase
 *                                       → (weak) → JoinSetSharedState
 */
template<typename T>
struct JoinSetSharedState {
    std::mutex                               mutex;
    std::shared_ptr<Waker>                   consumer_waker;
    std::set<std::shared_ptr<TaskBase>>      pending_handles; ///< Running tasks (lifetime anchors).
    std::list<std::shared_ptr<TaskState<T>>> idle_handles;    ///< Completed tasks awaiting consumption.
};

// -----------------------------------------------------------------------
// JoinSetTask<F>
// -----------------------------------------------------------------------

/**
 * @brief Concrete task for JoinSet::spawn(). A single allocation covers the
 * executor-facing TaskBase, the result-holding TaskState<T>, and the JoinSet
 * tracking data — no separate JoinHandle or handle wrapper is needed.
 *
 * Inherits TaskImpl<F> for all inner-future polling, the full cancellation
 * protocol, and result/exception/mark_done delivery. Overrides
 * on_task_complete() to move itself from pending to idle in the owning
 * JoinSet's shared state and wake the consumer.
 *
 * Uses weak_ptr<JoinSetSharedState> to avoid a reference cycle between the
 * shared state (which owns pending_handles → this task) and the task itself.
 * When JoinSet is destroyed, lock() returns null and on_task_complete() is a
 * silent no-op; the executor's temporary strong reference keeps the task alive
 * through the remainder of poll().
 */
template<Future F>
class JoinSetTask : public TaskImpl<F> {
    using T = typename F::OutputType;
public:
    JoinSetTask(F future, std::weak_ptr<JoinSetSharedState<T>> set_state)
        : TaskImpl<F>(std::move(future))
        , m_set_state(std::move(set_state)) {}

    void on_task_complete() noexcept override {
        auto state = m_set_state.lock();
        if (!state) return;  // JoinSet was destroyed — nothing to notify

        // Build an aliased shared_ptr<TaskState<T>> from shared_from_this().
        // Both share the same control block (same allocation), but the stored
        // pointer is the TaskState<T> subobject. The consumer uses this to
        // read the result directly, the same way JoinHandle::poll() would.
        auto self_base = this->shared_from_this();  // shared_ptr<TaskBase>
        std::shared_ptr<TaskState<T>> self_state(self_base, static_cast<TaskState<T>*>(this));

        std::shared_ptr<Waker> waker;
        {
            std::lock_guard lock(state->mutex);
            state->pending_handles.erase(self_base);
            state->idle_handles.push_back(std::move(self_state));
            waker = std::exchange(state->consumer_waker, nullptr);
        }
        if (waker) waker->wake();
    }

private:
    std::weak_ptr<JoinSetSharedState<T>> m_set_state;
};

// -----------------------------------------------------------------------
// JoinSetDrainFuture<T>
// -----------------------------------------------------------------------

/**
 * @brief Future<void> returned by JoinSet::drain().
 *
 * Polls until all pending tasks have completed or been cancelled. Each poll
 * sweeps idle_handles (discarding values, capturing the first exception) and
 * returns Pending if any tasks are still running. Rethrows the first exception
 * after pending_handles reaches zero.
 */
template<typename T>
class JoinSetDrainFuture {
public:
    using OutputType = void;

    explicit JoinSetDrainFuture(std::shared_ptr<JoinSetSharedState<T>> state)
        : m_state(std::move(state)) {}

    JoinSetDrainFuture(JoinSetDrainFuture&&) noexcept = default;

    PollResult<void> poll(detail::Context& ctx) {
        std::list<std::shared_ptr<TaskState<T>>> to_process;
        bool any_pending;
        {
            std::lock_guard lock(m_state->mutex);
            std::swap(m_state->idle_handles, to_process);
            any_pending = !m_state->pending_handles.empty();
            if (any_pending)
                m_state->consumer_waker = ctx.getWaker()->clone();
        }

        // Process completed tasks outside the lock — collect first exception, discard values.
        for (auto& ts : to_process) {
            if (!m_first_exception) {
                std::lock_guard tlock(ts->mutex);
                if (ts->exception) m_first_exception = ts->exception;
            }
        }
        // to_process destructs here, releasing task allocations.

        if (any_pending) return PollPending;

        // No pending tasks at snapshot time. Re-check: a task may have completed
        // and moved to idle between our swap and here.
        {
            std::lock_guard lock(m_state->mutex);
            if (!m_state->pending_handles.empty() || !m_state->idle_handles.empty()) {
                m_state->consumer_waker = ctx.getWaker()->clone();
                return PollPending;
            }
        }

        if (m_first_exception) return PollError(std::move(m_first_exception));
        return PollReady;
    }

private:
    std::shared_ptr<JoinSetSharedState<T>> m_state;
    std::exception_ptr                     m_first_exception;
};

} // namespace detail


// -----------------------------------------------------------------------
// JoinSetSpawnBuilder<T, F>
// -----------------------------------------------------------------------

/**
 * @brief Builder returned by `JoinSet<T>::build_task()`. Mirrors Tokio's
 * `JoinSet::build_task()` interface.
 *
 * Call `.name("...")` to set a human-readable task name, then `.spawn()` to
 * submit the task. `[[nodiscard]]` — discarding the builder without calling
 * `.spawn()` silently drops the future without scheduling it.
 */
template<typename T, Future F>
    requires std::same_as<typename F::OutputType, T>
class [[nodiscard]] JoinSetSpawnBuilder {
public:
    JoinSetSpawnBuilder(F future, std::shared_ptr<detail::JoinSetSharedState<T>> state)
        : m_future(std::move(future)), m_state(std::move(state)) {}

    JoinSetSpawnBuilder(JoinSetSpawnBuilder&&) noexcept            = default;
    JoinSetSpawnBuilder(const JoinSetSpawnBuilder&)                = delete;
    JoinSetSpawnBuilder& operator=(const JoinSetSpawnBuilder&)     = delete;
    JoinSetSpawnBuilder& operator=(JoinSetSpawnBuilder&&) noexcept = default;

    /// @brief Sets a human-readable name visible in diagnostics.
    JoinSetSpawnBuilder& name(std::string n) & {
        m_name = std::move(n);
        return *this;
    }

    JoinSetSpawnBuilder&& name(std::string n) && {
        m_name = std::move(n);
        return std::move(*this);
    }

    /// @brief Schedules the future as a child task in the owning `JoinSet`.
    /// Must be called exactly once. Consumes the builder.
    void spawn() && {
        auto task = std::make_shared<detail::JoinSetTask<F>>(
            std::move(m_future),
            std::weak_ptr<detail::JoinSetSharedState<T>>(m_state));
        task->name = std::move(m_name);
        std::shared_ptr<detail::TaskBase> task_base(task);
        {
            std::lock_guard lock(m_state->mutex);
            m_state->pending_handles.insert(task_base);
        }
        coro::current_runtime().schedule_task(std::move(task_base));
    }

private:
    F                                               m_future;
    std::shared_ptr<detail::JoinSetSharedState<T>>  m_state;
    std::string                                     m_name;
};


// -----------------------------------------------------------------------
// JoinSet<T> — public API
// -----------------------------------------------------------------------

/**
 * @brief Structured-concurrency set for spawning and collecting homogeneous child tasks.
 *
 * All tasks spawned into a `JoinSet<T>` must produce values of type `T`. Results
 * are delivered in completion order via `next()` or discarded via `drain()`.
 * `JoinSet<T>` satisfies `Stream<T>`.
 *
 * **Allocation model:** each `spawn()` creates a single `JoinSetTask<F>` allocation
 * that covers the executor-facing `TaskBase`, the result-holding `TaskState<T>`, and
 * the JoinSet tracking data. When the task completes it moves itself from the pending
 * to idle queue; the consumer reads the result directly from `TaskState<T>` without
 * an extra copy or intermediate handle.
 *
 * **Cancel on drop:** destroying a `JoinSet` cancels all pending tasks and drops the
 * persistent strong references. Tasks that are currently Idle are woken via
 * `cancel_task()` so the executor picks them up, observes `cancelled = true`, and
 * drains them; tasks that are Running or Notified are already held by the executor's
 * temporary reference and will self-terminate on their next poll.
 *
 * @tparam T The value type produced by spawned tasks. Use `JoinSet<void>` for tasks
 *           that produce no value.
 */
template<typename T>
class [[nodiscard]] JoinSet {
public:
    using ItemType     = T;
    using OptionalType = std::conditional_t<std::is_void_v<T>, bool, std::optional<T>>;

    JoinSet() : m_state(std::make_shared<detail::JoinSetSharedState<T>>()) {}

    JoinSet(const JoinSet&)            = delete;
    JoinSet& operator=(const JoinSet&) = delete;
    JoinSet(JoinSet&&) noexcept        = default;

    JoinSet& operator=(JoinSet&& other) noexcept {
        if (this != &other) {
            cancel_pending();
            m_state = std::move(other.m_state);
        }
        return *this;
    }

    ~JoinSet() { cancel_pending(); }

    /**
     * @brief Spawns `future` as a child task. The result is delivered via `next()` or `drain()`.
     *
     * May only be called from within a `Runtime::block_on()` context.
     * Use `build_task(future).name("...").spawn()` to set a task name.
     */
    template<Future F>
        requires std::same_as<typename F::OutputType, T>
    void spawn(F future) {
        build_task(std::move(future)).spawn();
    }

    /**
     * @brief Returns a builder for configuring and spawning a child task.
     *
     * Mirrors Tokio's `JoinSet::build_task()`. Call `.name("...")` to attach a
     * human-readable name, then `.spawn()` to submit. `[[nodiscard]]` — discarding
     * the builder without calling `.spawn()` silently drops the future.
     *
     * May only be called from within a `Runtime::block_on()` context.
     */
    template<Future F>
        requires std::same_as<typename F::OutputType, T>
    [[nodiscard]] JoinSetSpawnBuilder<T, F> build_task(F future) {
        return JoinSetSpawnBuilder<T, F>(std::move(future), m_state);
    }

    /**
     * @brief Satisfies `Stream<T>`. Returns the next completed result, or end-of-stream
     * when all tasks have finished. Rethrows a child's exception when dequeued.
     *
     * If a task was cancelled before producing a result, returns `PollDropped`.
     * Use via `co_await next(js)` rather than calling directly.
     */
    PollResult<OptionalType> poll_next(detail::Context& ctx) {
        std::unique_lock lock(m_state->mutex);

        if (!m_state->idle_handles.empty()) {
            auto task_state = std::move(m_state->idle_handles.front());
            m_state->idle_handles.pop_front();
            lock.unlock();

            // Read result directly from TaskState — same logic as JoinHandle::poll().
            std::lock_guard tlock(task_state->mutex);
            if (task_state->exception)
                return PollError(task_state->exception);
            if constexpr (std::is_void_v<T>) {
                if (task_state->result)
                    return PollResult<OptionalType>(true);
            } else {
                if (task_state->result.has_value())
                    return PollResult<OptionalType>(std::move(*task_state->result));
            }
            return PollDropped;  // task was cancelled before producing a result
        }

        if (!m_state->pending_handles.empty()) {
            m_state->consumer_waker = ctx.getWaker()->clone();
            return PollPending;
        }

        // Stream exhausted — no pending or idle tasks remain.
        if constexpr (std::is_void_v<T>)
            return PollResult<OptionalType>(false);
        else
            return PollResult<OptionalType>(std::nullopt);
    }

    /**
     * @brief Returns a `Future<void>` that completes once all spawned tasks finish.
     *
     * Result values are discarded. The first exception encountered is rethrown after
     * all tasks complete. `[[nodiscard]]` — discarding it skips the wait entirely.
     */
    [[nodiscard]] detail::JoinSetDrainFuture<T> drain() {
        return detail::JoinSetDrainFuture<T>{m_state};
    }

private:
    // Marks all pending tasks cancelled and drops the persistent strong references.
    // Tasks that are Idle are woken by cancel_task() so the executor observes
    // cancelled=true and drains them. Running/Notified tasks are already held by the
    // executor's temporary reference and self-terminate on the next poll.
    void cancel_pending() {
        if (!m_state) return;
        std::vector<std::shared_ptr<detail::TaskBase>> to_cancel;
        {
            std::lock_guard lock(m_state->mutex);
            to_cancel.reserve(m_state->pending_handles.size());
            for (auto& h : m_state->pending_handles)
                to_cancel.push_back(h);
            m_state->pending_handles.clear();  // drop persistent refs
        }
        // Call cancel_task() while to_cancel still holds strong refs — prevents
        // premature deallocation of Idle tasks between the ref drop and the wake.
        for (auto& task : to_cancel)
            task->cancel_task();
        // to_cancel destructs here; tasks are now Notified (executor holds temp ref).
    }

    std::shared_ptr<detail::JoinSetSharedState<T>> m_state;
};

} // namespace coro
