#include <coro/runtime/work_stealing_executor.h>
#include <coro/runtime/runtime.h>
#include <coro/detail/context.h>
#include <coro/detail/trace.h>
#include <algorithm>
#include <bit>
#include <cassert>
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

// The WakeBatch open on this thread, if any. Read only by enqueue() on this thread;
// no other thread ever sees it. See doc/design/work_stealing_executor.md,
// "Batched wakes (`WakeBatch`)".
thread_local detail::WakeBatch* t_wse_wake_batch = nullptr;

} // namespace

// --- WakeBatch ---------------------------------------------------------------------

namespace detail {

WakeBatch::WakeBatch() noexcept : m_prev(t_wse_wake_batch) {
    t_wse_wake_batch = this;
}

WakeBatch::~WakeBatch() {
    // Restored first: nothing the flush does may land in this batch again.
    // WARNING: correct only if this batch was opened on this thread and has not
    // been held across a suspension (the task may resume on another worker).
    assert(t_wse_wake_batch == this && "WakeBatch closed out of order or on another thread");
    t_wse_wake_batch = m_prev;
    if (m_count == 0) return;
    // The tasks are Notified and nothing else will enqueue them again: the flush
    // must happen, also during unwinding. flush_wake_batch() allocates nothing; its
    // lock and unparks are the same operations enqueue() makes.
    if (m_count <= kInlineCapacity)
        m_executor->flush_wake_batch(m_inline, m_count);
    else
        m_executor->flush_wake_batch(m_overflow.data(), m_overflow.size());
}

bool WakeBatch::is_open() noexcept {
    return t_wse_wake_batch != nullptr;
}

bool WakeBatch::try_add(WorkStealingExecutor* executor, TaskBase* task) {
    if (m_executor != nullptr && m_executor != executor) return false;
    if (m_count < kInlineCapacity) {
        m_inline[m_count++] = task;
    } else {
        // Past the inline buffer the tasks live in m_overflow, all of them. If the
        // allocation fails, the task goes through the normal path instead.
        try {
            if (m_count == kInlineCapacity)
                m_overflow.assign(m_inline, m_inline + kInlineCapacity);
            m_overflow.push_back(task);
        } catch (...) {
            return false;
        }
        ++m_count;
    }
    m_executor = executor;
    return true;
}

} // namespace detail

#ifdef CORO_USE_LOCAL_RUN_QUEUE
using TaskSP = detail::TaskBase*;

// Overflow handler for Local<TaskSP>::push_or_overflow().
// Moves spilled tasks directly into the injection queue — no boxing needed.
struct InjectionOverflow {
    std::mutex&               mutex;
    std::deque<TaskSP>&       queue;
    std::atomic<std::size_t>& len; ///< m_injection_len; written under `mutex`.

    void push(TaskSP task) {
        std::lock_guard lock(mutex);
        queue.push_back(std::move(task));
        len.store(queue.size(), std::memory_order_release);
    }

    void push_batch(TaskSP* tasks, std::size_t n) {
        std::lock_guard lock(mutex);
        for (std::size_t i = 0; i < n; ++i)
            queue.push_back(std::move(tasks[i]));
        len.store(queue.size(), std::memory_order_release);
    }
};
#endif

WorkStealingExecutor::WorkStealingExecutor(Runtime* runtime, std::size_t num_threads) :
#ifndef CORO_USE_LOCAL_RUN_QUEUE
    m_local_queues(std::min(num_threads, MAX_WORKERS)),
#endif
    m_runtime(runtime),
    m_driver(runtime->io_driver()),
    m_owned_tasks(std::min(num_threads, MAX_WORKERS))
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
    if (m_owned_tasks.insert(task)) {
        // Spawned during shutdown: born cancelled. The task is Notified, so
        // cancel_task() only sets the flag; its first poll shuts the future down.
        task->cancel_task();
        // RACE: see Runtime::shutdown(). Announced while the spawner is still a
        // live task, which is what keeps the "no tasks left" check from missing a
        // task handed between the executor and the blocking pool.
        m_runtime->shutdown_progress();
    }
    enqueue(std::move(task));
}

void WorkStealingExecutor::begin_shutdown() {
    // Outside the shard locks: cancel_task() wakes the task, which may take m_mutex.
    for (auto& task : m_owned_tasks.close_and_collect())
        task->cancel_task();
}

bool WorkStealingExecutor::has_tasks() const {
    return !m_owned_tasks.empty();
}

void WorkStealingExecutor::enqueue(std::shared_ptr<detail::TaskBase> task) {
    // A WakeBatch open on this thread collects the task; closing it pushes and
    // notifies for all of them at once. Never the LIFO slot.
    if (detail::WakeBatch* batch = t_wse_wake_batch;
            batch != nullptr && batch->try_add(this, task.get()))
        return;

    const int  local_idx = t_wse_worker_index;
    const bool local     = local_idx >= 0 && t_wse_owning_executor == this;

#ifdef CORO_USE_LOCAL_RUN_QUEUE
    if (local) {
        auto& queue = m_worker_queues[local_idx].local;
        InjectionOverflow overflow{m_mutex, m_injection_queue, m_injection_len};
        if (t_wse_in_driver_turn) {
            // Woken by this worker's own driver turn: run in order after the turn,
            // with no notify (see below). Not the LIFO slot.
            queue.push_or_overflow(task.get(), overflow);
            return;
        }
        // Woken by the task this worker is polling (or by a task's destruction just
        // after its poll): the LIFO slot, to run next on this worker while what the
        // waker handed it is still in cache. A task placed in an empty slot wakes
        // nobody: a peer could not take it anyway. One it displaces goes to the ring
        // and is notified for as any local push. The polled task's own re-queue
        // never comes here (requeue_after_poll()). See
        // doc/design/work_stealing_executor.md, "LIFO slot".
        detail::TaskBase* displaced = queue.push_lifo(task.get());
        if (displaced == nullptr) return;
        queue.push_or_overflow(displaced, overflow);
    } else {
        // Local<T> is single-owner so we cannot push directly to another
        // worker's ring from here. All remote pushes go to the injection queue.
        std::lock_guard lock(m_mutex);
        m_injection_queue.push_back(task.get());
        m_injection_len.store(m_injection_queue.size(), std::memory_order_release);
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
            m_injection_len.store(m_injection_queue.size(), std::memory_order_release);
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

void WorkStealingExecutor::requeue_after_poll(int index,
                                              std::shared_ptr<detail::TaskBase> task) {
#ifdef CORO_USE_LOCAL_RUN_QUEUE
    // Straight to the back of the ring: through the LIFO slot a yield would poll the
    // same task again at once (doc/design/task_budget.md, "What a yield does").
    InjectionOverflow overflow{m_mutex, m_injection_queue, m_injection_len};
    m_worker_queues[index].local.push_or_overflow(task.get(), overflow);
    notify_if_needed();
#else
    (void)index;
    enqueue(std::move(task));
#endif
}

void WorkStealingExecutor::flush_wake_batch(detail::TaskBase* const* tasks, std::size_t n) {
    {
        std::lock_guard lock(m_mutex);
        // To the back: a backlog already queued runs first.
        m_injection_queue.insert(m_injection_queue.end(), tasks, tasks + n);
        m_injection_len.store(m_injection_queue.size(), std::memory_order_release);
    }
    // Wake as many parked workers as there are tasks, ignoring m_searching: one
    // searcher would take one task and the rest would ramp up one wake at a time.
    //
    // Race (benign): a worker whose bit is read here may have just woken for another
    // reason; the unpark banks a token and costs it one loop iteration. A worker that
    // sets its bit after this read re-checks the injection queue under m_mutex in
    // Step 4 before parking, and that lock orders it after the push above.
    // Lowest-numbered first, as in notify_if_needed().
    uint64_t idle = m_idle_mask.load(std::memory_order_acquire);
    for (std::size_t woken = 0; idle != 0 && woken < n; ++woken) {
        const int idx = std::countr_zero(idle);
        idle &= idle - 1;
        unpark_worker(idx);
    }
}

void WorkStealingExecutor::wait_for_completion(detail::TaskStateBase& state) {
    state.wait_until_done();
}

void WorkStealingExecutor::notify_if_needed() {
    // If at least one worker is already searching it will find the new task: on its
    // sweep, or, if the sweep has already passed the queue the task went to, in the
    // check the last searcher makes before it parks (Step 4 of worker_loop()).
    //
    // Race: that argument needs this read to be ordered against the searcher's
    // decrement. The caller has just pushed to a lock-free queue and the searcher
    // reads that queue after decrementing: a store then a load on each side, which
    // acquire/release does not order. The read is therefore a seq_cst
    // read-modify-write, paired with the seq_cst fetch_sub in worker_loop(), as in
    // tokio's Idle::notify_should_wakeup().
    if (m_searching.fetch_add(0, std::memory_order_seq_cst) > 0) return;
    // Otherwise wake one parked worker to begin searching: the lowest-numbered, so
    // work stays on a few warm workers and the rest stay parked and their cores idle.
    // Deliberately not random or round-robin; see the design doc, "Which worker is
    // woken".
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
        detail::trace::SleepScope sleeping;
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

bool WorkStealingExecutor::peer_has_tasks(int index) const {
    const int n = static_cast<int>(m_workers.size());
    for (int i = 0; i < n; ++i) {
        if (i == index) continue;
#ifdef CORO_USE_LOCAL_RUN_QUEUE
        if (!m_worker_queues[i].steal.is_empty()) return true;
#else
        if (m_local_queues[i].size() != 0) return true;
#endif
    }
    return false;
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
    detail::trace::thread_name("coro-worker", worker_index);
    detail::trace::AwakeZone awake;

    const int    n            = static_cast<int>(m_workers.size());
    const int    max_search   = std::max(1, n / 2);
    // Task polls since this worker last tried a non-blocking driver turn.
    unsigned     polls_since_turn = 0;
    // Passes of the loop below; decides when the injection queue goes first.
    unsigned     tick = 0;
#ifdef CORO_USE_LOCAL_RUN_QUEUE
    // LIFO-slot polls in a row; reset by any other poll.
    unsigned      lifo_polls = 0;
#endif
    // Budget left by the last poll, passed on to a task run from the LIFO slot.
    std::uint32_t carried_budget = 0;

    while (true) {
        std::shared_ptr<detail::TaskBase> task;
        // Set when this pass ends a steal sweep that leaves no worker searching.
        bool last_searcher = false;
        // Set when `task` came from the LIFO slot.
        bool from_lifo = false;

#ifdef CORO_USE_LOCAL_RUN_QUEUE
        // --- LIFO slot ---
        // Only this worker fills it, from the poll that just ended (or from a task
        // destroyed at the end of that pass), so it goes first. At most
        // kMaxLifoPolls in a row: two tasks that keep waking each other would
        // otherwise keep the queues below from ever being reached. A slot task
        // with no budget left to carry over would only yield at its first await.
        // Either way it goes to the back of the ring, where a peer can steal it.
        {
            auto& local = m_worker_queues[worker_index].local;
            if (local.has_lifo()) {
                detail::TaskBase* t = local.pop_lifo();
                if (lifo_polls < kMaxLifoPolls && carried_budget != 0) {
                    task      = t->shared_from_this();
                    from_lifo = true;
                    ++lifo_polls;
                } else {
                    InjectionOverflow overflow{m_mutex, m_injection_queue, m_injection_len};
                    local.push_or_overflow(t, overflow);
                    notify_if_needed();
                }
            }
            if (!from_lifo) lifo_polls = 0;
        }
#endif

        // --- Periodic injection-queue check ---
        // Step 2 is reached only with an empty local queue. Tasks that keep each
        // other runnable, or keep spending their budget, never leave it empty, so on
        // every kGlobalQueueInterval-th pass one task is taken from the injection
        // queue first.
        //
        // Race (benign): the read of m_injection_len is lock-free and a zero may be
        // stale. Nothing is lost: the task is found at the next interval, or by
        // Step 2 or Step 4 of whichever worker runs dry first. This check plays no
        // part in parking.
        if (!task && ++tick % kGlobalQueueInterval == 0 &&
                m_injection_len.load(std::memory_order_acquire) != 0) {
            std::lock_guard lock(m_mutex);
            if (!m_injection_queue.empty()) {
                task = m_injection_queue.front()->shared_from_this();
                m_injection_queue.pop_front();
                m_injection_len.store(m_injection_queue.size(), std::memory_order_release);
            }
        }

        // --- Step 1: own local queue ---
#ifdef CORO_USE_LOCAL_RUN_QUEUE
        if (!task) {
            if (auto t = m_worker_queues[worker_index].local.pop())
                task = t->shared_from_this();
        }
#else
        if (!task) {
            if (auto t = m_local_queues[worker_index].pop())
                task = (*t)->shared_from_this();
        }
#endif

        // --- Step 2: injection queue ---
        // m_injection_len is read without the lock so that a worker whose local
        // queue is empty does not serialize on m_mutex just to find nothing here.
        // Potential race: a push can land right after a zero read. That is not a
        // lost wakeup: this worker goes on to Step 4, which re-checks the queue
        // under m_mutex after setting its idle bit, before it parks.
        if (!task && m_injection_len.load(std::memory_order_acquire) != 0) {
#ifdef CORO_USE_LOCAL_RUN_QUEUE
            // Take a batch, not one task: the first is run now and the rest go onto
            // this worker's local queue, so it does not come back to m_mutex for
            // each of them. Sizing follows tokio's Core::next_task():
            //  - a 1/n share of the queue (plus one), so peers get some too;
            //  - at most half the local ring, so the batch lands in the half that
            //    push_or_overflow() never spills back here;
            //  - at most the free slots. Only this thread adds to its local queue
            //    (stealers only remove), so the slots counted here are still free
            //    when push_back() runs below, after the lock is dropped.
            auto& local = m_worker_queues[worker_index].local;
            detail::TaskBase* batch[detail::LOCAL_QUEUE_CAPACITY / 2];
            std::size_t       taken = 0;
            {
                std::lock_guard lock(m_mutex);
                const std::size_t len = m_injection_queue.size();
                if (len != 0) {
                    const std::size_t cap =
                        std::min(local.remaining_slots(), local.max_capacity() / 2);
                    const std::size_t want = std::max<std::size_t>(
                        1, std::min(len / static_cast<std::size_t>(n) + 1, cap));
                    for (; taken < want; ++taken) {
                        batch[taken] = m_injection_queue.front();
                        m_injection_queue.pop_front();
                    }
                    m_injection_len.store(m_injection_queue.size(),
                                          std::memory_order_release);
                }
            }
            if (taken != 0) {
                task = batch[0]->shared_from_this();
                for (std::size_t i = 1; i < taken; ++i)
                    local.push_back(batch[i]);
            }
#else
            std::lock_guard lock(m_mutex);
            if (!m_injection_queue.empty()) {
                task = std::move(m_injection_queue.front()->shared_from_this());
                m_injection_queue.pop_front();
                m_injection_len.store(m_injection_queue.size(), std::memory_order_release);
            }
#endif
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

                // Re-check injection queue after sweep. Lock-free emptiness test
                // as in Step 2; Step 4 covers a push that races with it.
                if (!task && m_injection_len.load(std::memory_order_acquire) != 0) {
                    std::lock_guard lock(m_mutex);
                    if (!m_injection_queue.empty()) {
                        task = m_injection_queue.front()->shared_from_this();
                        m_injection_queue.pop_front();
                        m_injection_len.store(m_injection_queue.size(), std::memory_order_release);
                    }
                }

                // seq_cst: pairs with the read in notify_if_needed().
                last_searcher =
                    m_searching.fetch_sub(1, std::memory_order_seq_cst) == 1;

                // Found work by searching: pass the wake on before running it (tokio's
                // transition_worker_from_searching). enqueue() wakes one worker per
                // push at most, and a burst of pushes keeps picking the same worker
                // until it has woken and cleared its idle bit, so a burst of N tasks
                // wakes one worker, not N. That worker steals half of them and, without
                // this, would be the only help the burst ever got until some later
                // enqueue. Each worker woken here does the same in turn, so workers
                // keep waking until one finds nothing or none is idle.
                //
                // Race (benign): the task just taken may have been the last one. The
                // woken worker then sweeps, finds nothing and parks again.
                if (task) notify_if_needed();
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
                    m_injection_len.store(m_injection_queue.size(), std::memory_order_release);
                }
            }

            // The last searcher looks at its peers' queues once more (tokio's
            // notify_if_work_pending()). A worker that pushed to its own queue while
            // this one was still counted as searching woke nobody, on the grounds
            // that the searcher would find the task; but the sweep may already have
            // passed that queue. With nobody searching any more, this is the only
            // worker that can still make good on that. It goes round again and
            // steals.
            //
            // Race (benign): the peer may run the task itself first, or another
            // worker may be part-way through stealing it. The next pass then finds
            // nothing and comes back here, and goes round once more only if a peer's
            // queue is still non-empty, so the repeats end when that queue does.
            //
            // Not covered, as in tokio: a worker that parks without having searched
            // (the searching limit was reached) does not look. A push that saw no
            // searcher and no idle worker just before this one set its bit then
            // waits for its own worker, or for the next wake of any kind. Nothing is
            // lost, since the pushing worker runs its own queue; the task only
            // misses a chance to run in parallel.
            if (!task && last_searcher && peer_has_tasks(worker_index)) {
                m_idle_mask.fetch_and(~(1ull << worker_index),
                                      std::memory_order_relaxed);
                continue;
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

        // A slot task gets what the poll that woke it left of its budget, as in
        // tokio: a ping-pong pair shares one budget instead of a fresh one per hop.
        detail::Context ctx(std::static_pointer_cast<detail::Waker>(task),
                            from_lifo ? carried_budget : task_budget());
        detail::TaskBase::current = task.get();
        bool done;
        {
            detail::trace::PollZone zone(*task);
            done = task->poll(ctx);
            zone.finished(done);
        }
        detail::TaskBase::current = nullptr;
        carried_budget = ctx.remaining_budget();
        // WARNING: a WakeBatch held across a suspension. See WakeBatch.
        assert(!detail::WakeBatch::is_open() && "WakeBatch open after a task poll");

        if (done) {
            task->scheduling_state.store(
                detail::SchedulingState::Done, std::memory_order_relaxed);
            // Only the worker that polled a task to completion removes it, so no two
            // threads remove the same task. remove() hands back the list's reference
            // so that it is dropped here, outside the shard lock; `task` still holds
            // one, so nothing is destroyed on this line.
            auto removed = m_owned_tasks.remove(*task);
            removed.task.reset();
            // Shutting down: tell the thread in Runtime::shutdown() the list shrank.
            // The runtime outlives this call: it joins this worker before it goes.
            if (removed.closed) m_runtime->shutdown_progress();
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
                // Woken during its own poll: to the back of the queue, never the
                // LIFO slot.
                requeue_after_poll(worker_index, std::move(task));
            }
        }

        // --- Poll I/O while busy ---
        // A worker that never runs out of work never parks, so without this I/O
        // could starve while every worker is busy. Edge-triggered events wait in the
        // kernel until then. If another worker holds the driver, try_turn() returns
        // at once; that worker is handling events anyway.
        //
        // A poll that yielded (its budget ran out, or it called yield_now()) brings
        // the turn forward: the task is asking for I/O, timers and signals to be
        // looked at before it runs again.
        if (++polls_since_turn >= kEventInterval || ctx.yielded()) {
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
    t_wse_owning_executor = nullptr;
    t_wse_worker_index    = -1;
}

} // namespace coro
