#pragma once

// Internal — not part of the public API.
// Sharded lifetime anchor for the tasks a multi-threaded executor owns.
// See doc/design/work_stealing_executor.md, "Owned tasks".

#include <coro/detail/task.h>
#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace coro::detail {

/**
 * @brief Keeps every live task of an executor alive from `schedule()` until it reaches
 * a terminal state. Modeled on tokio's `OwnedTasks` / `ShardedList`.
 *
 * A single mutex around one container serializes every spawn and every completion
 * across all workers. Here the tasks are spread over a power-of-two number of shards,
 * each an intrusive doubly-linked list behind its own mutex, so two workers contend
 * only when their tasks hash to the same shard.
 *
 * The list is intrusive: the links (`TaskBase::owned_prev` / `owned_next`) and the
 * strong reference the list holds (`TaskBase::owned_self`) live in the task's own
 * allocation. `insert()` and `remove()` are a few pointer writes under the shard lock,
 * with no allocation and no hashing of a container.
 *
 * ### Thread safety
 * `insert()` and `remove()` are safe from any thread. A task's shard is derived from
 * its address, which never changes, so both always take the same lock for a given task.
 *
 * ### Closing
 * `close_and_collect()` marks every shard closed and hands back the tasks linked at
 * that moment, for runtime shutdown to cancel. From then on `insert()` and `remove()`
 * report that the list is closed, so the executor can cancel a task spawned during
 * shutdown and tell the runtime each time the list shrinks. The flag lives in the
 * shard, under the shard mutex the caller already takes, so closing adds no shared
 * state to the spawn and completion paths. See doc/design/runtime_shutdown.md.
 *
 * Differences from tokio: there is no live-task counter; `empty()` walks the shards,
 * and only shutdown calls it.
 */
class OwnedTasks {
public:
    /// Upper bound on the shard count (tokio's MAX_SHARDED_LIST_SIZE).
    static constexpr std::size_t kMaxShards = std::size_t{1} << 16;

    /// @param num_workers Worker threads of the owning executor. The shard count is
    ///                    four times that, rounded up to a power of two.
    explicit OwnedTasks(std::size_t num_workers)
        : m_shard_mask(shard_count(num_workers) - 1)
        , m_shards(std::make_unique<Shard[]>(m_shard_mask + 1)) {}

    /// @brief Releases every task still owned. The owning executor must have stopped
    /// its workers first: no other thread may call insert() or remove() concurrently
    /// with destruction of the list itself.
    ~OwnedTasks() {
        for (std::size_t i = 0; i <= m_shard_mask; ++i) {
            for (;;) {
                // Released outside the lock: a task's destructor may run arbitrary
                // user destructors, which must not execute under a shard mutex.
                std::shared_ptr<TaskBase> released;
                {
                    std::lock_guard lock(m_shards[i].mutex);
                    TaskBase* task = m_shards[i].head;
                    if (!task) break;
                    unlink(m_shards[i], *task);
                    released = std::move(task->owned_self);
                }
            }
        }
    }

    OwnedTasks(const OwnedTasks&)            = delete;
    OwnedTasks& operator=(const OwnedTasks&) = delete;

    /// @brief Takes a strong reference to `task` and links it into its shard.
    /// Must be called at most once per task, before the task is first enqueued.
    /// @return True if the list is closed. The task is linked all the same; the caller
    ///         cancels it.
    bool insert(std::shared_ptr<TaskBase> task) {
        TaskBase& t     = *task;
        Shard&    shard = shard_for(t);
        std::lock_guard lock(shard.mutex);
        t.owned_prev = nullptr;
        t.owned_next = shard.head;
        if (shard.head) shard.head->owned_prev = &t;
        shard.head   = &t;
        t.owned_self = std::move(task);
        return shard.closed;
    }

    /// What remove() hands back.
    struct Removed {
        /// The strong reference the list held, or null if the task was not linked.
        /// Returned, not dropped inside remove(), so that the caller releases it
        /// outside the shard lock.
        std::shared_ptr<TaskBase> task;
        /// True if the list was closed when the task was unlinked.
        bool closed = false;
    };

    /// @brief Unlinks `task`.
    [[nodiscard]] Removed remove(TaskBase& task) noexcept {
        Shard& shard = shard_for(task);
        std::lock_guard lock(shard.mutex);
        // owned_self doubles as the "is linked" flag; it is only read or written
        // under this shard's mutex.
        if (!task.owned_self) return {nullptr, shard.closed};
        unlink(shard, task);
        return {std::move(task.owned_self), shard.closed};
    }

    /// @brief Closes the list and returns a strong reference to every task linked in
    /// it. The tasks stay linked; each is removed as usual when it finishes.
    ///
    /// RACE: an insert() racing with this either links its task before its shard is
    /// closed, and the task is in the returned vector, or after, and insert() returns
    /// true. No task is in neither group.
    [[nodiscard]] std::vector<std::shared_ptr<TaskBase>> close_and_collect() {
        std::vector<std::shared_ptr<TaskBase>> tasks;
        for (std::size_t i = 0; i <= m_shard_mask; ++i) {
            std::lock_guard lock(m_shards[i].mutex);
            m_shards[i].closed = true;
            for (TaskBase* task = m_shards[i].head; task; task = task->owned_next)
                tasks.push_back(task->owned_self);
        }
        return tasks;
    }

    /// @brief True if no task is linked. Locks each shard in turn, so the answer is a
    /// snapshot only if the caller has stopped tasks from being inserted meanwhile;
    /// see Runtime::shutdown().
    [[nodiscard]] bool empty() const {
        for (std::size_t i = 0; i <= m_shard_mask; ++i) {
            std::lock_guard lock(m_shards[i].mutex);
            if (m_shards[i].head) return false;
        }
        return true;
    }

private:
    // One cache line per shard, so workers locking neighbouring shards do not
    // false-share.
    struct alignas(64) Shard {
        mutable std::mutex mutex;
        TaskBase*          head   = nullptr; ///< GUARDED BY mutex.
        bool               closed = false;   ///< GUARDED BY mutex. Set once, never cleared.
    };

    static std::size_t shard_count(std::size_t num_workers) {
        return std::min(kMaxShards, std::bit_ceil(std::max<std::size_t>(num_workers, 1)) * 4);
    }

    Shard& shard_for(const TaskBase& task) const noexcept {
        // Tasks are cache-line aligned, so the low address bits carry no information.
        // Fibonacci hashing spreads allocator-adjacent tasks across the shards.
        const auto addr = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(&task));
        const auto hash = ((addr >> 6) * 0x9E3779B97F4A7C15ull) >> 32;
        return m_shards[static_cast<std::size_t>(hash) & m_shard_mask];
    }

    /// Caller holds shard.mutex and `task` is linked in `shard`.
    static void unlink(Shard& shard, TaskBase& task) noexcept {
        if (task.owned_prev) task.owned_prev->owned_next = task.owned_next;
        else                 shard.head                  = task.owned_next;
        if (task.owned_next) task.owned_next->owned_prev = task.owned_prev;
        task.owned_prev = task.owned_next = nullptr;
    }

    std::size_t              m_shard_mask;
    std::unique_ptr<Shard[]> m_shards;
};

} // namespace coro::detail
