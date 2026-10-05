#pragma once

#include <coro/runtime/executor.h>
#include <coro/runtime/io_driver.h>
#include <coro/detail/owned_tasks.h>
#include <coro/detail/task.h>
#include <coro/detail/task_state.h>
#include <coro/detail/work_stealing_deque.h>
#ifdef CORO_USE_LOCAL_RUN_QUEUE
#include <coro/detail/local_run_queue.h>
#endif
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <latch>
#include <memory>
#include <mutex>
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
 * - **Per-worker parking**: the first idle worker parks in the Runtime's IoDriver
 *   (blocking in epoll); other idle workers park on their own condition variable.
 *   An idle bitmask (`m_idle_mask`) lets enqueuers pick a specific worker to wake.
 *   See doc/design/work_stealing_executor.md, "Parking in the I/O driver".
 * - **Task affinity**: `Task::last_worker_index` routes re-enqueued tasks back to
 *   their last worker's local queue, improving cache locality.
 *
 * **Worker limit**: `m_idle_mask` is a `uint64_t`, so the pool is capped at
 * `MAX_WORKERS` (64): a larger `num_threads`, including the hardware_concurrency()
 * default on a >64-core machine, is silently clamped to 64. Temporary; see
 * doc/roadmap.md, "WorkStealingExecutor: more than 64 workers".
 *
 * @see WorkSharingExecutor for the simpler reference implementation.
 */
class WorkStealingExecutor : public Executor {
public:
    static constexpr std::size_t MAX_WORKERS = 64;

    /// @param runtime     Back-pointer to the owning Runtime.
    /// @param num_threads Number of worker threads (default: hardware concurrency). Must be at least 2;
    ///                    values above MAX_WORKERS are clamped to MAX_WORKERS.
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

    /// Workers park in the driver and busy workers poll it; see park_worker().
    bool turns_io_driver() const noexcept override { return true; }

private:
#ifdef CORO_USE_LOCAL_RUN_QUEUE
    // shared_ptr<TaskBase> is stored by value directly in the ring buffer.
    // The head/tail Release/Acquire fences synchronize buffer slot access, so
    // no per-element atomics or boxing are needed.
    using TaskPtr = detail::TaskBase*;

    // One per worker: the owner-side Local handle plus a clonable Steal handle
    // that other workers call steal_into() on.
    struct WorkerQueue {
        detail::Local<TaskPtr> local;
        detail::Steal<TaskPtr> steal;

        WorkerQueue(detail::Local<TaskPtr> l, detail::Steal<TaskPtr> s)
            : local(std::move(l)), steal(std::move(s)) {}
        WorkerQueue(WorkerQueue&&) = default;
    };
#endif

    /// Where a worker is parked, or whether a wake token is banked for it.
    enum class ParkState {
        Empty,          ///< Not parked, no token.
        Notified,       ///< Token banked: the next park_worker() returns at once.
        ParkedDriver,   ///< Holds the driver; blocked (or about to block) in its poll().
        ParkedCondvar,  ///< Blocked on park_cv.
    };

    /// @brief One slot per worker thread. Not movable (mutex, condition variable).
    struct WorkerSlot {
        std::thread             thread;
        std::mutex              park_mutex;
        std::condition_variable park_cv;
        ParkState               park_state = ParkState::Empty; ///< GUARDED BY park_mutex.

        WorkerSlot() = default;
        WorkerSlot(const WorkerSlot&) = delete;
        WorkerSlot& operator=(const WorkerSlot&) = delete;
        WorkerSlot(WorkerSlot&&) = delete;
        WorkerSlot& operator=(WorkerSlot&&) = delete;
    };

    void worker_loop(int worker_index);

    /// @brief Wake one parked worker if no workers are currently searching.
    void notify_if_needed();

    /// @brief Parks worker `index` until unpark_worker(index), or until the driver
    /// returns if this worker got it.
    /// @return The number of I/O events dispatched (0 if this worker didn't turn).
    std::size_t park_worker(int index);

    /// @brief Wakes worker `index` wherever it is parked, or banks a token if it isn't.
    /// Safe from any thread.
    void unpark_worker(int index);

    /// @brief Number of tasks in worker `index`'s local queue. Owner thread only.
    std::size_t local_len(int index) const;

    /// @brief Called by worker `index` after a turn that dispatched `dispatched` events:
    /// wakes a peer if the turn left more than one task queued (tokio's
    /// should_notify_others).
    void after_turn(int index, std::size_t dispatched);

    // --- Per-worker state ---

    // Category 3 (see doc/task_ownership.md): temporary strong references held while
    // a task is Notified (in queue) or Running (local variable in worker loop).
    // Dropped when the task parks (Running → Idle). Must be shared_ptr — no other strong
    // reference keeps a Notified task alive between enqueue and the worker's poll call.
#ifdef CORO_USE_LOCAL_RUN_QUEUE
    std::vector<WorkerQueue>                                                   m_worker_queues;
#else
    std::vector<detail::WorkStealingDeque<detail::TaskBase*>> m_local_queues;
#endif
    std::vector<std::unique_ptr<WorkerSlot>>                                  m_workers;

    // --- Shared injection queue (remote enqueue / initial schedule) ---

    // Same category 3 reasoning as m_local_queues.
    std::deque<detail::TaskBase*>             m_injection_queue;
    std::mutex                                m_mutex; ///< Guards m_injection_queue and m_stop.
    /// m_injection_queue.size(), published so workers can skip m_mutex when the queue
    /// is empty. Written only under m_mutex; read without it. A zero read may be stale,
    /// so it must never be the last check before parking — that one takes m_mutex.
    std::atomic<std::size_t>                  m_injection_len{0};
    bool                                      m_stop{false};

    // --- Construction barrier ---

    /// Counted down to 0 by the constructor after all WorkerSlots are pushed.
    /// Each worker waits on this before reading m_workers.size().
    std::latch m_start_latch{1};

    // --- Parking / searching protocol ---

    /// Number of workers currently performing a steal sweep. Capped at num_workers/2.
    std::atomic<int>      m_searching{0};
    /// Bitmask of parked workers: bit k is set while worker k is parking or parked (park_worker()).
    std::atomic<uint64_t> m_idle_mask{0};

    Runtime*  m_runtime;
    /// Shared by all workers; owned by the Runtime, which declares it before the
    /// executor, so it outlives every worker.
    IoDriver& m_driver;

    /// Busy workers try a non-blocking turn every this many task polls.
    static constexpr unsigned kEventInterval = IoDriverParker::kDefaultEventInterval;

    // Category 1 (doc/task_ownership.md): persistent lifetime anchor for every live task.
    // Inserted in schedule(), removed after poll() returns true (task reached terminal state).
    // Sharded so that workers spawning and completing tasks do not serialize on one lock.
    detail::OwnedTasks m_owned_tasks;
};

} // namespace coro
