#pragma once

#include <coro/runtime/executor.h>
#include <coro/detail/task.h>
#include <coro/detail/task_state.h>
#include <coro/detail/work_stealing_deque.h>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <latch>
#include <memory>
#include <mutex>
#include <semaphore>
#include <thread>
#include <vector>

namespace coro {

class Runtime;

/**
 * @brief Multi-threaded @ref Executor with per-worker local queues and work stealing.
 *
 * Improves on @ref WorkSharingExecutor by allowing idle workers to steal tasks from
 * busy workers' local queues before parking, reducing idle time under uneven load.
 *
 * Key design differences from WorkSharingExecutor:
 * - **Stealing**: idle workers attempt `steal_half()` from peers before parking.
 * - **Bounded searching**: at most `num_workers / 2` workers search simultaneously,
 *   preventing thundering herd on the victim queues.
 * - **Per-worker parking**: each worker parks on its own `std::binary_semaphore`
 *   instead of a shared condition variable. An idle bitmask (`m_idle_mask`) lets
 *   enqueuers wake a specific worker without acquiring any lock.
 * - **Task affinity**: `Task::last_worker_index` routes re-enqueued tasks back to
 *   their last worker's local queue, improving cache locality.
 *
 * **Worker limit**: `m_idle_mask` is a `uint64_t`; pools larger than 64 workers are
 * rejected at construction time via `static_assert` / runtime check.
 *
 * @see WorkSharingExecutor for the simpler reference implementation.
 */
class WorkStealingExecutor : public Executor {
public:
    static constexpr std::size_t MAX_WORKERS = 64;

    /// @param runtime     Back-pointer to the owning Runtime.
    /// @param num_threads Number of worker threads (default: hardware concurrency). Must be in [2, MAX_WORKERS].
    WorkStealingExecutor(Runtime* runtime, std::size_t num_threads = std::thread::hardware_concurrency());
    ~WorkStealingExecutor() override;

    WorkStealingExecutor(const WorkStealingExecutor&)            = delete;
    WorkStealingExecutor& operator=(const WorkStealingExecutor&) = delete;

    /// @brief Submit a new task. Routes to injection queue then wakes a worker.
    void schedule(std::shared_ptr<detail::TaskBase> task) override;

    /// @brief Re-enqueue a woken task.
    ///
    /// Routing priority:
    /// 1. Caller is a worker of this executor → own local queue.
    /// 2. Task carries a valid affinity hint (`last_worker_index`) → that worker's queue.
    /// 3. Otherwise → shared injection queue.
    ///
    /// After routing, calls `notify_if_needed()` to wake a parked worker if no
    /// workers are currently searching.
    void enqueue(std::shared_ptr<detail::TaskBase> task) override;

    /// @brief Delegates to `state.wait_until_done()`.
    void wait_for_completion(detail::TaskStateBase& state) override;

private:
    /// @brief One slot per worker thread. Not movable due to binary_semaphore.
    struct WorkerSlot {
        std::thread           thread;
        std::binary_semaphore parker{0}; ///< 0 = no pending wake token.

        WorkerSlot() = default;
        WorkerSlot(const WorkerSlot&) = delete;
        WorkerSlot& operator=(const WorkerSlot&) = delete;
        WorkerSlot(WorkerSlot&&) = delete;
        WorkerSlot& operator=(WorkerSlot&&) = delete;
    };

    void worker_loop(int worker_index);

    /// @brief Wake one parked worker if no workers are currently searching.
    void notify_if_needed();

    // --- Per-worker state ---

    std::vector<detail::WorkStealingDeque<std::shared_ptr<detail::TaskBase>>> m_local_queues;
    std::vector<std::unique_ptr<WorkerSlot>>                                  m_workers;

    // --- Shared injection queue (remote enqueue / initial schedule) ---

    std::deque<std::shared_ptr<detail::TaskBase>> m_injection_queue;
    std::mutex                                m_mutex; ///< Guards m_injection_queue and m_stop.
    bool                                      m_stop{false};

    // --- Construction barrier ---

    /// Counted down to 0 by the constructor after all WorkerSlots are pushed.
    /// Each worker waits on this before reading m_workers.size().
    std::latch m_start_latch{1};

    // --- Parking / searching protocol ---

    /// Number of workers currently performing a steal sweep. Capped at num_workers/2.
    std::atomic<int>      m_searching{0};
    /// Bitmask of parked workers: bit k is set while worker k is in parker.acquire().
    std::atomic<uint64_t> m_idle_mask{0};

    Runtime* m_runtime;
};

} // namespace coro
