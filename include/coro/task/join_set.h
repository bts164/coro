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
#include <coro/task/cancel_handle.h>
#include <concepts>
#include <exception>
#include <memory>
#include <optional>
#include <coro/detail/mutex.h>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace coro {

// -----------------------------------------------------------------------
// JoinCancelled, OnError, JoinResult<T, Tag>
// -----------------------------------------------------------------------

/**
 * @brief Thrown by `JoinResult::value()` for a task that was cancelled, and by
 * `JoinSet::drain()` when the first failure is a cancelled task.
 *
 * To the parent a missing result is an ordinary error, not its own cancellation, so this
 * derives from `std::exception` and is unrelated to `TaskCancelled`: a `TaskCancelled`
 * escaping the parent would cancel the parent itself. It carries nothing; a caller who
 * cares which task it was checks `JoinResult::is_cancelled()` before calling `value()`,
 * and has its `tag()` there.
 */
struct JoinCancelled : std::exception {
    const char* what() const noexcept override {
        return "coro::JoinCancelled: the task was cancelled before it produced a result";
    }
};

/// @brief What `JoinSet::join_all()` and `JoinSet::drain()` do at a task's failure: an
/// exception, or a task that ended cancelled.
enum class OnError {
    Cancel,  ///< Cancel the remaining tasks; they then end as cancelled, unless they finish first.
    Wait,    ///< Let every task run to its own end.
};

namespace detail {

/// The tag stored for a `JoinSet<T, void>`: nothing.
struct NoTag {};

template<typename Tag>
using TagStorage = std::conditional_t<std::is_void_v<Tag>, NoTag, Tag>;

/// How a task ended: its value (`std::monostate` for `void`), its exception, or cancelled.
struct CancelledOutcome {};

template<typename T>
using JoinOutcome = std::variant<std::conditional_t<std::is_void_v<T>, std::monostate, T>,
                                 std::exception_ptr, CancelledOutcome>;

} // namespace detail

/**
 * @brief How one task of a `JoinSet<T, Tag>` ended, and its tag.
 *
 * Exactly one of: a value (`has_value()`), an exception (`exception()` is non-null), or
 * cancelled (`is_cancelled()`). `value()` is the shortcut for a caller that expects a
 * value: it returns it, rethrows the task's own exception, or throws `JoinCancelled`.
 * Never undefined behaviour.
 *
 * ```cpp
 * while (auto r = co_await next(js)) {
 *     if (r->is_cancelled()) log("call %d cancelled", r->tag());
 *     else total += r->value();  // rethrows the task's exception
 * }
 * ```
 *
 * @tparam T   The task's value type; `void` for none.
 * @tparam Tag The tag given at spawn; `void` (the default) for none, and then `tag()`
 *             does not exist.
 */
template<typename T, typename Tag = void>
class JoinResult {
    using Value    = std::conditional_t<std::is_void_v<T>, std::monostate, T>;
    using TagValue = detail::TagStorage<Tag>;
    using Ref      = std::conditional_t<std::is_void_v<T>, void, Value&>;
    using ConstRef = std::conditional_t<std::is_void_v<T>, void, const Value&>;
    using RRef     = std::conditional_t<std::is_void_v<T>, void, Value&&>;

public:
    using ValueType = T;
    using TagType   = Tag;

    /// @brief Made by `JoinSet`.
    JoinResult(detail::JoinOutcome<T> outcome, TagValue tag)
        : m_outcome(std::move(outcome)), m_tag(std::move(tag)) {}

    /// @brief True if the task returned a value (for `void`, returned normally).
    [[nodiscard]] bool has_value() const noexcept { return m_outcome.index() == 0; }

    /// @brief True if the task was cancelled before it produced a result.
    [[nodiscard]] bool is_cancelled() const noexcept { return m_outcome.index() == 2; }

    /// @brief The task's exception, or null if it returned a value or was cancelled.
    [[nodiscard]] std::exception_ptr exception() const noexcept {
        if (auto* e = std::get_if<1>(&m_outcome)) return *e;
        return nullptr;
    }

    /**
     * @brief The task's value. For `void`, returns if the task returned normally.
     *
     * @throws the task's own exception, rethrown, if it threw.
     * @throws JoinCancelled if the task was cancelled.
     */
    Ref value() & {
        check();
        if constexpr (!std::is_void_v<T>) return std::get<0>(m_outcome);
    }
    ConstRef value() const & {
        check();
        if constexpr (!std::is_void_v<T>) return std::get<0>(m_outcome);
    }
    RRef value() && {
        check();
        if constexpr (!std::is_void_v<T>) return std::move(std::get<0>(m_outcome));
    }

    /// @brief The tag the task was spawned with, whatever its ending.
    TagValue&       tag() &       requires (!std::is_void_v<Tag>) { return m_tag; }
    const TagValue& tag() const & requires (!std::is_void_v<Tag>) { return m_tag; }
    TagValue&&      tag() &&      requires (!std::is_void_v<Tag>) { return std::move(m_tag); }

private:
    void check() const {
        if (auto* e = std::get_if<1>(&m_outcome)) std::rethrow_exception(*e);
        if (m_outcome.index() == 2) throw JoinCancelled{};
    }

    detail::JoinOutcome<T>          m_outcome;
    [[no_unique_address]] TagValue  m_tag;
};

/// @brief A container `JoinSet::join_all()` can fill: movable, and takes each result
/// through `emplace_back()`, as `std::vector`, `std::deque` and `std::list` do.
template<typename C, typename R>
concept JoinAllContainer = std::movable<C> && requires(C& c, R&& r) {
    c.emplace_back(std::move(r));
};

namespace detail {

// -----------------------------------------------------------------------
// JoinSetSharedState<T, Tag>
// -----------------------------------------------------------------------

/**
 * @brief Shared state for a JoinSet<T, Tag>.
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
template<typename T, typename Tag>
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
        // In the task, not in its future, so a cancelled task still has it. Written at
        // spawn, read once by take_result().
        [[no_unique_address]] TagStorage<Tag> tag;
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

    /**
     * @brief Takes the outcome and tag of a node just popped from idle_handles, and
     * releases the task. Call outside the mutex: the release may destroy the task.
     *
     * No lock on the task's own mutex: the task wrote its result before
     * on_task_complete() pushed the node under this state's mutex, and the caller popped
     * it under that same mutex, which orders the write before these reads. Nothing writes
     * the result after completion. A JoinHandle has no such hand-off, which is why
     * JoinHandle::poll() locks.
     */
    static JoinResult<T, Tag> take_result(Node& node) {
        Rc<TaskState<T>> ts = std::move(node.completed);
        // Moved out before `ts` is released: the node lives in the task.
        TagStorage<Tag> tag = std::move(node.tag);
        if (ts->exception)
            return {JoinOutcome<T>(std::in_place_index<1>, std::move(ts->exception)), std::move(tag)};
        if constexpr (std::is_void_v<T>) {
            if (ts->result)
                return {JoinOutcome<T>(std::in_place_index<0>), std::move(tag)};
        } else {
            if (ts->result.has_value())
                return {JoinOutcome<T>(std::in_place_index<0>, std::move(*ts->result)), std::move(tag)};
        }
        return {JoinOutcome<T>(std::in_place_index<2>), std::move(tag)};
    }

    detail::Mutex                       mutex;
    Rc<Waker>                           consumer_waker;
    detail::IntrusiveList<Node*>        pending_handles; ///< Running tasks (lifetime anchors).
    detail::IntrusiveList<Node*>        idle_handles;    ///< Completed tasks awaiting consumption.
};

// -----------------------------------------------------------------------
// JoinSetTask<F, Tag>
// -----------------------------------------------------------------------

/**
 * @brief Concrete task for JoinSet::spawn(). A single allocation covers the
 * executor-facing TaskBase, the result-holding TaskState<T>, and the Node
 * that links this task into JoinSetSharedState::pending_handles and holds its tag.
 *
 * Lifetime is anchored by the executor's owned map. The Node holds a non-owning
 * raw TaskBase* pointer so the intrusive list gets O(1) removal.
 *
 * cancel_pending() sets m_node.task to nullptr under the mutex.
 * on_task_complete() checks !m_node.task under the same mutex: if null, the
 * JoinSet is being destroyed so the result is discarded rather than pushed to
 * idle_handles.
 *
 * cancel_task() is TaskImpl's and never touches the set's mutex, so
 * join_set_cancel_all() and a CancelHandle may call it whether or not they hold it.
 */
template<Future F, typename Tag>
class JoinSetTask : public TaskImpl<F> {
    using T           = typename F::OutputType;
    using SharedState = JoinSetSharedState<T, Tag>;
public:
    JoinSetTask(F future, TagStorage<Tag> tag, Weak<SharedState> set_state)
        : TaskImpl<F>(std::move(future))
        , m_node{.tag = std::move(tag)}
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

    typename SharedState::Node m_node;
private:
    Weak<SharedState> m_set_state;
};

/**
 * @brief Cancels every pending task of the set, leaving each in pending_handles
 * with its Node::task set: unlike JoinSet::cancel_pending(), the set keeps tracking
 * them, and on_task_complete() moves each to idle_handles when it finishes.
 *
 * Only asks: the tasks finish at their next poll, on whatever worker.
 */
template<typename SharedState>
void join_set_cancel_all(SharedState& state) {
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
// JoinSetBulkFuture<T, Tag, Collector>
// -----------------------------------------------------------------------

/// JoinSet::join_all(): keeps every result, in completion order.
template<typename C>
struct JoinAllCollector {
    using OutputType = C;
    static constexpr bool kCancelsAtStart = false;

    C results;

    template<typename R>
    void add(R&& result) { results.emplace_back(std::move(result)); }
    PollResult<C> finish() { return std::move(results); }
};

/// JoinSet::drain(): keeps the first failure, to rethrow once every task has finished.
struct DrainCollector {
    using OutputType = void;
    static constexpr bool kCancelsAtStart = false;

    /// Whether an exception should give way to a later one. See set_ranks_last().
    using RanksLast = bool (*)(const std::exception_ptr&) noexcept;

    std::exception_ptr first_failure;
    RanksLast          ranks_last = nullptr;

    template<typename R>
    void add(R&& result) {
        if (result.has_value()) return;
        if (first_failure && !(ranks_last && ranks_last(first_failure))) return;
        std::exception_ptr failure = result.is_cancelled()
            ? std::make_exception_ptr(JoinCancelled{})
            : result.exception();
        // Here a first failure ranks last, so ranks_last is set.
        if (!first_failure || !ranks_last(failure))
            first_failure = std::move(failure);
    }

    PollResult<void> finish() {
        if (first_failure) return PollError(std::move(first_failure));
        return PollReady;
    }
};

/// JoinSet::shutdown(): cancels every task at the first poll and ignores every outcome.
struct ShutdownCollector {
    using OutputType = void;
    static constexpr bool kCancelsAtStart = true;

    template<typename R>
    void add(R&&) {}
    PollResult<void> finish() { return PollReady; }
};

/**
 * @brief The future behind JoinSet::join_all(), drain() and shutdown(): one `next()`
 * loop run inside the set, so the awaiter is resumed once rather than once per task.
 *
 * Each poll sweeps idle_handles, handing each task's JoinResult to the collector, and
 * returns Pending while any task is still running. It completes only once every task
 * has finished, so the set is empty afterwards, then reports what the collector made
 * of the outcomes.
 *
 * With `OnError::Cancel`, the first failure (an exception, or a cancelled task) cancels
 * every pending task (join_set_cancel_all()).
 *
 * Cancellable: cancel() cancels every pending task, and the future then returns
 * `PollDropped` once all have finished, discarding their outcomes.
 */
template<typename T, typename Tag, typename Collector>
class JoinSetBulkFuture {
    using SharedState = JoinSetSharedState<T, Tag>;
public:
    using OutputType = typename Collector::OutputType;

    JoinSetBulkFuture(Rc<SharedState> state, OnError on_error, Collector collector = {})
        : m_state(std::move(state)), m_collector(std::move(collector)), m_on_error(on_error) {}

    JoinSetBulkFuture(JoinSetBulkFuture&&) = default;

    /// @brief The first exception is kept unless it ranks last and a later one
    /// doesn't. For parallel(), whose parts fail with TeamBroken once one of them
    /// has thrown: that one's exception is the one to report. drain() only.
    void set_ranks_last(DrainCollector::RanksLast ranks_last) noexcept
        requires std::same_as<Collector, DrainCollector> {
        m_collector.ranks_last = ranks_last;
    }

    /// Cancels every pending task. Polls then return pending until all have
    /// finished, then `PollDropped`.
    void cancel() {
        m_dropped = true;
        cancel_all_once();
    }

    PollResult<OutputType> poll(detail::Context& ctx) {
        if constexpr (Collector::kCancelsAtStart) cancel_all_once();

        IntrusiveList<typename SharedState::Node*> to_process;
        bool any_pending;
        {
            std::lock_guard lock(m_state->mutex);
            to_process = std::move(m_state->idle_handles);
            any_pending = !m_state->pending_handles.empty();
            if (any_pending)
                m_state->consumer_waker = ctx.getWaker();
        }

        // Outside the lock: take_result() releases each task, which may free it.
        bool failed = false;
        while (auto* node = to_process.pop_front()) {
            JoinResult<T, Tag> result = SharedState::take_result(*node);
            if (!result.has_value()) failed = true;
            if (!m_dropped) m_collector.add(std::move(result));
        }

        // The tasks of a set may wait on each other; with one failed the rest may
        // never finish on their own.
        if (failed && m_on_error == OnError::Cancel) cancel_all_once();

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

        if (m_dropped) return PollDropped;
        return m_collector.finish();
    }

private:
    void cancel_all_once() {
        if (m_cancelled) return;
        m_cancelled = true;
        join_set_cancel_all(*m_state);
    }

    Rc<SharedState> m_state;
    Collector       m_collector;
    OnError         m_on_error;
    bool            m_cancelled = false;  // cancel_all done
    bool            m_dropped   = false;  // cancel() called
};

template<typename T, typename Tag, typename C>
using JoinSetJoinAllFuture  = JoinSetBulkFuture<T, Tag, JoinAllCollector<C>>;
template<typename T, typename Tag>
using JoinSetDrainFuture    = JoinSetBulkFuture<T, Tag, DrainCollector>;
template<typename T, typename Tag>
using JoinSetShutdownFuture = JoinSetBulkFuture<T, Tag, ShutdownCollector>;

} // namespace detail


// -----------------------------------------------------------------------
// JoinSetSpawnBuilder<T, Tag, F>
// -----------------------------------------------------------------------

/**
 * @brief Builder returned by `JoinSet<T, Tag>::build_task()`. Mirrors Tokio's
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
template<typename T, typename Tag, Future F>
    requires std::same_as<typename F::OutputType, T>
class [[nodiscard]] JoinSetSpawnBuilder {
    using SharedState = detail::JoinSetSharedState<T, Tag>;
public:
    JoinSetSpawnBuilder(F future, detail::TagStorage<Tag> tag, const detail::Rc<SharedState>& state)
        : m_future(std::move(future)), m_tag(std::move(tag)), m_state(&state) {}

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
    /// @return A handle that can cancel the task; ignoring it is fine.
    CancelHandle spawn() && {
        const auto& state = *m_state;
        auto task = detail::make_rc<detail::JoinSetTask<F, Tag>>(
            std::move(m_future), std::move(m_tag), detail::Weak<SharedState>(state));
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
        detail::Rc<detail::TaskBase> base(std::move(task));
        // Taken before scheduling: from then on the task may finish and be freed at
        // any moment, and the handle sees it as finished.
        CancelHandle handle{detail::Weak<detail::TaskBase>(base)};
        coro::schedule_task(std::move(base));
        return handle;
    }

private:
    F                                  m_future;
    [[no_unique_address]] detail::TagStorage<Tag> m_tag;
    const detail::Rc<SharedState>*     m_state;  ///< The JoinSet's own; not owned.
    std::string                        m_name;
};


// -----------------------------------------------------------------------
// JoinSet<T, Tag> — public API
// -----------------------------------------------------------------------

/**
 * @brief Structured-concurrency set for spawning and collecting homogeneous child tasks.
 *
 * All tasks spawned into a `JoinSet<T, Tag>` must produce values of type `T`. The set is a
 * `Stream` of outcomes: `co_await next(js)` yields a `JoinResult<T, Tag>` for each task, in
 * completion order, however it ended (a value, an exception or cancelled), and `nullopt`
 * once the set is empty. It never fails itself: a task's failure is part of its result.
 *
 * `join_all()`, `drain()` and `shutdown()` run that loop inside the set, resuming the
 * awaiter once rather than once per task. Each waits for every task, so the set is empty
 * and reusable afterwards. An operation that hands back outcomes (`next()`, `join_all()`)
 * never throws them; one that hands back none (`drain()`) throws the first failure.
 *
 * **Tags:** with `Tag` other than `void`, every task is spawned with a tag, the caller's
 * own data (an id, a category), which its `JoinResult` returns. The set only stores it:
 * it never looks a task up by tag, so tags need not be unique.
 *
 * **Allocation model:** each `spawn()` creates a single `JoinSetTask<F, Tag>` allocation
 * covering the executor-facing `TaskBase`, the result-holding `TaskState<T>` and the tag.
 * Task lifetime is anchored by the executor's owned map. When a task completes it
 * moves itself from the pending list to the idle queue; the consumer reads the result
 * directly from `TaskState<T>` without an extra copy.
 *
 * **Cancel on drop:** destroying a `JoinSet` cancels all pending tasks. Each cancelled
 * task is registered as a weak_ptr in the enclosing coroutine scope (if inside one),
 * so the parent waits for all children to drain before completing.
 *
 * @tparam T   The value type produced by spawned tasks. Use `JoinSet<void>` for tasks
 *             that produce no value.
 * @tparam Tag The tag each task is spawned with, or `void` (the default) for none.
 */
template<typename T, typename Tag = void>
class [[nodiscard]] JoinSet {
    using SharedState = detail::JoinSetSharedState<T, Tag>;
public:
    using ItemType = JoinResult<T, Tag>;

    JoinSet() : m_state(detail::make_rc<SharedState>()) {}

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
     * @brief Spawns `future` as a child task. Its outcome is delivered via `next()`,
     * `join_all()` or `drain()`. For an untagged set.
     *
     * May only be called from within a `Runtime::block_on()` context.
     * Use `build_task(future).name("...").spawn()` to set a task name.
     *
     * @return A handle that can cancel the task; ignoring it is fine.
     */
    template<Future F>
        requires (std::is_void_v<Tag> && std::same_as<typename F::OutputType, T>)
    CancelHandle spawn(F future) {
        return build_task(std::move(future)).spawn();
    }

    /// @brief Spawns `future` with `tag`, which its `JoinResult` returns. For a tagged set.
    template<Future F>
        requires (!std::is_void_v<Tag> && std::same_as<typename F::OutputType, T>)
    CancelHandle spawn(detail::TagStorage<Tag> tag, F future) {
        return build_task(std::move(tag), std::move(future)).spawn();
    }

    /**
     * @brief Returns a builder for configuring and spawning a child task. For an
     * untagged set.
     *
     * Mirrors Tokio's `JoinSet::build_task()`. Call `.name("...")` to attach a
     * human-readable name, then `.spawn()` to submit. `[[nodiscard]]` — discarding
     * the builder without calling `.spawn()` silently drops the future.
     *
     * May only be called from within a `Runtime::block_on()` context.
     */
    template<Future F>
        requires (std::is_void_v<Tag> && std::same_as<typename F::OutputType, T>)
    [[nodiscard]] JoinSetSpawnBuilder<T, Tag, F> build_task(F future) {
        return JoinSetSpawnBuilder<T, Tag, F>(std::move(future), {}, m_state);
    }

    /// @brief As build_task(future), with `tag`. For a tagged set.
    template<Future F>
        requires (!std::is_void_v<Tag> && std::same_as<typename F::OutputType, T>)
    [[nodiscard]] JoinSetSpawnBuilder<T, Tag, F> build_task(detail::TagStorage<Tag> tag, F future) {
        return JoinSetSpawnBuilder<T, Tag, F>(std::move(future), std::move(tag), m_state);
    }

    /**
     * @brief Satisfies `Stream<JoinResult<T, Tag>>`. Returns the next task's outcome, in
     * completion order, or end-of-stream when no task is left. Never fails: an exception
     * or a cancellation is the outcome of one task, inside its `JoinResult`.
     *
     * Use via `co_await next(js)` rather than calling directly.
     */
    PollResult<std::optional<ItemType>> poll_next(detail::Context& ctx) {
        std::unique_lock lock(m_state->mutex);

        if (auto* node = m_state->idle_handles.pop_front()) {
            lock.unlock();
            return std::optional<ItemType>(SharedState::take_result(*node));
        }

        if (!m_state->pending_handles.empty()) {
            m_state->consumer_waker = ctx.getWaker();
            return PollPending;
        }

        // Stream exhausted — no pending or idle tasks remain.
        return std::optional<ItemType>(std::nullopt);
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
     * @brief Waits for every task and returns all their `JoinResult`s, in completion
     * order, in a `std::vector` or the container `C`. Never throws a task's failure:
     * each is in its result.
     *
     * ```cpp
     * for (auto& r : co_await js.join_all()) total += r.value();
     * ```
     *
     * @param on_error `Cancel` (the default) cancels the remaining tasks at the first
     *                 failure; they are then collected as cancelled, unless they finish
     *                 first. `Wait` lets every task run to its own end.
     *
     * The future is `Cancellable`: cancelled, it cancels every task, waits for them, and
     * ends as cancelled.
     */
    template<typename C = std::vector<ItemType>>
        requires (JoinAllContainer<C, ItemType> && std::default_initializable<C>)
    [[nodiscard]] detail::JoinSetJoinAllFuture<T, Tag, C> join_all(OnError on_error = OnError::Cancel) {
        return join_all(C{}, on_error);
    }

    /**
     * @brief As join_all(on_error), into `into`, which is moved in and returned filled:
     * one reserved ahead, or carrying an allocator.
     */
    template<typename C>
        requires JoinAllContainer<C, ItemType>
    [[nodiscard]] detail::JoinSetJoinAllFuture<T, Tag, C> join_all(C into, OnError on_error = OnError::Cancel) {
        return {m_state, on_error, detail::JoinAllCollector<C>{std::move(into)}};
    }

    /**
     * @brief Waits for every task, discarding values, then rethrows the first failure in
     * completion order, if any: a task's exception, or `JoinCancelled` for a cancelled
     * task. Later failures are dropped. `join_all()` without the container.
     *
     * @param on_error as for join_all().
     *
     * The future is `Cancellable`: cancelled, it cancels every task, waits for them, and
     * ends as cancelled.
     */
    [[nodiscard]] detail::JoinSetDrainFuture<T, Tag> drain(OnError on_error = OnError::Cancel) {
        return {m_state, on_error};
    }

    /**
     * @brief Cancels every task, waits for them all, and ignores their outcomes, as
     * Tokio's `shutdown()`. Throws nothing; cancelled itself, it still waits for every
     * task, and ends as cancelled.
     *
     * Tasks spawned after its first poll are not cancelled, but are waited for.
     */
    [[nodiscard]] detail::JoinSetShutdownFuture<T, Tag> shutdown() {
        return {m_state, OnError::Cancel};
    }

    /**
     * @brief Cancels every pending task without dropping the set, like tokio's
     * `abort_all()`. The tasks stay in the set: each still finishes, as cancelled unless
     * it finishes first, and is reported by next(), join_all() and drain() as usual.
     *
     * WARNING: waits for nothing. A task finishes at its next poll, on whatever
     * worker; anything it references must outlive that. Await shutdown() instead
     * to wait for them.
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

    detail::Rc<SharedState> m_state;
};

} // namespace coro
