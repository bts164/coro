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
#include <set>
#include <type_traits>
#include <utility>
#include <variant>

namespace coro {

namespace detail {

template<typename T>
struct JoinSetTaskHandle;

// -----------------------------------------------------------------------
// JoinSetSharedState<T>
// Shared between JoinSet, every JoinSetTask, and any live DrainFuture.
// -----------------------------------------------------------------------

// OWNERSHIP AND REFERENCE CYCLES
//
// While a child task is pending, the following shared_ptr graph exists:
//
//   JoinSet::m_state ──────────────────────────────────► JoinSetSharedState
//                                                              │
//                                            pending_handles   │  (std::set)
//                                                              ▼
//   TaskState::join_waker ─────────────────────────► JoinSetTaskHandle
//        │                                                  │     │
//        │                                          m_state │     │ m_handle
//        │                                                  │     ▼
//        │                                                  │  JoinHandle
//        │                                                  │     │ m_state
//        │                                                  │     ▼
//        └──────────────────────────────────────────── TaskState ◄┘
//
// Two interlocking cycles are present while the task is live:
//
//   Cycle A (task ↔ handle):
//     JoinSetTaskHandle → (m_handle) → JoinHandle → TaskState
//     TaskState → (join_waker) → JoinSetTaskHandle
//
//   Cycle B (handle ↔ shared state):
//     JoinSetSharedState → (pending_handles) → JoinSetTaskHandle
//     JoinSetTaskHandle → (m_state) → JoinSetSharedState
//
// HOW THE CYCLES ARE BROKEN
//
//   Normal completion (task finishes):
//     TaskState::setResult/setDone/setException() moves join_waker out, breaking
//     Cycle A.  JoinSetTaskHandle::wake() then moves m_handle out and calls
//     pending_handles.erase(self), breaking Cycle B.
//
//   Cancellation (JoinSet::cancel_pending() / ~JoinSet):
//     take_handle() moves m_handle out of each JoinSetTaskHandle (breaking Cycle A),
//     then pending_handles.clear() drops all handle refs (breaking Cycle B).
//     The extracted JoinHandles are destroyed outside the lock; their destructors
//     set cancelled=true on TaskState.  When the executor eventually polls those
//     tasks, it sees cancelled=true, calls mark_done(), which moves join_waker out
//     (fires JoinSetTaskHandle::wake(), which is a no-op since m_handle_taken=true),
//     and frees the remaining chain.
//
// WHAT NOT TO DO
//   Do NOT store the drain waker (or any TaskWaker pointing back to the parent
//   Task) inside an object that is itself owned by that Task.  CoroutineScope
//   previously kept a `m_drain_waker` member for this reason — it created a
//   self-referential Task→Coro→CoroutineScope→TaskWaker→Task cycle that prevented
//   the Task from ever being freed.  See coro_scope.h for the authoritative note.

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
    std::mutex                                        mutex;
    std::shared_ptr<Waker>                            consumer_waker;
    std::set<std::shared_ptr<JoinSetTaskHandle<T>>>   pending_handles;
    std::list<PollResult<T>>                          results;
    // Set by JoinSet destructor/move-assignment before clearing pending_handles.
    // wake() checks this to avoid terminate() when the JoinSet has been dropped.
    bool                                              closing{false};
};

template<typename T>
struct JoinSetTaskHandle : public std::enable_shared_from_this<JoinSetTaskHandle<T>>, public Waker
{
    JoinSetTaskHandle(JoinHandle<T> handle, std::shared_ptr<JoinSetSharedState<T>> state) :
        m_handle(std::move(handle)),
        m_state(state)
    {}
    JoinSetTaskHandle(JoinSetTaskHandle const &) = delete;
    JoinSetTaskHandle& operator=(JoinSetTaskHandle const &) = delete;
    ~JoinSetTaskHandle() override = default;

    std::shared_ptr<Waker> clone() override
    {
        return this->shared_from_this();
    }

    // Called from JoinSet::cancel_pending() while holding m_state->mutex.
    // Extracts the JoinHandle so its destructor can run (cancel + scope registration)
    // outside the lock, and sets m_handle_taken to suppress any racing wake().
    //
    // Lock order: m_state->mutex → m_handle_mutex (never reversed).
    JoinHandle<T> take_handle()
    {
        std::lock_guard hlock(m_handle_mutex);
        m_handle_taken = true;
        return std::move(m_handle);
    }

    void wake() override
    {
        auto self = this->shared_from_this();
        // Guard against concurrent take_handle() in JoinSet destructor/move-assign.
        // Poll under m_handle_mutex so take_handle() cannot race on m_handle.
        // Lock order: m_handle_mutex first, m_state->mutex second (released before acquiring).
        std::optional<JoinHandle<T>> handle;
        PollResult<T> result = PollPending;
        {
            std::lock_guard hlock(m_handle_mutex);
            if (std::exchange(m_handle_taken, true)) return;
            handle = std::move(m_handle);
        }
        detail::Context ctx(self);
        result = handle->poll(ctx);
        if (result.isPending()) {
            std::terminate();
        }
        std::move(handle).value().detach();
        std::unique_lock lock(m_state->mutex);
        // JoinSet was dropped between our poll and acquiring m_state->mutex.
        if (m_state->closing) return;
        if (0 == m_state->pending_handles.erase(self)) {
            std::terminate();
        }
        m_state->results.emplace_back(std::move(result));
        auto waker = std::exchange(m_state->consumer_waker, nullptr);
        lock.unlock();
        if (waker) {
            waker->wake();
        }
    }

    JoinHandle<T>                          m_handle;
    std::shared_ptr<JoinSetSharedState<T>> m_state;
    // Protects m_handle against concurrent wake() and take_handle().
    std::mutex                             m_handle_mutex;
    bool                                   m_handle_taken{false};
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
        std::size_t pending_count = 0;
        std::list<PollResult<T>> results;
        {
            std::lock_guard lock(m_state->mutex);
            std::swap(m_state->results, results);
            pending_count = m_state->pending_handles.size();
            if (pending_count > 0) {
                m_state->consumer_waker = ctx.getWaker()->clone();
            }
        }
        for (auto &result : results) {
            if (m_first_exception) {
                break;
            }
            if (result.isError()) {
                m_first_exception = result.error();
                break;
            }
        }
        if (pending_count > 0) {
            return PollPending;
        }
        // to_destroy destructs here, outside the lock.
        if (m_first_exception) return PollError(std::move(m_first_exception));
        return PollReady;
    }

private:
    std::shared_ptr<JoinSetSharedState<T>> m_state;
    std::exception_ptr                     m_first_exception;
};

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
    using OptionalType = std::conditional_t<
        std::is_void_v<T>,
        bool, std::optional<T>>;

    JoinSet() : m_state(std::make_shared<detail::JoinSetSharedState<T>>()) {}

    JoinSet(const JoinSet&)            = delete;
    JoinSet& operator=(const JoinSet&) = delete;
    JoinSet(JoinSet&&) noexcept            = default;

    // Custom move-assignment: cancel pending tasks on the old state before overwriting.
    JoinSet& operator=(JoinSet&& other) noexcept {
        if (this != &other) {
            cancel_pending();
            m_state = std::move(other.m_state);
        }
        return *this;
    }

    // Cancels all pending tasks and registers them with the enclosing CoroutineScope
    // (if any) so the scope drains them before the parent coroutine completes.
    ~JoinSet() { cancel_pending(); }

    /**
     * @brief Spawns `future` as a child task. The result is delivered via `next()` or `drain()`.
     *
     * Inserts a `JoinHandle<void>` into `pending_handles`, passes its iterator to the
     * `JoinSetTask`, and sweeps any accumulated `done_handles` before scheduling.
     *
     * May only be called from within a `Runtime::block_on()` context.
     */
    void add(JoinHandle<T> join_handle) {
        // Pre-create the TaskState so we can build a JoinHandle before scheduling.
        // to_destroy destructs here — JoinHandle destructors fire outside the lock.
        auto join_set_handle = std::make_shared<detail::JoinSetTaskHandle<T>>(
            std::move(join_handle), m_state);
        {
            std::lock_guard lock(m_state->mutex);
            m_state->pending_handles.insert(join_set_handle);
        }
        // now poll the join_handle to either move it to the idle queue now, or register the
        // set_handle as the waker for the join_handle so it moves it upon completion
        detail::Context ctx(join_set_handle);
        auto result = join_set_handle->m_handle.poll(ctx);
        if (!result.isPending()) {
            std::lock_guard lock(m_state->mutex);
            if (0 == m_state->pending_handles.erase(join_set_handle)) {
                std::terminate();
            }
            m_state->results.emplace_back(std::move(result));
        }
    }

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
        // Pre-create the TaskState so we can build a JoinHandle before scheduling.
        // to_destroy destructs here — JoinHandle destructors fire outside the lock.
        auto task_state = std::make_shared<detail::TaskState<T>>();
        auto task = std::make_unique<detail::Task>(std::move(future), task_state);
        coro::current_runtime().schedule_task(std::move(task));
        add(JoinHandle<T>(task_state));
    }

    /**
     * @brief Satisfies `Stream<T>`. Returns the next completed result, or `nullopt` when
     * all tasks have finished. Rethrows a child's exception immediately when dequeued.
     * Sweeps `done_handles` on each call.
     *
     * Use via `co_await next(js)` rather than calling directly.
     */
    PollResult<OptionalType> poll_next(detail::Context& ctx) {
        std::unique_lock lock(m_state->mutex);
        if (m_state->results.empty()) {
            if (m_state->pending_handles.empty()) {
                if constexpr (std::is_void_v<T>) {
                    return PollResult<OptionalType>(false);
                } else {
                    return PollResult<OptionalType>(std::nullopt);
                }
            }
            m_state->consumer_waker = ctx.getWaker()->clone();
            return PollPending;
        }
        auto result = std::move(m_state->results.front());
        m_state->results.pop_front();
        lock.unlock();
        if (result.isReady()) {
            if constexpr (std::is_void_v<T>) {
                return PollResult<OptionalType>(true);
            } else {
                return PollResult<OptionalType>(std::move(result).value());
            }
        } else if (result.isError()) {
            return PollError(result.error());
        } else if (result.isDropped()) {
            return PollDropped;
        }
        std::terminate();
        return PollPending;
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
    // Breaks the JoinSetSharedState ↔ JoinSetTaskHandle reference cycle by extracting
    // all JoinHandle<T> objects while holding m_state->mutex, then destroying them
    // outside the lock.  JoinHandle<T>::~JoinHandle() marks each task cancelled and,
    // if called from within a coroutine's poll(), registers it with the enclosing
    // CoroutineScope so the scope drains those tasks before the parent completes.
    void cancel_pending() {
        if (!m_state) return;
        std::vector<JoinHandle<T>> to_cancel;
        {
            std::lock_guard lock(m_state->mutex);
            m_state->closing = true;
            to_cancel.reserve(m_state->pending_handles.size());
            for (auto& h : m_state->pending_handles)
                to_cancel.push_back(h->take_handle());
            m_state->pending_handles.clear();
        }
        // JoinHandle<T> destructors fire here, outside the lock.
    }

    std::shared_ptr<detail::JoinSetSharedState<T>> m_state;
};

} // namespace coro
