#include <coro/runtime/work_stealing_executor.h>
#include <coro/runtime/runtime.h>
#include <coro/runtime/single_threaded_uv_executor.h>
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

#ifdef CORO_USE_LOCAL_RUN_QUEUE
using TaskSP = detail::TaskBase*;

// Overflow handler for Local<TaskSP>::push_or_overflow().
// Moves spilled tasks directly into the injection queue — no boxing needed.
struct InjectionOverflow {
    std::mutex&           mutex;
    std::deque<TaskSP>&   queue;

    void push(TaskSP task) {
        std::lock_guard lock(mutex);
        queue.push_back(std::move(task));
    }

    void push_batch(TaskSP* tasks, std::size_t n) {
        std::lock_guard lock(mutex);
        for (std::size_t i = 0; i < n; ++i)
            queue.push_back(std::move(tasks[i]));
    }
};
#endif

WorkStealingExecutor::WorkStealingExecutor(Runtime* runtime, std::size_t num_threads) :
#ifndef CORO_USE_LOCAL_RUN_QUEUE
    m_local_queues(num_threads),
#endif
    m_runtime(runtime)
{
    if (num_threads > MAX_WORKERS)
        throw std::invalid_argument("WorkStealingExecutor: num_threads exceeds MAX_WORKERS (64)");

#ifdef CORO_USE_LOCAL_RUN_QUEUE
    m_worker_queues.reserve(num_threads);
    for (std::size_t i = 0; i < num_threads; ++i) {
        auto [steal, local] = detail::make_local_run_queue<detail::TaskBase*>();
        m_worker_queues.emplace_back(std::move(local), std::move(steal));
    }
#endif

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

void WorkStealingExecutor::schedule(std::shared_ptr<detail::TaskBase> task) {
    task->owning_executor = this;
    task->scheduling_state.store(
        detail::SchedulingState::Notified, std::memory_order_relaxed);
    enqueue(std::move(task));
}

void WorkStealingExecutor::enqueue(std::shared_ptr<detail::TaskBase> task) {
    const int local_idx = t_wse_worker_index;

#ifdef CORO_USE_LOCAL_RUN_QUEUE
    if (local_idx >= 0 && t_wse_owning_executor == this) {
        // Fast path: push to own local queue; spill to injection queue if full.
        InjectionOverflow overflow{m_mutex, m_injection_queue};
        m_worker_queues[local_idx].local.push_or_overflow(task.get(), overflow);
    } else {
        // Local<T> is single-owner so we cannot push directly to another
        // worker's ring from here. All remote pushes go to the injection queue.
        std::lock_guard lock(m_mutex);
        m_injection_queue.push_back(task.get());
    }
#else
    if (local_idx >= 0 && t_wse_owning_executor == this) {
        // Fast path: called from a worker of this executor.
        m_local_queues[local_idx].push(task.get());
    } else {
        const int affinity = task->last_worker_index;
        if (affinity >= 0 && affinity < static_cast<int>(m_local_queues.size())) {
            // Affinity path: re-enqueue to the worker that last ran this task.
            m_local_queues[affinity].push(task.get());
        } else {
            // Remote path: injection queue.
            std::lock_guard lock(m_mutex);
            m_injection_queue.push_back(task.get());
        }
    }
#endif
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
        std::shared_ptr<detail::TaskBase> task;

        // --- Step 1: own local queue ---
#ifdef CORO_USE_LOCAL_RUN_QUEUE
        if (auto t = m_worker_queues[worker_index].local.pop())
            task = t->shared_from_this();
#else
        if (auto t = m_local_queues[worker_index].pop()) {
            task = (*t)->shared_from_this();
        }
#endif

        // --- Step 2: injection queue ---
        if (!task) {
            std::lock_guard lock(m_mutex);
            if (!m_injection_queue.empty()) {
                task = std::move(m_injection_queue.front()->shared_from_this());
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
#ifdef CORO_USE_LOCAL_RUN_QUEUE
                    if (auto t = m_worker_queues[victim].steal.steal_into(
                                     m_worker_queues[worker_index].local))
                        task = t->shared_from_this();
#else
                    if (auto t = m_local_queues[victim].steal_half(
                                     m_local_queues[worker_index])) {
                        task = (*t)->shared_from_this();
                    }
#endif
                }

                // Re-check injection queue after sweep.
                if (!task) {
                    std::lock_guard lock(m_mutex);
                    if (!m_injection_queue.empty()) {
                        task = m_injection_queue.front()->shared_from_this();
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
#ifdef CORO_USE_LOCAL_RUN_QUEUE
            if (auto t = m_worker_queues[worker_index].local.pop())
                task = t->shared_from_this();
#else
            if (auto t = m_local_queues[worker_index].pop())
                task = (*t)->shared_from_this();
#endif

            // Re-check injection queue.
            if (!task) {
                std::lock_guard lock(m_mutex);
#ifdef CORO_USE_LOCAL_RUN_QUEUE
                if (m_stop && m_injection_queue.empty() &&
                        m_worker_queues[worker_index].local.len() == 0) {
#else
                if (m_stop && m_injection_queue.empty()) {
#endif
                    m_idle_mask.fetch_and(~(1ull << worker_index),
                                         std::memory_order_relaxed);
                    break;
                }
                if (!m_injection_queue.empty()) {
                    task = std::move(m_injection_queue.front()->shared_from_this());
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
#ifdef CORO_USE_LOCAL_RUN_QUEUE
                    if (m_stop && m_injection_queue.empty() &&
                            m_worker_queues[worker_index].local.len() == 0)
#else
                    if (m_stop && m_injection_queue.empty() &&
                            m_local_queues[worker_index].empty())
#endif
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

        detail::Context ctx(std::static_pointer_cast<detail::Waker>(task));
        detail::TaskBase::current = task.get();
        const bool done = task->poll(ctx);
        detail::TaskBase::current = nullptr;

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
                task.reset(); // release temporary executor ref; task lives via OwnedTask
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
