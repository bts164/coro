#include <coro/runtime/work_stealing_executor.h>
#include <coro/runtime/runtime.h>
#include <coro/detail/context.h>
#include <algorithm>
#include <bit>
#include <cstdlib>
#include <iostream>

namespace coro {

// Thread-locals identifying which WorkStealingExecutor owns this thread and
// which worker slot it occupies. Both are checked in enqueue() to route
// intra-executor wakeups to the local queue without a lock.
thread_local WorkStealingExecutor* t_wse_owning_executor = nullptr;
thread_local int                   t_wse_worker_index    = -1;

namespace {

// True while this worker thread is inside IoDriver::try_turn(). Wakes the driver
// dispatches then are local enqueues that skip notify_if_needed(): this worker runs
// them itself as soon as the turn returns (after_turn() wakes a peer if it got more
// than one). See doc/design/work_stealing_executor.md, "Parking and its races".
thread_local bool t_wse_in_driver_turn = false;

struct InDriverTurn {
    InDriverTurn()  { t_wse_in_driver_turn = true; }
    ~InDriverTurn() { t_wse_in_driver_turn = false; }
    InDriverTurn(const InDriverTurn&)            = delete;
    InDriverTurn& operator=(const InDriverTurn&) = delete;
};

} // namespace

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
    m_local_queues(std::min(num_threads, MAX_WORKERS)),
#endif
    m_runtime(runtime),
    m_driver(runtime->io_driver())
{
    // TODO: temporary cap. m_idle_mask is one uint64_t, so extra workers are dropped
    // rather than failing construction on >64-core machines, where the default
    // (hardware_concurrency) would otherwise throw. Lifting it means a multi-word
    // idle bitmap; see doc/roadmap.md, "WorkStealingExecutor: more than 64 workers".
    num_threads = std::min(num_threads, MAX_WORKERS);

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
    // Wake every parked worker so they observe m_stop and exit: a worker in the
    // driver gets driver.unpark(), one on its condvar gets notified, and one that
    // isn't parked yet banks a token so its next park returns at once.
    for (int i = 0; i < static_cast<int>(m_workers.size()); ++i)
        unpark_worker(i);
    for (auto& slot : m_workers)
        slot->thread.join();
}

void WorkStealingExecutor::schedule(std::shared_ptr<detail::TaskBase> task) {
    task->owning_executor = this;
    task->scheduling_state.store(
        detail::SchedulingState::Notified, std::memory_order_relaxed);
    {
        std::lock_guard lock(m_owned_mutex);
        m_owned_tasks.insert(task);
    }
    enqueue(std::move(task));
}

void WorkStealingExecutor::enqueue(std::shared_ptr<detail::TaskBase> task) {
    const int  local_idx = t_wse_worker_index;
    const bool local     = local_idx >= 0 && t_wse_owning_executor == this;

#ifdef CORO_USE_LOCAL_RUN_QUEUE
    if (local) {
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
    if (local) {
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
    // A wake dispatched by this worker's own driver turn: the worker runs the task
    // itself when the turn returns, and after_turn() wakes a peer if needed. Waking
    // one here would at best pick this very worker (its idle bit is still set) and
    // at worst have a peer steal the task it is about to run.
    if (local && t_wse_in_driver_turn) return;
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
        // Race (benign): two enqueuers may both pick the same worker before it
        // clears its idle bit. unpark_worker() is idempotent; the second call only
        // re-banks the token, costing that worker one extra loop iteration.
        const int idx = std::countr_zero(idle);
        unpark_worker(idx);
    }
}

std::size_t WorkStealingExecutor::park_worker(int index) {
    WorkerSlot& slot = *m_workers[index];

    // ParkedDriver is published only while this worker already holds the driver.
    // Publishing it first was a lost wake-up: unpark_worker()'s eventfd write could
    // be consumed by ANOTHER worker's turn (a busy worker's try_turn(0), or the
    // previous holder returning), after which this worker took the driver and blocked
    // in epoll with Notified sitting unread. Only the holder resets the eventfd, so
    // an unpark after before_poll() below always reaches this worker's poll().
    //
    // Race (benign): a stale eventfd write from an earlier unpark can make this turn
    // return at once; the worker finds nothing and parks again.
    std::optional<std::size_t> dispatched;
    {
        InDriverTurn in_turn;
        dispatched = m_driver.try_turn(std::nullopt, [&slot] {
            std::lock_guard lock(slot.park_mutex);
            if (slot.park_state == ParkState::Notified) return false;   // token banked
            slot.park_state = ParkState::ParkedDriver;
            return true;
        });
    }

    std::unique_lock lock(slot.park_mutex);
    if (!dispatched && slot.park_state != ParkState::Notified) {
        // Another worker holds the driver: wait on our own condvar instead.
        slot.park_state = ParkState::ParkedCondvar;
        slot.park_cv.wait(lock, [&] { return slot.park_state == ParkState::Notified; });
    }
    // Consumes any token, including one whose driver.unpark() hasn't been written
    // yet. That late write then makes the next turn (by whichever worker) return
    // at once: a stale unpark, costing one loop iteration.
    slot.park_state = ParkState::Empty;
    return dispatched.value_or(0);
}

void WorkStealingExecutor::unpark_worker(int index) {
    WorkerSlot& slot = *m_workers[index];
    ParkState prev;
    {
        std::lock_guard lock(slot.park_mutex);
        prev = slot.park_state;
        slot.park_state = ParkState::Notified;
    }
    // Outside the lock. Race (benign): the worker may already have woken for
    // another reason; see the stale-unpark note in park_worker().
    if (prev == ParkState::ParkedDriver)
        m_driver.unpark();
    else if (prev == ParkState::ParkedCondvar)
        slot.park_cv.notify_one();
    // Empty or Notified: the token is banked; the next park_worker() returns at once.
}

std::size_t WorkStealingExecutor::local_len(int index) const {
#ifdef CORO_USE_LOCAL_RUN_QUEUE
    return m_worker_queues[index].local.len();
#else
    return m_local_queues[index].size();
#endif
}

void WorkStealingExecutor::after_turn(int index, std::size_t dispatched) {
    // One woken task: run it here, no cross-thread wake. More: let a peer help.
    if (dispatched > 0 && local_len(index) > 1)
        notify_if_needed();
}

void WorkStealingExecutor::worker_loop(int worker_index) {
    // Wait until the constructor has finished pushing all WorkerSlots so that
    // m_workers.size() is stable before we read it. RACE: reading m_workers
    // while the constructor is still push_back()-ing would be undefined.
    m_start_latch.wait();

    t_wse_owning_executor = this;
    t_wse_worker_index    = worker_index;
    set_current_runtime(m_runtime);
    // I/O primitives not yet on the IoDriver still reach the uv loop through this.
    set_current_uv_executor(&m_runtime->uv_executor());

    const int    n            = static_cast<int>(m_workers.size());
    const int    max_search   = std::max(1, n / 2);
    // Task polls since this worker last tried a non-blocking driver turn.
    unsigned     polls_since_turn = 0;

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
                // after we set the idle bit, park_worker() returns immediately.
                // Otherwise we block in the driver, or on our condvar if another
                // worker holds the driver.
                const std::size_t dispatched = park_worker(worker_index);
                m_idle_mask.fetch_and(~(1ull << worker_index),
                                      std::memory_order_relaxed);
                after_turn(worker_index, dispatched);

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
            {
                std::lock_guard lock(m_owned_mutex);
                m_owned_tasks.erase(task);
            }
            // task.reset() here — owned map was the lifetime anchor
        } else {
            // Try Running → Idle.
            expected = detail::SchedulingState::Running;
            if (task->scheduling_state.compare_exchange_strong(
                    expected, detail::SchedulingState::Idle,
                    std::memory_order_acq_rel,
                    std::memory_order_relaxed))
            {
                task.reset(); // release temporary executor ref; task lives via m_owned_tasks
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

        // --- Poll I/O while busy ---
        // A worker that never runs out of work never parks, so without this I/O
        // could starve while every worker is busy. Edge-triggered events wait in the
        // kernel until then. If another worker holds the driver, try_turn() returns
        // at once; that worker is handling events anyway.
        if (++polls_since_turn >= kEventInterval) {
            polls_since_turn = 0;
            std::optional<std::size_t> dispatched;
            {
                InDriverTurn in_turn;
                dispatched = m_driver.try_turn(std::chrono::nanoseconds{0});
            }
            if (dispatched) after_turn(worker_index, *dispatched);
        }
    }

    set_current_runtime(nullptr);
    set_current_uv_executor(nullptr);
    t_wse_owning_executor = nullptr;
    t_wse_worker_index    = -1;
}

} // namespace coro
