#pragma once

// Internal — not part of the public API.
// Type-erased, heap-allocated unit of work held by the executor.
// Users never construct a TaskBase or TaskImpl directly — spawn() creates them internally.

#include <assert.h>
#include <coro/detail/context.h>
#include <coro/detail/waker.h>
#include <coro/future.h>
#include <coro/stream.h>
#include <coro/detail/task_state.h>
#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

// Forward-declared to avoid a circular include with executor.h (which includes task.h).
// TaskBase stores a raw Executor* so both wake() and JoinHandle::cancel() can enqueue
// the task without going through a separate waker object.
namespace coro { class Executor; }

namespace coro::detail {

/**
 * @brief Non-template abstract base for type-erased tasks held by the executor.
 *
 * `TaskBase` carries the scheduling machinery (atomic CAS state, queue membership)
 * without depending on the future type `F` or result type `T`. The executor queues
 * hold `shared_ptr<TaskBase>`.
 *
 * `TaskBase` IS the `Waker`: it inherits `Waker` and `enable_shared_from_this` so
 * that `clone()` is a reference-count increment on the existing task allocation rather
 * than a separate heap object. `wake()` performs the same `Idle → Notified` CAS loop
 * and calls `owning_executor->enqueue()` directly.
 *
 * The concrete subclass `TaskImpl<F>` inherits both `TaskBase` and
 * `TaskState<F::OutputType>`, combining the executor interface, the result/cancellation
 * state, and the future itself into a single `make_shared` allocation.
 *
 * **Not movable:** `TaskBase` objects are always heap-allocated via `make_shared` and
 * accessed through `shared_ptr`. Moving the object itself is never required.
 */
class TaskBase : public Waker, public std::enable_shared_from_this<TaskBase> {
public:
    TaskBase()                             = default;
    TaskBase(const TaskBase&)              = delete;
    TaskBase& operator=(const TaskBase&)   = delete;
    TaskBase(TaskBase&&)                   = delete;
    TaskBase& operator=(TaskBase&&)        = delete;
    ~TaskBase() override                   = default;

    /**
     * @brief Atomic scheduling state. Managed exclusively by the executor and wake().
     *
     * Starts at `Idle`. `schedule()` sets it to `Notified` before the first enqueue.
     * All subsequent transitions are CAS operations — see `SchedulingState` for the
     * full transition table.
     */
    alignas(64) std::atomic<SchedulingState> scheduling_state{SchedulingState::Idle};

    /// @brief Optional human-readable name set via SpawnBuilder::name(). Empty if unset.
    std::string name;

    /**
     * @brief Index of the worker that last ran this task, or -1 if unknown.
     *
     * Written by the WorkSharingExecutor worker loop after each poll() call.
     * Read by enqueue() to route re-enqueued tasks back to the same worker's local
     * queue (task affinity). The Running → Idle CAS provides the happens-before
     * edge that makes a plain int safe here.
     */
    int last_worker_index = -1;

    /**
     * @brief The executor that first scheduled this task. Set by Executor::schedule()
     * before the task is ever enqueued. Never changes after that.
     *
     * Used by wake() and JoinHandle::cancel() to re-enqueue a sleeping task. Raw
     * pointer is safe because the executor always outlives the tasks it owns.
     *
     * EDGE CASE: null if the task was constructed but never scheduled (not possible
     * through any public spawn path, but defensively guarded in JoinHandle::cancel()).
     */
    Executor* owning_executor = nullptr;

    /**
     * @brief Advances the inner future by one step.
     * @return `true` if the task has reached a terminal state (Ready, Error, or Dropped);
     *         `false` if still Pending and should be moved to the Suspended state.
     */
    virtual bool poll(Context& ctx) = 0;

    /**
     * @brief Returns true if the task has reached a terminal state.
     * Implemented by TaskImpl<F> by checking TaskState<T>::terminated under lock.
     * Called by CoroutineScope for type-erased drain tracking via weak_ptr<TaskBase>.
     */
    virtual bool is_complete() const = 0;

    /**
     * @brief Installs a weak waker that is fired when this task completes.
     * Called by CoroutineScope::set_drain_waker() to notify the parent when a
     * dropped child finishes. Implemented by TaskImpl<F>.
     */
    virtual void set_waker(std::weak_ptr<Waker> waker) = 0;

    /**
     * @brief Called by TaskImpl<F>::poll() immediately after the terminal
     * setResult/setException/mark_done call, before returning true.
     *
     * Default is a no-op. Override (e.g. in JoinSetTask) to perform
     * post-completion bookkeeping without re-implementing the full poll loop.
     */
    virtual void on_task_complete() noexcept {}

    /**
     * @brief Marks the task cancelled and wakes it so it sees cancelled on
     * its next poll. Implemented by TaskImpl<F>.
     *
     * Default is a no-op. Called by JoinSet::cancel_pending() through the
     * type-erased TaskBase* stored in pending_handles.
     */
    virtual void cancel_task() noexcept {}

    // Waker implementation — defined in task.cpp to break the circular include with executor.h.
    //
    // NOT ISR-SAFE: wake() calls shared_from_this() (atomic shared_ptr refcount) and
    // enqueue() on the owning executor. On RP2040 single-core this is safe in practice,
    // but the guarantee is platform- and configuration-dependent. Do not call wake()
    // from an ISR without first reading doc/isr_safety.md. A safe wake_from_isr()
    // alternative is tracked there.
    void wake() override;
    std::shared_ptr<Waker> clone() override;

    /// @brief The task currently being polled on this thread, or nullptr if outside a poll.
    /// Set by the executor before each poll() call and cleared afterward.
#ifdef CORO_PICO
    static TaskBase* current;
#else
    static thread_local TaskBase* current;
#endif

    /// @brief Returns the name of the currently-polling task, or "" if none.
    static std::string_view current_name() {
        return current ? std::string_view(current->name) : std::string_view{};
    }
};

/**
 * @brief Concrete, type-erased task. One `make_shared` allocation combines the executor
 * interface, the result/cancellation state, and the future.
 *
 * Inherits `TaskBase` (executor queue / waker) and `TaskState<F::OutputType>`
 * (`JoinHandle`). `spawn()` creates one instance via `make_shared<TaskImpl<F>>()` and
 * produces two aliased `shared_ptr`s from it:
 * - `shared_ptr<TaskBase>` for the executor and waker clones (`Idle` ownership)
 * - `shared_ptr<TaskState<T>>` for the `JoinHandle`
 *
 * Both alias the same `TaskImpl<F>` allocation and share the same reference count.
 *
 * **Cancellation:** if `TaskState::cancelled` is set before `poll()` is called and `F`
 * satisfies `Cancellable`, `TaskImpl` calls `m_future.cancel()` once and then polls
 * until the future drains (running the 4-step cancel protocol). For non-`Cancellable`
 * futures the task is marked done immediately.
 *
 * @tparam F Any type satisfying @ref Future.
 */
template<Future F>
class TaskImpl : public TaskBase, public TaskState<typename F::OutputType> {
public:
    using OutputType = typename F::OutputType;

    explicit TaskImpl(F future)
        : m_future(std::move(future)) {}

    bool is_complete() const override {
        std::lock_guard lock(this->mutex);
        return this->terminated;
    }

    void set_waker(std::weak_ptr<Waker> waker) override {
        std::lock_guard lock(this->mutex);
        this->waker = std::move(waker);
    }

    void cancel_task() noexcept override {
        // owning_executor must always be set. A null executor means the task was never
        // scheduled, so cancel_task() would set cancelled=true but the task would never
        // be polled, mark_done() would never fire, and any CoroutineScope drain would
        // deadlock. Every task must go through spawn() before its handle can be dropped.
        assert(owning_executor != nullptr);
        this->cancelled.store(true, std::memory_order_relaxed);
        wake();
    }

    bool poll(Context& ctx) override {
        if (m_completed) return true;

        const bool cancelled = this->cancelled.load(std::memory_order_relaxed);

        if (cancelled) {
            if constexpr (Cancellable<F>) {
                // Cooperative cancel: call cancel() once, then poll until the future
                // drains (Coro<T>/CoroStream<T> will run the 4-step cancel protocol).
                if (!m_cancel_requested) {
                    m_future->cancel();
                    m_cancel_requested = true;
                }
            } else {
                // Non-Cancellable (leaf) future: mark done immediately.
                m_completed = true;
                m_future.reset();
                this->mark_done();
                on_task_complete();
                return true;
            }
        }

        auto result = m_future->poll(ctx);
        if (result.isPending()) return false;

        m_completed = true;
        // Destroy the future (and any coroutine frame it holds) immediately on
        // completion — Coro<T>/CoroStream<T> already destroy their frames when done,
        // but other Future types may not. Reset here so that a JoinHandle keeping
        // TaskState<T> alive does not also pin the future allocation.
        m_future.reset();

        if (cancelled || result.isDropped()) {
            this->mark_done();
        } else if (result.isError()) {
            this->setException(result.error());
        } else {
            if constexpr (std::is_void_v<OutputType>)
                this->setResult();
            else
                this->setResult(std::move(result).value());
        }
        on_task_complete();
        return true;
    }

private:
    std::optional<F> m_future;
    bool m_completed        = false;
    bool m_cancel_requested = false;
};

/**
 * @brief Concrete stream task. One make_shared allocation combines the executor interface
 * (TaskBase), the bounded item queue (StreamTaskState<T>), and the stream itself.
 *
 * Mirrors the structure of TaskImpl<F>: inherits TaskBase (executor queue / waker) and
 * StreamTaskState<T> (StreamHandle queue access). spawn(stream) creates one instance and
 * produces two aliased shared_ptrs from it:
 *   - shared_ptr<TaskBase>               for the executor (lifetime anchor)
 *   - shared_ptr<StreamTaskState<T>>     for the StreamHandle (queue access)
 *
 * On each poll() the stream produces the next item into StreamTaskState's queue. When the
 * queue is full the task parks (stores producer_waker) until StreamHandle consumes an item
 * and wakes it. No separate driver coroutine or channel allocation needed.
 *
 * @tparam S Any type satisfying @ref Stream.
 */
template<Stream S>
class StreamTaskImpl : public TaskBase, public StreamTaskState<typename S::ItemType> {
public:
    using ItemType = typename S::ItemType;

    explicit StreamTaskImpl(S stream, std::size_t capacity)
        : StreamTaskState<ItemType>(capacity), m_stream(std::move(stream)) {}

    bool is_complete() const override {
        std::lock_guard lock(this->mutex);
        return this->terminated;
    }

    void set_waker(std::weak_ptr<Waker> waker) override {
        std::lock_guard lock(this->mutex);
        this->scope_waker = std::move(waker);
    }

    void cancel_task() noexcept override {
        // See TaskImpl::cancel_task() — owning_executor must always be set.
        assert(owning_executor != nullptr);
        m_cancelled.store(true, std::memory_order_relaxed);
        wake();
    }

    bool poll(Context& ctx) override {
        if (m_completed) return true;

        if (m_cancelled.load(std::memory_order_relaxed)) {
            // Consumer dropped the StreamHandle — close the queue and terminate.
            std::weak_ptr<Waker> consumer_wk;
            {
                std::lock_guard lock(this->mutex);
                this->closed = true;
                consumer_wk = std::move(this->consumer_waker);
            }
            if (auto w = consumer_wk.lock()) w->wake();
            m_stream.reset();
            m_pending.reset();
            m_completed = true;
            this->mark_done();
            on_task_complete();
            return true;
        }

        // Flush a pending item that was buffered during backpressure.
        if (m_pending.has_value()) {
            std::weak_ptr<Waker> consumer_wk;
            {
                std::unique_lock lock(this->mutex);
                if (this->buffer.size() < this->capacity) {
                    this->buffer.push(std::move(*m_pending));
                    m_pending.reset();
                    consumer_wk = std::move(this->consumer_waker);
                } else {
                    this->producer_waker = ctx.get_weak_waker();
                    return false;
                }
            }
            if (auto w = consumer_wk.lock()) w->wake();
            // Fall through to poll the stream again.
        }

        // Drain the stream into the queue.
        while (true) {
            auto result = m_stream->poll_next(ctx);

            if (result.isPending()) return false;

            if (result.isError()) {
                std::weak_ptr<Waker> consumer_wk;
                {
                    std::lock_guard lock(this->mutex);
                    this->exception = result.error();
                    this->closed    = true;
                    consumer_wk = std::move(this->consumer_waker);
                }
                if (auto w = consumer_wk.lock()) w->wake();
                m_stream.reset();
                m_completed = true;
                this->mark_done();
                on_task_complete();
                return true;
            }

            auto opt = std::move(result).value();
            if (!opt.has_value()) {
                // Stream exhausted.
                std::weak_ptr<Waker> consumer_wk;
                {
                    std::lock_guard lock(this->mutex);
                    this->closed = true;
                    consumer_wk = std::move(this->consumer_waker);
                }
                if (auto w = consumer_wk.lock()) w->wake();
                m_stream.reset();
                m_completed = true;
                this->mark_done();
                on_task_complete();
                return true;
            }

            // Push item into the queue, or park under backpressure.
            std::weak_ptr<Waker> consumer_wk;
            {
                std::unique_lock lock(this->mutex);
                if (this->buffer.size() < this->capacity) {
                    this->buffer.push(std::move(*opt));
                    consumer_wk = std::move(this->consumer_waker);
                } else {
                    m_pending            = std::move(opt);
                    this->producer_waker = ctx.get_weak_waker();
                    return false;
                }
            }
            if (auto w = consumer_wk.lock()) w->wake();
            // Keep polling the stream.
        }
    }

private:
    std::optional<S>        m_stream;
    std::optional<ItemType> m_pending;
    bool                    m_completed = false;
    std::atomic<bool>       m_cancelled{false};
};

} // namespace coro::detail
