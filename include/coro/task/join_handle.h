#pragma once

#include <coro/detail/context.h>
#include <coro/future.h>
#include <coro/detail/poll_result.h>
#include <coro/detail/task_state.h>
#include <coro/detail/task.h>
#include <coro/runtime/executor.h>
#include <coro/detail/coro_scope.h>
#include <coro/detail/rc.h>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

namespace coro {

/**
 * @brief Owned handle to a spawned task. Returned by `spawn(...)`. Satisfies `Future<T>`.
 *
 * **Awaiting:** `co_await handle` suspends the caller until the task completes and returns its value.
 *
 * **Dropping:** destroying a `JoinHandle<T>` without `co_await`-ing it marks the task for
 * cancellation. If destroyed inside a coroutine's `poll()` call the enclosing
 * `CoroutineScope` records a `weak_ptr<TaskBase>` to the child and waits for it to drain
 * before the parent coroutine completes. Task lifetime is always anchored by the executor's
 * owned map — no ownership transfer is needed.
 *
 * **Detaching:** `std::move(handle).detach()` lets the task run to completion without
 * cancellation. The executor's owned map keeps it alive until it reaches a terminal state.
 *
 * **Empty state:** `JoinHandle()` default-constructs to an empty handle (`valid()` returns false).
 * Assigning a new handle from `spawn()` replaces the old one, cancelling any running task.
 * Assigning `{}` is equivalent to calling `stop()` — it cancels and drops the current task.
 *
 * `JoinHandle` is `[[nodiscard]]` — silently discarding it would cancel the task immediately,
 * which is almost always unintentional.
 *
 * @tparam T The value type produced by the spawned task.
 */
template<typename T>
class [[nodiscard]] JoinHandle {
public:
    using OutputType = T;

    /// @brief Constructs an empty (no-task) handle. valid() returns false.
    /// Useful as a member that starts unset and is later assigned from spawn().
    JoinHandle() noexcept = default;

    explicit JoinHandle(detail::Rc<detail::TaskState<T>> state,
                        detail::Weak<detail::TaskBase> task_ref = {})
        : m_state(std::move(state)), m_task_ref(std::move(task_ref)) {}

    JoinHandle(const JoinHandle&)            = delete;
    JoinHandle& operator=(const JoinHandle&) = delete;

    JoinHandle(JoinHandle&&) noexcept = default;

    // Hand-written rather than defaulted: a defaulted move-assignment would
    // just overwrite m_state/m_task_ref, dropping the old task reference
    // without running the destructor's cancel()-and-register-with-scope
    // protocol below. std::swap is NOT sufficient — if the right-hand side
    // is a named object moved via std::move rather than a genuine temporary,
    // swapping just stashes the old state inside that named object,
    // deferring the cancel until it happens to go out of scope. Explicitly
    // close the old handle via the same helper the destructor uses, then
    // take over the new state (e.g. `handle = spawn(...)` must cancel
    // handle's previous task right now, not silently let it keep running
    // until some unrelated variable's scope ends).
    JoinHandle& operator=(JoinHandle&& other) noexcept {
        if (this != &other) {
            close();
            m_cancelOnDestroy = other.m_cancelOnDestroy;
            m_state           = std::move(other.m_state);
            m_task_ref        = std::move(other.m_task_ref);
        }
        return *this;
    }

    /// @brief Returns true if this handle refers to a live task.
    /// False for a default-constructed handle or after detach().
    bool valid() const noexcept { return m_state != nullptr; }
    explicit operator bool() const noexcept { return valid(); }

    /// @brief Marks the task for cancellation and wakes it so it sees cancelled on its next poll.
    /// Satisfies the Cancellable concept — the enclosing Coro<T> cancel protocol calls this,
    /// then polls the JoinHandle until it returns non-Pending, ensuring the inner task fully
    /// drains (and its frame locals are destroyed) before the outer frame is torn down.
    ///
    /// Uses the same CAS loop as TaskBase::wake() to handle all scheduling states:
    /// - Idle           → Notified: task is parked; enqueue it so it sees cancelled on next poll.
    /// - Running        → RunningAndNotified: task is mid-poll; worker re-enqueues after poll.
    /// - Notified / RunningAndNotified / Done: already queued or finished — no-op.
    ///
    /// EDGE CASE: if m_task_ref is expired (handle moved-from or detached) or owning_executor is
    /// null (task never scheduled), cancellation is degraded: cancelled=true is set but the
    /// task is not actively woken. It will remain parked until naturally woken by whatever
    /// it awaits, at which point it will observe cancelled=true and self-terminate.
    void cancel() noexcept {
        if (!m_state) return;
        m_state->cancelled.store(true, std::memory_order_relaxed);
        auto task = m_task_ref.lock();
        if (!task) return;
        // owning_executor must always be set — see TaskImpl::cancel_task() for rationale.
        assert(task->owning_executor != nullptr);

        auto expected = detail::SchedulingState::Idle;
        while (true) {
            switch (expected) {
            case detail::SchedulingState::Idle:
                if (task->scheduling_state.compare_exchange_strong(
                        expected, detail::SchedulingState::Notified,
                        std::memory_order_acq_rel, std::memory_order_relaxed))
                {
                    task->owning_executor->enqueue(task);
                    return;
                }
                break;
            case detail::SchedulingState::Running:
                if (task->scheduling_state.compare_exchange_strong(
                        expected, detail::SchedulingState::RunningAndNotified,
                        std::memory_order_acq_rel, std::memory_order_relaxed))
                {
                    return;
                }
                break;
            case detail::SchedulingState::Notified:
            case detail::SchedulingState::RunningAndNotified:
            case detail::SchedulingState::Done:
                return;
            default:
                std::abort();
            }
        }
    }

    /// @brief Cancels the task and returns the handle for the caller to co_await.
    ///
    /// Calls cancel() then moves the handle out so the caller can co_await it
    /// directly — no intermediate coroutine frame, no extra allocation.
    ///
    ///   co_await std::move(*g_handle).cancel_and_join();
    ///   g_handle = spawn(new_task());
    ///
    /// The caller suspends until the task has fully drained (frame and all scope
    /// children destroyed), then resumes. For JoinHandle<void>, the co_await
    /// completes silently whether the task finished normally or was cancelled.
    [[nodiscard]] JoinHandle cancel_and_join() && {
        cancel();
        return std::move(*this);
    }

    /// @brief Cancels the task (if cancelOnDestroy) and registers with the enclosing scope.
    ///
    /// If inside a coroutine poll (t_current_coro != null), records a weak_ptr to the child
    /// in the scope's pending list so the parent waits for the child to drain.
    /// The executor's owned map keeps the task alive — no ownership transfer is needed.
    ~JoinHandle() { close(); }

    /// @brief Configures whether dropping this handle cancels the task (default: `true`).
    JoinHandle& cancelOnDestroy(bool b = true) & {
        m_cancelOnDestroy = b;
        return *this;
    }

    JoinHandle&& cancelOnDestroy(bool b = true) && {
        m_cancelOnDestroy = b;
        return std::forward<JoinHandle>(*this);
    }

    /// @brief Lets the task run without cancellation; the caller surrenders all synchronization.
    ///
    /// The executor's owned map already keeps the task alive until it reaches a terminal state.
    /// Detaching simply releases the JoinHandle's state reference and task_ref.
    JoinHandle& detach() & {
        m_state.reset();
        m_task_ref = {};
        return *this;
    }

    JoinHandle&& detach() && {
        detach();
        return std::forward<JoinHandle>(*this);
    }

    PollResult<T> poll(detail::Context& ctx) {
        std::lock_guard lock(m_state->mutex);
        // Check terminated first: covers normal completion, exception, and cancellation
        // (mark_done sets terminated without setting result, so checking result alone
        // would incorrectly leave a cancelled void task as Pending indefinitely).
        if (m_state->terminated) {
            if (m_state->exception)
                return PollError(m_state->exception);
            if constexpr (std::is_void_v<T>) {
                if (m_state->result) {
                    m_state->result = false;
                    return PollReady;
                }
            } else {
                if (m_state->result.has_value())
                    return std::move(*m_state->result);
            }
            return PollDropped; // task was cancelled before producing a result
        }
        // Not ready yet — store weak waker so the Task can wake us on completion.
        // weak_ptr avoids contributing to ownership cycles (see doc/task_ownership.md).
        m_state->waker = ctx.get_weak_waker();
        return PollPending;
    }

private:
    // Cancels the task (if cancelOnDestroy) and registers it with the
    // enclosing scope. Shared between the destructor and move-assignment so
    // both close out a held task through the same protocol — resets m_state
    // so a subsequent call (e.g. from the destructor, after move-assignment
    // already ran it) is a no-op.
    void close() {
        if (!m_state) return;  // moved-from or already cleaned up
        if (m_cancelOnDestroy) {
            cancel();
        }
        if (detail::t_current_coro) {
            detail::t_current_coro->add_child(m_task_ref);
        }
        m_state.reset();
        m_task_ref = {};
    }

    bool m_cancelOnDestroy = true;
    // Category 2 (doc/task_ownership.md): aliased shared_ptr into the same TaskImpl
    // allocation as m_task_ref. Provides typed access to the result and waker slot.
    detail::Rc<detail::TaskState<T>> m_state;
    // Category 4 (doc/task_ownership.md): notification/cancel only — no lifetime ownership.
    // The executor's owned map is the lifetime anchor (Category 1).
    detail::Weak<detail::TaskBase> m_task_ref;
};

/**
 * @brief Consumer end of a bounded queue backed by a background StreamTaskImpl task.
 *
 * Returned by `Runtime::spawn(stream)` and `SpawnBuilder::spawn(stream)`. Satisfies `Stream<T>`.
 *
 * Holds an aliased `shared_ptr<StreamTaskState<T>>` into the same `StreamTaskImpl<S>`
 * allocation as the executor's task reference — no separate channel allocation. The
 * background task polls the original stream and pushes items into the queue; `poll_next()`
 * dequeues them, waking the producer when space becomes available.
 *
 * Dropping the handle cancels the background task. The executor's owned set keeps it alive
 * until it drains.
 *
 * **Empty state:** `StreamHandle()` default-constructs to an empty handle (`valid()` returns
 * false). `poll_next()` on an empty handle immediately returns `nullopt`.
 *
 * `[[nodiscard]]` — discarding drops the consumer end; produced items are lost.
 *
 * @tparam T The item type yielded by the stream.
 */
template<typename T>
class [[nodiscard]] StreamHandle {
public:
    using ItemType = T;

    /// @brief Constructs an empty (no-task) handle. valid() returns false.
    StreamHandle() noexcept = default;

    explicit StreamHandle(detail::Rc<detail::StreamTaskState<T>> state,
                          detail::Weak<detail::TaskBase> task_ref = {})
        : m_state(std::move(state)), m_task_ref(std::move(task_ref)) {}

    bool valid() const noexcept { return m_state != nullptr; }
    explicit operator bool() const noexcept { return valid(); }

    /// @brief Cancels the background task, keeping the handle alive to drain buffered items.
    ///
    /// Items already in the queue remain readable via poll_next(); after they are consumed,
    /// poll_next() returns nullopt (clean end, no exception). Unlike dropping the handle,
    /// cancel() does not register with the enclosing CoroutineScope.
    void cancel() noexcept {
        if (auto task = m_task_ref.lock())
            task->cancel_task();
    }

    /// @brief Cancels the background task and registers with the enclosing CoroutineScope.
    ///
    /// If destroyed inside a coroutine's poll() call, records a weak_ptr to the background
    /// task in the scope's pending list so the parent coroutine waits for it to drain.
    /// Items remaining in the buffer are discarded — the consumer end is gone.
    ~StreamHandle() { close(); }

    StreamHandle(const StreamHandle&)            = delete;
    StreamHandle& operator=(const StreamHandle&) = delete;

    StreamHandle(StreamHandle&&) noexcept = default;

    // Hand-written rather than defaulted: a defaulted move-assignment would
    // just overwrite m_state/m_task_ref, dropping the old background task
    // reference without running the destructor's cancel_task()-and-register-
    // with-scope protocol below. std::swap is NOT sufficient — if the
    // right-hand side is a named object moved via std::move rather than a
    // genuine temporary, swapping just stashes the old state inside that
    // named object, deferring the cancel until it happens to go out of
    // scope, which may be arbitrarily late. Explicitly close the old handle
    // via the same helper the destructor uses, then take over the new state.
    StreamHandle& operator=(StreamHandle&& other) noexcept {
        if (this != &other) {
            close();
            m_state    = std::move(other.m_state);
            m_task_ref = std::move(other.m_task_ref);
        }
        return *this;
    }

    PollResult<std::optional<T>> poll_next(detail::Context& ctx) {
        if (!m_state) return std::optional<T>(std::nullopt);

        std::unique_lock lock(m_state->mutex);

        if (!m_state->buffer.empty()) {
            T item = std::move(m_state->buffer.front());
            m_state->buffer.pop();
            auto to_wake = std::move(m_state->producer_waker);
            lock.unlock();
            if (auto w = to_wake.lock()) w->wake();
            return std::optional<T>(std::move(item));
        }

        if (m_state->closed) {
            if (m_state->exception) return PollError(m_state->exception);
            return std::optional<T>(std::nullopt);
        }

        m_state->consumer_waker = ctx.get_weak_waker();
        return PollPending;
    }

private:
    // Cancels the background task and registers it with the enclosing scope.
    // Shared between the destructor and move-assignment so both close out a
    // held task through the same protocol — resets m_state so a subsequent
    // call (e.g. from the destructor, after move-assignment already ran it)
    // is a no-op.
    void close() {
        if (!m_state) return;  // moved-from or already cleaned up
        if (auto task = m_task_ref.lock())
            task->cancel_task();
        if (detail::t_current_coro)
            detail::t_current_coro->add_child(m_task_ref);
        m_state.reset();
        m_task_ref = {};
    }

    /// Aliased shared_ptr into the StreamTaskImpl allocation — queue access only.
    detail::Rc<detail::StreamTaskState<T>> m_state;
    /// Notification/cancel reference only — lifetime anchored by the executor's owned set.
    detail::Weak<detail::TaskBase> m_task_ref;
};

} // namespace coro
