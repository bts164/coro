#pragma once

#include <coro/detail/context.h>
#include <coro/detail/poll_result.h>
#include <coro/detail/task.h>
#include <coro/detail/task_state.h>
#include <coro/detail/waker.h>
#include <coro/future.h>
#include <coro/runtime/runtime.h>
#include <coro/task/join_handle.h>
#include <exception>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <type_traits>
#include <utility>
#include <variant>

namespace coro {

namespace detail {

// -----------------------------------------------------------------------
// JoinSetSharedState<T>
// Shared between JoinSet, every JoinSetTask, and any live DrainFuture.
// -----------------------------------------------------------------------

/**
 * @brief Shared state for a `JoinSet<T>` (non-void specialisation).
 *
 * All fields are protected by `mutex`. The two handle lists track task
 * lifetimes without requiring an O(n) scan to find completed entries:
 *
 * - `pending_handles` — handles for tasks still running. Each `JoinSetTask`
 *   holds a `std::list` iterator into this list (stable under splicing).
 * - `done_handles`    — handles for tasks that completed or were cancelled,
 *   waiting to be destroyed. `JoinHandle` destructors touch other locks
 *   (atomics, `CoroutineScope`) so they are deferred until the next sweep
 *   point, where they are moved out and destroyed after the mutex is released.
 *
 * @tparam T The value type produced by spawned tasks.
 */
template<typename T>
struct JoinSetSharedState {
    std::mutex                                           mutex;
    std::queue<std::variant<T, std::exception_ptr>>     results;
    std::size_t                                          pending_count  = 0;
    std::shared_ptr<Waker>                               consumer_waker;
    std::list<JoinHandle<void>>                          pending_handles;
    std::list<JoinHandle<void>>                          done_handles;
};

/**
 * @brief Shared state for a `JoinSet<void>`.
 *
 * Only exceptions are queued; successful void completions merely decrement
 * `pending_count`. Handle tracking is identical to the non-void specialisation.
 */
template<>
struct JoinSetSharedState<void> {
    std::mutex                       mutex;
    std::queue<std::exception_ptr>   exceptions;
    std::size_t                      pending_count  = 0;
    std::shared_ptr<Waker>           consumer_waker;
    std::list<JoinHandle<void>>      pending_handles;
    std::list<JoinHandle<void>>      done_handles;
};

using HandleIter = std::list<JoinHandle<void>>::iterator;


// -----------------------------------------------------------------------
// JoinSetTask<T, F>
// -----------------------------------------------------------------------

/**
 * @brief Internal `Future<void>` wrapper that routes a user future's result
 * into the owning `JoinSet`'s shared state.
 *
 * Holds a `std::list` iterator into `JoinSetSharedState::pending_handles`.
 * On completion (`poll()`) or cancellation (destructor), the handle is spliced
 * O(1) from `pending_handles` to `done_handles` under the mutex. The handle is
 * destroyed later, at the next sweep point, outside the lock.
 *
 * The default move constructor leaves `m_state` null in the moved-from object,
 * which the destructor uses as a sentinel to skip cleanup — preventing a double
 * splice if the object is moved before being polled.
 *
 * @tparam T  Value type of the owning `JoinSet` (may be `void`).
 * @tparam F  The concrete `Future<T>` type being wrapped.
 */
template<typename T, typename F>
class JoinSetTask {
public:
    using OutputType = void;

    JoinSetTask(F future,
                std::shared_ptr<JoinSetSharedState<T>> state,
                HandleIter handle_iter)
        : m_future(std::move(future))
        , m_state(std::move(state))
        , m_handle_iter(handle_iter) {}

    JoinSetTask(JoinSetTask&&) noexcept = default;

    ~JoinSetTask() {
        // Moved-from (m_state null) or already completed — nothing to do.
        if (!m_state || m_completed) return;
        // Cancellation path: task destroyed before completing.
        std::shared_ptr<Waker> waker;
        {
            std::lock_guard lock(m_state->mutex);
            if (m_state->pending_count > 0)
                --m_state->pending_count;
            m_state->done_handles.splice(m_state->done_handles.end(),
                                         m_state->pending_handles,
                                         m_handle_iter);
            waker = std::move(m_state->consumer_waker);
        }
        if (waker) waker->wake();
    }

    PollResult<void> poll(detail::Context& ctx) {
        auto result = m_future.poll(ctx);
        if (result.isPending()) return PollPending;

        std::shared_ptr<Waker> waker;
        {
            std::lock_guard lock(m_state->mutex);
            --m_state->pending_count;

            if (result.isError()) {
                if constexpr (std::is_void_v<T>)
                    m_state->exceptions.push(result.error());
                else
                    m_state->results.push(result.error());
            } else if (!result.isDropped()) {
                if constexpr (!std::is_void_v<T>)
                    m_state->results.push(std::move(result).value());
            }

            m_state->done_handles.splice(m_state->done_handles.end(),
                                         m_state->pending_handles,
                                         m_handle_iter);
            waker = std::move(m_state->consumer_waker);
        }
        m_completed = true;
        if (waker) waker->wake();
        return PollReady;
    }

private:
    F                                      m_future;
    std::shared_ptr<JoinSetSharedState<T>> m_state;
    HandleIter                             m_handle_iter;
    bool                                   m_completed = false;
};


// -----------------------------------------------------------------------
// JoinSetDrainFuture<T>
// -----------------------------------------------------------------------

/**
 * @brief `Future<void>` returned by `JoinSet::drain()`.
 *
 * Polls until all pending tasks have completed or been cancelled. On each
 * poll, sweeps `done_handles` (destroying them after releasing the lock) and
 * drains the results queue — discarding values, capturing the first exception.
 * Rethrows the first exception after `pending_count` reaches zero.
 *
 * @tparam T Value type of the owning `JoinSet` (may be `void`).
 */
template<typename T>
class JoinSetDrainFuture {
public:
    using OutputType = void;

    explicit JoinSetDrainFuture(std::shared_ptr<JoinSetSharedState<T>> state)
        : m_state(std::move(state)) {}

    JoinSetDrainFuture(JoinSetDrainFuture&&) noexcept = default;

    PollResult<void> poll(detail::Context& ctx) {
        std::list<JoinHandle<void>> to_destroy;
        {
            std::lock_guard lock(m_state->mutex);
            to_destroy = std::move(m_state->done_handles);

            if constexpr (std::is_void_v<T>) {
                while (!m_state->exceptions.empty()) {
                    auto e = std::move(m_state->exceptions.front());
                    m_state->exceptions.pop();
                    if (!m_first_exception) m_first_exception = std::move(e);
                }
            } else {
                while (!m_state->results.empty()) {
                    auto item = std::move(m_state->results.front());
                    m_state->results.pop();
                    if (!m_first_exception &&
                            std::holds_alternative<std::exception_ptr>(item))
                        m_first_exception = std::get<std::exception_ptr>(item);
                }
            }

            if (m_state->pending_count > 0) {
                m_state->consumer_waker = ctx.getWaker()->clone();
                // Return inside the lock — lock_guard releases before to_destroy
                // destructs (to_destroy is in the outer function scope).
                return PollPending;
            }
        }
        // to_destroy destructs here, outside the lock.
        if (m_first_exception) return PollError(std::move(m_first_exception));
        return PollReady;
    }

private:
    std::shared_ptr<JoinSetSharedState<T>> m_state;
    std::exception_ptr                     m_first_exception;
};


// -----------------------------------------------------------------------
// spawn_into_join_set — shared spawn logic for both JoinSet specialisations
// -----------------------------------------------------------------------

// Creates a TaskState, inserts the JoinHandle into pending_handles, sweeps
// done_handles, and schedules the JoinSetTask — all without holding the lock
// while destructors or the scheduler run.
template<typename T, typename F>
void spawn_into_join_set(F future, std::shared_ptr<JoinSetSharedState<T>>& state) {
    std::list<JoinHandle<void>> to_destroy;
    HandleIter iter;

    // Pre-create the TaskState so we can build a JoinHandle before scheduling.
    auto task_state = std::make_shared<TaskState<void>>();

    {
        std::lock_guard lock(state->mutex);
        to_destroy = std::move(state->done_handles);
        ++state->pending_count;
        state->pending_handles.push_back(JoinHandle<void>(task_state));
        iter = std::prev(state->pending_handles.end());
    }
    // to_destroy destructs here — JoinHandle destructors fire outside the lock.

    using TaskT = JoinSetTask<T, F>;
    coro::current_runtime().schedule_task(
        std::make_unique<Task>(
            TaskT{std::move(future), state, iter},
            std::move(task_state)));
}

} // namespace detail


// -----------------------------------------------------------------------
// JoinSet<T> — public API
// -----------------------------------------------------------------------

/**
 * @brief Structured-concurrency set for spawning and collecting homogeneous child tasks.
 *
 * All tasks spawned into a `JoinSet<T>` must produce values of type `T`. Results are
 * delivered in completion order. `JoinSet<T>` satisfies `Stream<T>`, making it composable
 * with `next()`, `select`, and other stream combinators.
 *
 * **Handle lifecycle:** each spawned task's `JoinHandle<void>` lives in `pending_handles`
 * until the task finishes, then is spliced O(1) to `done_handles`. At the next sweep
 * point (`poll_next()`, `spawn()`, or `drain()` poll), done handles are destroyed outside
 * the shared-state lock.
 *
 * **Cancel on drop:** dropping a `JoinSet` cancels all pending tasks. When dropped inside
 * a coroutine, the enclosing `CoroutineScope` ensures tasks drain before the coroutine
 * completes — use inside `co_invoke` to guarantee reference safety.
 *
 * **Exception handling:**
 * - `next()`: rethrows a child's exception when that result is dequeued.
 * - `drain()`: waits for all tasks; rethrows the first exception after all finish.
 *
 * @tparam T The value type produced by spawned tasks. For `void` tasks use `JoinSet<void>`.
 */
template<typename T>
class [[nodiscard]] JoinSet {
public:
    using ItemType = T;

    JoinSet() : m_state(std::make_shared<detail::JoinSetSharedState<T>>()) {}

    JoinSet(const JoinSet&)            = delete;
    JoinSet& operator=(const JoinSet&) = delete;
    JoinSet(JoinSet&&) noexcept            = default;
    JoinSet& operator=(JoinSet&&) noexcept = default;

    /**
     * @brief Spawns `future` as a child task. The result is delivered via `next()` or `drain()`.
     *
     * Inserts a `JoinHandle<void>` into `pending_handles`, passes its iterator to the
     * `JoinSetTask`, and sweeps any accumulated `done_handles` before scheduling.
     *
     * May only be called from within a `Runtime::block_on()` context.
     */
    template<Future F>
        requires std::same_as<typename F::OutputType, T>
    void spawn(F future) {
        detail::spawn_into_join_set<T>(std::move(future), m_state);
    }

    /**
     * @brief Satisfies `Stream<T>`. Returns the next completed result, or `nullopt` when
     * all tasks have finished. Rethrows a child's exception immediately when dequeued.
     * Sweeps `done_handles` on each call.
     *
     * Use via `co_await next(js)` rather than calling directly.
     */
    PollResult<std::optional<T>> poll_next(detail::Context& ctx) {
        std::list<JoinHandle<void>> to_destroy;
        {
            std::lock_guard lock(m_state->mutex);
            to_destroy = std::move(m_state->done_handles);

            if (!m_state->results.empty()) {
                auto item = std::move(m_state->results.front());
                m_state->results.pop();
                if (std::holds_alternative<std::exception_ptr>(item))
                    return PollError(std::get<std::exception_ptr>(item));
                return std::optional<T>(std::move(std::get<T>(item)));
            }

            if (m_state->pending_count == 0)
                return std::optional<T>(std::nullopt);

            m_state->consumer_waker = ctx.getWaker()->clone();
            return PollPending;
        }
        // to_destroy destructs here for any early return above:
        // lock_guard releases first (inner scope), then to_destroy destructs
        // as part of the function return sequence (outer scope) — outside the lock.
    }

    /**
     * @brief Returns a `Future<void>` that completes once all spawned tasks finish.
     *
     * Result values are discarded. The first exception encountered is rethrown after all
     * tasks complete. `[[nodiscard]]` — discarding it skips the wait entirely.
     */
    [[nodiscard]] detail::JoinSetDrainFuture<T> drain() {
        return detail::JoinSetDrainFuture<T>{m_state};
    }

private:
    std::shared_ptr<detail::JoinSetSharedState<T>> m_state;
};


/**
 * @brief `JoinSet` specialisation for `void`-producing tasks.
 *
 * Does not satisfy `Stream` (void items carry no information); only `spawn()` and `drain()`
 * are provided. `drain()` waits for all tasks and rethrows the first exception.
 * Handle lifecycle (pending/done lists, O(1) splice, deferred sweep) is identical to the
 * primary template.
 */
template<>
class [[nodiscard]] JoinSet<void> {
public:
    JoinSet() : m_state(std::make_shared<detail::JoinSetSharedState<void>>()) {}

    JoinSet(const JoinSet&)            = delete;
    JoinSet& operator=(const JoinSet&) = delete;
    JoinSet(JoinSet&&) noexcept            = default;
    JoinSet& operator=(JoinSet&&) noexcept = default;

    /**
     * @brief Spawns a `Future<void>` child task.
     *
     * May only be called from within a `Runtime::block_on()` context.
     */
    template<Future F>
        requires std::same_as<typename F::OutputType, void>
    void spawn(F future) {
        detail::spawn_into_join_set<void>(std::move(future), m_state);
    }

    /**
     * @brief Returns a `Future<void>` that completes once all spawned tasks finish.
     *
     * Rethrows the first exception (if any) after all tasks complete.
     */
    [[nodiscard]] detail::JoinSetDrainFuture<void> drain() {
        return detail::JoinSetDrainFuture<void>{m_state};
    }

private:
    std::shared_ptr<detail::JoinSetSharedState<void>> m_state;
};

} // namespace coro
