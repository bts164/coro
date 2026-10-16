#pragma once

#ifndef CORO_PICO
#include <cstddef>
#include <vector>
#endif

namespace coro {
class WorkStealingExecutor;
}

namespace coro::detail {

class TaskBase;

/**
 * @brief Collects the wakes made on this thread while it is open, and hands them to
 * the executor in one go when it closes. Internal.
 *
 * On `WorkStealingExecutor`, a task woken while a batch is open is not pushed and
 * notified for on its own; closing the batch pushes every collected task to the
 * injection queue under one lock and wakes as many parked workers as it has tasks.
 * On every other executor, and on Pico, it does nothing, so callers open one
 * unconditionally.
 *
 * See doc/design/work_stealing_executor.md, "Batched wakes (`WakeBatch`)".
 *
 * WARNING: never hold a batch across a suspension. The batch is per thread, not per
 * task: open it as a stack local inside a future's `poll()`, or in a coroutine close
 * it before the next `co_await`, as with a held mutex.
 */
#ifdef CORO_PICO
class WakeBatch {
public:
    WakeBatch() noexcept = default;
    WakeBatch(const WakeBatch&)            = delete;
    WakeBatch& operator=(const WakeBatch&) = delete;
};
#else
class WakeBatch {
public:
    /// Opens the batch on this thread. A batch already open is set aside and
    /// restored when this one closes.
    WakeBatch() noexcept;
    /// Hands the collected tasks to their executor and restores the batch that was
    /// open before. Runs during unwinding too: dropping the tasks would lose wakes.
    ~WakeBatch();

    WakeBatch(const WakeBatch&)            = delete;
    WakeBatch& operator=(const WakeBatch&) = delete;

    /// True if a batch is open on this thread. For debug assertions.
    static bool is_open() noexcept;

private:
    friend class coro::WorkStealingExecutor;

    /// Tasks held without a vector allocation. Enough for one per worker.
    static constexpr std::size_t kInlineCapacity = 64;

    /// Appends `task` if this batch belongs to `executor`, binding it to `executor` on
    /// the first task. Returns false (and takes nothing) for another executor's task.
    bool try_add(WorkStealingExecutor* executor, TaskBase* task);

    WakeBatch*            m_prev;
    WorkStealingExecutor* m_executor = nullptr;
    std::size_t           m_count    = 0;
    // The owned-task list keeps each task alive, as for the run queues.
    TaskBase*             m_inline[kInlineCapacity];
    std::vector<TaskBase*> m_overflow;
};
#endif

} // namespace coro::detail
