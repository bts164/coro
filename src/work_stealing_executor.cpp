#include <coro/runtime/work_stealing_executor.h>
#include <coro/runtime/runtime.h>
#include <coro/runtime/single_threaded_uv_executor.h>
#include <coro/runtime/task_waker.h>
#include <coro/detail/context.h>
#include <bit>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace coro {

// Thread-locals identifying which WorkStealingExecutor owns this thread and
// which worker slot it occupies. Both are checked in enqueue() to route
// intra-executor wakeups to the local queue without a lock.
thread_local WorkStealingExecutor* t_wse_owning_executor = nullptr;
thread_local int                   t_wse_worker_index    = -1;


WorkStealingExecutor::WorkStealingExecutor(Runtime* runtime, std::size_t num_threads)
    : m_local_queues(num_threads)
    , m_runtime(runtime)
{
    if (num_threads > MAX_WORKERS)
        throw std::invalid_argument("WorkStealingExecutor: num_threads exceeds MAX_WORKERS (64)");

    m_workers.reserve(num_threads);
    for (std::size_t i = 0; i < num_threads; ++i) {
        m_workers.push_back(std::make_unique<WorkerSlot>());
        m_workers.back()->thread = std::thread([this, i] {
            worker_loop(static_cast<int>(i));
        });
    }
    // All WorkerSlots are now in m_workers. Let the workers proceed.
    m_start_latch.count_down();
}

WorkStealingExecutor::~WorkStealingExecutor() {
    {
        std::lock_guard lock(m_mutex);
        m_stop = true;
    }
    // Wake every parked worker so they observe m_stop and exit.
    for (auto& slot : m_workers)
        slot->parker.release();
    for (auto& slot : m_workers)
        slot->thread.join();
}

void WorkStealingExecutor::schedule(std::unique_ptr<detail::Task> task) {
    auto shared = std::shared_ptr<detail::Task>(std::move(task));
    shared->scheduling_state.store(
        detail::SchedulingState::Notified, std::memory_order_relaxed);
    enqueue(std::move(shared));
}

void WorkStealingExecutor::enqueue(std::shared_ptr<detail::Task> task) {
    const int local_idx = t_wse_worker_index;

    if (local_idx >= 0 && t_wse_owning_executor == this) {
        // Fast path: called from a worker of this executor.
        m_local_queues[local_idx].push(std::move(task));
    } else {
        const int affinity = task->last_worker_index;
        if (affinity >= 0 && affinity < static_cast<int>(m_local_queues.size())) {
            // Affinity path: re-enqueue to the worker that last ran this task.
            m_local_queues[affinity].push(std::move(task));
        } else {
            // Remote path: injection queue.
            std::lock_guard lock(m_mutex);
            m_injection_queue.push_back(std::move(task));
        }
    }
    notify_if_needed();
}

void WorkStealingExecutor::wait_for_completion(detail::TaskStateBase& state) {
    state.wait_until_done();
}

void WorkStealingExecutor::notify_if_needed() {
    // If at least one worker is already searching it will find the new task.
    if (m_searching.load(std::memory_order_acquire) > 0) return;
    // Otherwise wake one parked worker to begin searching.
    const uint64_t idle = m_idle_mask.load(std::memory_order_acquire);
    if (idle) {
        const int idx = std::countr_zero(idle);
        m_workers[idx]->parker.release();
    }
}

void WorkStealingExecutor::worker_loop(int worker_index) {
    // Wait until the constructor has finished pushing all WorkerSlots so that
    // m_workers.size() is stable before we read it. RACE: reading m_workers
    // while the constructor is still push_back()-ing would be undefined.
    m_start_latch.wait();

    t_wse_owning_executor = this;
    t_wse_worker_index    = worker_index;
    set_current_runtime(m_runtime);
    set_current_uv_executor(&m_runtime->uv_executor());

    const int    n            = static_cast<int>(m_workers.size());
    const int    max_search   = std::max(1, n / 2);

    while (true) {
        std::shared_ptr<detail::Task> task;

        // --- Step 1: own local queue ---
        if (auto t = m_local_queues[worker_index].pop()) {
            task = std::move(*t);
        }

        // --- Step 2: injection queue ---
        if (!task) {
            std::lock_guard lock(m_mutex);
            if (!m_injection_queue.empty()) {
                task = std::move(m_injection_queue.front());
                m_injection_queue.pop_front();
            }
        }

        // --- Step 3: enter searching (bounded) and steal ---
        if (!task) {
            const int cur = m_searching.load(std::memory_order_acquire);
            if (cur < max_search &&
                m_searching.compare_exchange_strong(
                    const_cast<int&>(cur), cur + 1,
                    std::memory_order_acq_rel))
            {
                // Steal sweep: try each peer once.
                for (int i = 0; i < n && !task; ++i) {
                    const int victim = (worker_index + 1 + i) % n;
                    if (victim == worker_index) continue;
                    if (auto t = m_local_queues[victim].steal_half(
                                     m_local_queues[worker_index])) {
                        task = std::move(*t);
                    }
                }

                // Re-check injection queue after sweep.
                if (!task) {
                    std::lock_guard lock(m_mutex);
                    if (!m_injection_queue.empty()) {
                        task = std::move(m_injection_queue.front());
                        m_injection_queue.pop_front();
                    }
                }

                m_searching.fetch_sub(1, std::memory_order_release);
            }
        }

        // --- Step 4: park if still nothing to do ---
        if (!task) {
            // Set idle bit, then re-check all sources to close the lost-wakeup window.
            m_idle_mask.fetch_or(1ull << worker_index, std::memory_order_release);

            // Re-check local queue.
            if (auto t = m_local_queues[worker_index].pop())
                task = std::move(*t);

            // Re-check injection queue.
            if (!task) {
                std::lock_guard lock(m_mutex);
                if (m_stop && m_injection_queue.empty()) {
                    // Drain local queue one last time, then exit.
                    m_idle_mask.fetch_and(~(1ull << worker_index),
                                         std::memory_order_relaxed);
                    break;
                }
                if (!m_injection_queue.empty()) {
                    task = std::move(m_injection_queue.front());
                    m_injection_queue.pop_front();
                }
            }

            if (!task) {
                // Truly nothing — park. If a token was banked by notify_if_needed()
                // after we set the idle bit, acquire() returns immediately.
                m_workers[worker_index]->parker.acquire();
                m_idle_mask.fetch_and(~(1ull << worker_index),
                                      std::memory_order_relaxed);

                // Check shutdown after waking.
                {
                    std::lock_guard lock(m_mutex);
                    if (m_stop && m_injection_queue.empty() &&
                        m_local_queues[worker_index].empty())
                        break;
                }
                continue;
            } else {
                m_idle_mask.fetch_and(~(1ull << worker_index),
                                      std::memory_order_relaxed);
            }
        }

        // --- Run the task ---

        auto expected = detail::SchedulingState::Notified;
        if (!task->scheduling_state.compare_exchange_strong(
                expected, detail::SchedulingState::Running,
                std::memory_order_acq_rel,
                std::memory_order_relaxed))
        {
            std::cerr << "[coro] WorkStealingExecutor: unexpected scheduling_state "
                      << static_cast<int>(expected)
                      << " during Notified→Running (expected Notified=2)\n";
            std::abort();
        }

        task->last_worker_index = worker_index;

        auto waker = std::make_shared<TaskWaker>();
        waker->task     = task;
        waker->executor = this;
        detail::Context ctx(waker);
        detail::Task::current = task.get();
        const bool done = task->poll(ctx);
        detail::Task::current = nullptr;

        if (done) {
            task->scheduling_state.store(
                detail::SchedulingState::Done, std::memory_order_relaxed);
            task.reset();
        } else {
            // Try Running → Idle.
            expected = detail::SchedulingState::Running;
            if (task->scheduling_state.compare_exchange_strong(
                    expected, detail::SchedulingState::Idle,
                    std::memory_order_acq_rel,
                    std::memory_order_relaxed))
            {
                task.reset(); // waker holds the only ref
            } else {
                // CAS failed: expected now holds the actual state. The only valid
                // state here is RunningAndNotified — wake() fired during poll().
                if (expected != detail::SchedulingState::RunningAndNotified) {
                    std::cerr << "[coro] WorkStealingExecutor: unexpected scheduling_state "
                              << static_cast<int>(expected)
                              << " after Running→Idle CAS failure (expected RunningAndNotified=3)\n";
                    std::abort();
                }
                if (!task->scheduling_state.compare_exchange_strong(
                        expected, detail::SchedulingState::Notified,
                        std::memory_order_acq_rel,
                        std::memory_order_relaxed))
                {
                    std::cerr << "[coro] WorkStealingExecutor: unexpected scheduling_state "
                              << static_cast<int>(expected)
                              << " during RunningAndNotified→Notified\n";
                    std::abort();
                }
                enqueue(std::move(task));
            }
        }
    }

    set_current_runtime(nullptr);
    set_current_uv_executor(nullptr);
    t_wse_owning_executor = nullptr;
    t_wse_worker_index    = -1;
}

} // namespace coro
