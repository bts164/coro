#pragma once

#include <coro/detail/context.h>
#include <coro/detail/coro_scope.h>
#include <coro/detail/intrusive_list.h>
#include <coro/detail/poll_result.h>
#include <coro/detail/task.h>
#include <coro/detail/task_state.h>
#include <coro/detail/waker.h>
#include <coro/detail/rc.h>
#include <coro/future.h>
#include <coro/runtime/runtime.h>
#include <exception>
#include <memory>
#include <optional>
#include <coro/detail/mutex.h>
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
 * - `pending_handles` — intrusive list of Nodes embedded in each running
 *   JoinSetTask. Each Node holds a non-owning raw TaskBase* pointer.
 *   Lifetime is anchored by the executor's owned map. When a task completes
 *   normally, on_task_complete() removes its node in O(1). When the JoinSet
 *   is destroyed, cancel_pending() sets each node's task pointer to nullptr
 *   (the sentinel that tells on_task_complete() to skip) and calls cancel_task().
 *
 * - `idle_handles` — intrusive list of the same Nodes, for tasks that have
 *   reached a terminal state. A task's Node moves from pending_handles to
 *   idle_handles when it completes, so it is in at most one list at a time.
 *   Once complete the executor no longer keeps the task alive, so the Node
 *   holds an aliased Rc<TaskState<T>> to its own task while it is idle;
 *   whoever pops the Node takes that reference. The consumer reads results
 *   directly from TaskState<T>, avoiding any extra result copy. Unlike a
 *   deque, the list allocates nothing, including when a drain swaps it out.
 *
 * JoinSetTask holds a weak_ptr<JoinSetSharedState> — no strong back-reference.
 */
template<typename T>
struct JoinSetSharedState {
    struct Node {
        // Raw pointer, not weak_ptr: weak_ptr::lock() is an atomic operation, and
        // it would always succeed here anyway — a task stays alive in the executor's
        // m_owned_tasks set for the entire time its Node is in pending_handles.
        // on_task_complete() fires from inside poll() before the executor erases the
        // task, so there is no window where this pointer can dangle.
        // nullptr is the sentinel meaning cancel_pending() already processed this node.
        TaskBase* task = nullptr;
        // Set only while the Node is in idle_handles: the task's reference to itself,
        // which keeps it alive until the consumer pops it. Move it out on popping, and
        // drop it outside the mutex, since that may destroy the task.
        Rc<TaskState<T>> completed;
        Node* next = nullptr;
        Node* prev = nullptr;
    };

    JoinSetSharedState() = default;
    JoinSetSharedState(const JoinSetSharedState&)            = delete;
    JoinSetSharedState& operator=(const JoinSetSharedState&) = delete;

    /// Releases completed tasks nobody consumed: each refers to itself while in
    /// idle_handles, so unlinking alone would leak it. No lock: this is the last owner.
    ~JoinSetSharedState() {
        while (Node* node = idle_handles.pop_front()) {
            Rc<TaskState<T>> completed = std::move(node->completed);
        }
    }

    detail::Mutex                       mutex;
    Rc<Waker>                           consumer_waker;
    detail::IntrusiveList<Node*>        pending_handles; ///< Running tasks (lifetime anchors).
    detail::IntrusiveList<Node*>        idle_handles;    ///< Completed tasks awaiting consumption.
};

// -----------------------------------------------------------------------
// JoinSetTask<F>
// -----------------------------------------------------------------------

/**
 * @brief Concrete task for JoinSet::spawn(). A single allocation covers the
 * executor-facing TaskBase, the result-holding TaskState<T>, and the Node
 * that links this task into JoinSetSharedState::pending_handles.
 *
 * Lifetime is anchored by the executor's owned map. The Node holds a non-owning
 * raw TaskBase* pointer so the intrusive list gets O(1) removal.
 *
 * cancel_pending() sets m_node.task to nullptr under the mutex.
 * on_task_complete() checks !m_node.task under the same mutex: if null, the
 * JoinSet is being destroyed so the result is discarded rather than pushed to
 * idle_handles.
 */
template<Future F>
class JoinSetTask : public TaskImpl<F> {
    using T = typename F::OutputType;
public:
    JoinSetTask(F future, Weak<JoinSetSharedState<T>> set_state)
        : TaskImpl<F>(std::move(future))
        , m_set_state(std::move(set_state)) {}

    void on_task_complete() noexcept override {
        auto state = m_set_state.lock();
        if (!state) {
            // JoinSet was destroyed. cancel_pending() already cleared m_node.task.
            // The executor's temporary reference keeps the task alive through this poll.
            return;
        }

        auto self_base = this->shared_from_this();  // Rc<TaskBase>
        Rc<TaskState<T>> self_state(self_base, static_cast<TaskState<T>*>(this));

        Rc<Waker> waker;
        {
            std::lock_guard lock(state->mutex);
            if (!m_node.task) return;  // JoinSet is being destroyed — discard result
            state->pending_handles.remove(&m_node);
            m_node.task = nullptr;
            m_node.completed = std::move(self_state);
            state->idle_handles.push_back(&m_node);
            waker = std::exchange(state->consumer_waker, nullptr);
        }
        if (waker) waker->wake();
    }

    typename JoinSetSharedState<T>::Node m_node;
private:
    Weak<JoinSetSharedState<T>> m_set_state;
};

/**
 * @brief Cancels every pending task of the set, leaving each in pending_handles
 * with its Node::task set: unlike JoinSet::cancel_pending(), the set keeps tracking
 * them, and on_task_complete() moves each to idle_handles when it finishes.
 *
 * Only asks: the tasks finish at their next poll, on whatever worker.
 */
template<typename T>
void join_set_cancel_all(JoinSetSharedState<T>& state) {
    // Strong refs taken under the lock: once it is released a task may finish and
    // be destroyed (see JoinSet::cancel_pending()).
    std::vector<Rc<TaskBase>> to_cancel;
    {
        std::lock_guard lock(state.mutex);
        to_cancel.reserve(state.pending_handles.size());
        for (auto* node = state.pending_handles.front(); node; node = node->next)
            if (node->task) to_cancel.push_back(node->task->shared_from_this());
    }
    // Outside the lock: cancel_task() wakes the task, and a task that has already
    // finished ignores both the flag and the wake.
    for (auto& task : to_cancel)
        task->cancel_task();
}

// -----------------------------------------------------------------------
// JoinSetDrainFuture<T, Mode>
// -----------------------------------------------------------------------

/// What a drain does when a task fails. See doc/design/join_set.md.
enum class DrainMode {
    All,            ///< drain(): wait for every task, then rethrow the first exception.
    CancelOnError,  ///< drain_or_cancel(): on the first exception cancel the rest,
                    ///< wait for every task, then rethrow it.
};

/**
 * @brief Future<void> returned by JoinSet::drain() and JoinSet::drain_or_cancel().
 *
 * Polls until all pending tasks have completed or been cancelled. Each poll
 * sweeps idle_handles (discarding values, capturing the first exception) and
 * returns Pending if any tasks are still running. Rethrows the first exception
 * after pending_handles reaches zero.
 *
 * With `DrainMode::CancelOnError` the first exception also cancels every pending
 * task (join_set_cancel_all()), and the future is `Cancellable`: cancel() cancels
 * every pending task, and the future then returns `PollDropped` once all have
 * finished.
 */
template<typename T, DrainMode Mode = DrainMode::All>
class JoinSetDrainFuture {
public:
    using OutputType = void;

    explicit JoinSetDrainFuture(Rc<JoinSetSharedState<T>> state)
        : m_state(std::move(state)) {}

    JoinSetDrainFuture(JoinSetDrainFuture&&) noexcept = default;

    /// Whether an exception should give way to a later one. See set_ranks_last().
    using RanksLast = bool (*)(const std::exception_ptr&) noexcept;

    /// @brief The first exception is kept unless it ranks last and a later one
    /// doesn't. For parallel(), whose parts fail with TeamBroken once one of them
    /// has thrown: that one's exception is the one to report.
    void set_ranks_last(RanksLast ranks_last) noexcept { m_ranks_last = ranks_last; }

    /// Cancels every pending task. Polls then return pending until all have
    /// finished, then `PollDropped`.
    void cancel() requires (Mode == DrainMode::CancelOnError) {
        m_dropped = true;
        cancel_all_once();
    }

    PollResult<void> poll(detail::Context& ctx) {
        IntrusiveList<typename JoinSetSharedState<T>::Node*> to_process;
        bool any_pending;
        {
            std::lock_guard lock(m_state->mutex);
            to_process = std::move(m_state->idle_handles);
            any_pending = !m_state->pending_handles.empty();
            if (any_pending)
                m_state->consumer_waker = ctx.getWaker();
        }

        // Process completed tasks outside the lock — collect first exception, discard values.
        // Each task's reference is dropped at the end of its iteration, which may free it.
        while (auto* node = to_process.pop_front()) {
            Rc<TaskState<T>> ts = std::move(node->completed);
            if (m_first_exception && !(m_ranks_last && m_ranks_last(m_first_exception)))
                continue;
            // No lock on ts->mutex, for the reason given in JoinSet::poll_next(): the
            // node was taken from idle_handles under m_state->mutex.
            std::exception_ptr exception = ts->exception;
            if (!exception) continue;
            if (!m_first_exception || !m_ranks_last(exception))
                m_first_exception = std::move(exception);
        }

        if constexpr (Mode == DrainMode::CancelOnError) {
            // The tasks of this set wait on each other; with one failed the rest may
            // never finish on their own. Cancelled tasks carry no exception, so later
            // sweeps only find tasks that failed for themselves.
            if (m_first_exception) cancel_all_once();
        }

        if (any_pending) return PollPending;

        // No pending tasks at snapshot time. Re-check: a task may have completed
        // and moved to idle between our swap and here.
        {
            std::lock_guard lock(m_state->mutex);
            if (!m_state->pending_handles.empty() || !m_state->idle_handles.empty()) {
                m_state->consumer_waker = ctx.getWaker();
                return PollPending;
            }
        }

        if constexpr (Mode == DrainMode::CancelOnError) {
            if (m_dropped) return PollDropped;
        }
        if (m_first_exception) return PollError(std::move(m_first_exception));
        return PollReady;
    }

private:
    void cancel_all_once() {
        if (m_cancelled) return;
        m_cancelled = true;
        join_set_cancel_all(*m_state);
    }

    Rc<JoinSetSharedState<T>> m_state;
    std::exception_ptr        m_first_exception;
    RanksLast                 m_ranks_last = nullptr;
    bool                      m_cancelled = false;  // CancelOnError: cancel_all done
    bool                      m_dropped   = false;  // CancelOnError: cancel() called
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
 *
 * WARNING: use it only as a temporary, in one expression:
 * `js.build_task(f).name("x").spawn();`. It holds a non-owning pointer to the
 * set, so storing it and spawning after the `JoinSet` is destroyed or moved from
 * is undefined behaviour. Holding a reference instead would cost every spawn a
 * reference count increment and decrement on the set's state, which the set's
 * children, completing on other workers, also touch.
 */
template<typename T, Future F>
    requires std::same_as<typename F::OutputType, T>
class [[nodiscard]] JoinSetSpawnBuilder {
public:
    JoinSetSpawnBuilder(F future, const detail::Rc<detail::JoinSetSharedState<T>>& state)
        : m_future(std::move(future)), m_state(&state) {}

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
        const auto& state = *m_state;
        auto task = detail::make_rc<detail::JoinSetTask<F>>(
            std::move(m_future),
            detail::Weak<detail::JoinSetSharedState<T>>(state));
        task->name = std::move(m_name);
        // Non-owning raw pointer — lifetime is anchored by the executor's owned map.
        task->m_node.task = task.get();
#ifdef CORO_PICO
        task->set_self(detail::Weak<detail::TaskBase>(task));
#endif
        {
            std::lock_guard lock(state->mutex);
            state->pending_handles.push_back(&task->m_node);
        }
        coro::schedule_task(detail::Rc<detail::TaskBase>(std::move(task)));
    }

private:
    F                                                m_future;
    const detail::Rc<detail::JoinSetSharedState<T>>* m_state;  ///< The JoinSet's own; not owned.
    std::string                                      m_name;
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
 * covering the executor-facing `TaskBase` and the result-holding `TaskState<T>`.
 * Task lifetime is anchored by the executor's owned map. When a task completes it
 * moves itself from the pending list to the idle queue; the consumer reads the result
 * directly from `TaskState<T>` without an extra copy.
 *
 * **Cancel on drop:** destroying a `JoinSet` cancels all pending tasks. Each cancelled
 * task is registered as a weak_ptr in the enclosing coroutine scope (if inside one),
 * so the parent waits for all children to drain before completing.
 *
 * @tparam T The value type produced by spawned tasks. Use `JoinSet<void>` for tasks
 *           that produce no value.
 */
template<typename T>
class [[nodiscard]] JoinSet {
public:
    using ItemType     = T;
    using OptionalType = std::conditional_t<std::is_void_v<T>, bool, std::optional<T>>;

    JoinSet() : m_state(detail::make_rc<detail::JoinSetSharedState<T>>()) {}

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

        if (auto* node = m_state->idle_handles.pop_front()) {
            detail::Rc<detail::TaskState<T>> task_state = std::move(node->completed);
            lock.unlock();

            // Read result directly from TaskState — same logic as JoinHandle::poll().
            // No need for task_state->mutex: the task wrote its result before
            // on_task_complete() pushed the node under m_state->mutex, and we popped it
            // under that same mutex, which orders the write before this read. Nothing
            // writes the result after completion. A JoinHandle has no such hand-off,
            // which is why JoinHandle::poll() locks.
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
            m_state->consumer_waker = ctx.getWaker();
            return PollPending;
        }

        // Stream exhausted — no pending or idle tasks remain.
        if constexpr (std::is_void_v<T>)
            return PollResult<OptionalType>(false);
        else
            return PollResult<OptionalType>(std::nullopt);
    }

    /**
     * @brief Returns true if no tasks are currently pending or awaiting consumption.
     *
     * Useful to skip awaiting `next(js)` when the set has nothing running — an empty
     * `JoinSet`'s `poll_next()` resolves immediately (end-of-stream), so a bare
     * `co_await next(js)` in a `select()` would otherwise win every round without
     * ever suspending. Pair with `coro::when(!js.empty(), [&]{ return next(js); })`.
     */
    [[nodiscard]] bool empty() const {
        std::lock_guard lock(m_state->mutex);
        return m_state->pending_handles.empty() && m_state->idle_handles.empty();
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

    /**
     * @brief Like drain(), but the first exception cancels the remaining tasks; the
     * future still waits for every task to finish before it rethrows.
     *
     * For tasks that wait on each other (parts meeting at a `Barrier`, a producer
     * and its consumers), where drain() would wait forever once one has failed.
     * The future is `Cancellable`: cancelled, it cancels the tasks and completes
     * once they have all finished.
     */
    [[nodiscard]] detail::JoinSetDrainFuture<T, detail::DrainMode::CancelOnError>
    drain_or_cancel() {
        return detail::JoinSetDrainFuture<T, detail::DrainMode::CancelOnError>{m_state};
    }

    /**
     * @brief Cancels every pending task without dropping the set, like tokio's
     * `abort_all()`. The tasks stay in the set: each still finishes (with no result)
     * and is reported by next(), drain() and drain_or_cancel() as usual.
     *
     * WARNING: waits for nothing. A task finishes at its next poll, on whatever
     * worker; anything it references must outlive that. Await drain() before
     * releasing it.
     */
    void cancel_all() { detail::join_set_cancel_all(*m_state); }

private:
    // Cancels all pending tasks and, if inside a coroutine scope, registers each
    // as a weak_ptr so the parent waits for them to drain.
    //
    // Sets each node's task pointer to nullptr under the mutex — the sentinel that
    // tells on_task_complete() the JoinSet is gone and to discard the result.
    //
    // A node being present in pending_handles guarantees the task hasn't completed
    // yet ONLY while m_state->mutex is held — on_task_complete() runs concurrently
    // on another worker and races to remove the same node under the same lock. Once
    // the lock is released, that guarantee is gone: the task can finish, get erased
    // from the executor's owned map, and be destroyed before the loop below gets to
    // call cancel_task() on it. shared_from_this() must therefore be captured while
    // still holding the lock, turning each entry into a strong ref that keeps the
    // task alive for the rest of this function — a raw TaskBase* here previously
    // raced with concurrent destruction (caught by TSan as a vptr data race).
    void cancel_pending() {
        if (!m_state) return;
        std::vector<detail::Rc<detail::TaskBase>> to_cancel;
        {
            std::lock_guard lock(m_state->mutex);
            to_cancel.reserve(m_state->pending_handles.size());
            while (auto h = m_state->pending_handles.pop_front()) {
                if (h->task) to_cancel.push_back(h->task->shared_from_this());
                h->task = nullptr;  // sentinel: on_task_complete() discards result
            }
        }
        for (auto& task : to_cancel)
            task->cancel_task();
        if (detail::t_current_coro)
            for (auto& task : to_cancel)
                detail::t_current_coro->add_child(task->weak_from_this());
    }

    detail::Rc<detail::JoinSetSharedState<T>> m_state;
};

} // namespace coro
