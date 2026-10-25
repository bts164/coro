#pragma once

#include <coro/detail/rc.h>
#include <coro/detail/task.h>
#include <cassert>
#include <cstdint>
#include <stdexcept>
#include <utility>

namespace coro {

/**
 * @brief A copyable handle that can only cancel a task, like Tokio's `AbortHandle`.
 *
 * Returned by `JoinSet::spawn()` and `JoinSetSpawnBuilder::spawn()`, and by the
 * `cancel_handle()` accessor of `JoinHandle`, `StreamHandle` and `BlockingHandle`. The
 * task's own handle stays its only owner and the only way to get its result; a
 * `CancelHandle` just lets other code ask the task to stop. The main use is a detached
 * task that should still be stoppable: take a `CancelHandle` first, then `detach()`.
 *
 * ```cpp
 * auto handle = coro::spawn(serve(conn));
 * coro::CancelHandle stop = handle.cancel_handle();
 * std::move(handle).detach();
 * // ... later:
 * stop.cancel();
 * ```
 *
 * **State:** the handle keeps its own, `Empty`, `Tracking` or `Finished`, and every answer
 * comes from it. The task pointer is used only while `Tracking`, to cancel the task or ask
 * whether it has ended, and is released the moment the handle sees the task has ended, so
 * a handle kept around does not pin the task's memory.
 *
 * - `Empty`: default-constructed, moved-from, or taken from a handle with no task.
 *   `cancel()` does nothing and `is_finished()` throws.
 * - `Tracking`: the task may still be running.
 * - `Finished`: the task has ended, with a value, an exception or cancelled.
 *
 * **Cancelling** has the same effect as `JoinHandle::cancel()`: the task ends as cancelled
 * at its next poll, and its handle or JoinSet reports it so. For a blocking task it is a
 * request, seen at the callable's next cancellation point. A cancel that arrives after the
 * task has ended changes nothing: the task reports its own result.
 *
 * Dropping a `CancelHandle` never cancels, so discarding one is harmless. It is not a
 * `Future` and does not satisfy `Cancellable`.
 *
 * **Threads:** copies may be used on any threads at once, but one object may not: `cancel()`
 * and `is_finished()` are `const` yet may release the pointer, which races with another
 * call on the same object. Give each thread its own copy; copying is cheap.
 */
class CancelHandle {
public:
    /// @brief Constructs an `Empty` handle.
    CancelHandle() noexcept = default;

    /// @brief A handle to `task`: `Tracking`, or `Finished` if the caller already knows
    /// the task has ended, in which case the pointer is not kept.
    explicit CancelHandle(detail::Weak<detail::TaskBase> task, bool finished = false) noexcept
        : m_state(finished ? State::Finished : State::Tracking) {
        if (!finished) m_task = std::move(task);
    }

    CancelHandle(const CancelHandle&)            = default;
    CancelHandle& operator=(const CancelHandle&) = default;

    // Hand-written: a moved-from handle is Empty, not left in its old state with no task.
    CancelHandle(CancelHandle&& other) noexcept
        : m_state(std::exchange(other.m_state, State::Empty)),
          m_task(std::move(other.m_task)) {
        other.m_task.reset();
    }

    CancelHandle& operator=(CancelHandle&& other) noexcept {
        if (this != &other) {
            m_state = std::exchange(other.m_state, State::Empty);
            m_task  = std::move(other.m_task);
            other.m_task.reset();
        }
        return *this;
    }

    /// @brief True unless the handle is `Empty`.
    [[nodiscard]] bool valid() const noexcept { return m_state != State::Empty; }
    explicit operator bool() const noexcept { return valid(); }

    /**
     * @brief Whether the task has ended, with a value, an exception or cancelled.
     *
     * @throws std::logic_error if the handle is `Empty`: there is no task to answer for.
     */
    [[nodiscard]] bool is_finished() const {
        assert_pointer_only_while_tracking();
        switch (m_state) {
        case State::Empty:
            throw std::logic_error("CancelHandle::is_finished() called on an empty handle");
        case State::Finished:
            return true;
        case State::Tracking:
            break;
        }
        // An expired pointer means the task has ended: the executor holds a task until then.
        auto task = m_task.lock();
        if (!task || task->is_complete()) {
            finish();
            return true;
        }
        return false;
    }

    /**
     * @brief Asks the task to stop. Does nothing if the handle is `Empty` or the task
     * has ended. Safe from any thread, on this thread's own copy.
     *
     * RACE: the task may end at any moment relative to this call; then the request is
     * ignored and the task reports its own result.
     */
    void cancel() const noexcept {
        assert_pointer_only_while_tracking();
        if (m_state != State::Tracking) return;
        auto task = m_task.lock();
        if (!task || task->is_complete()) {
            finish();
            return;
        }
        // The pointer is kept: until the task stops, is_finished() still needs it.
        task->cancel_task();
    }

private:
    enum class State : std::uint8_t { Empty, Tracking, Finished };

    // The task can never be needed again: drop the pointer, which frees the task's
    // memory once nothing else holds it.
    //
    // RACE: mutates through a const method. Two threads calling cancel() or
    // is_finished() on the same object race here; each thread must use its own copy.
    void finish() const noexcept {
        m_state = State::Finished;
        m_task.reset();
    }

    void assert_pointer_only_while_tracking() const noexcept {
        assert(m_state == State::Tracking
               || detail::same_rc(m_task, detail::Weak<detail::TaskBase>{}));
    }

    mutable State                          m_state = State::Empty;
    mutable detail::Weak<detail::TaskBase> m_task;  ///< Engaged only while Tracking.
};

} // namespace coro
