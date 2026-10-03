#include <coro/runtime/work_sharing_executor.h>
#include <coro/runtime/runtime.h>
#include <coro/detail/context.h>
#include <cstdlib>
#include <iostream>

namespace coro {

// Thread-locals identifying which WorkSharingExecutor owns this thread and
// which worker slot it occupies. Both must be checked in enqueue() to avoid
// routing a cross-executor wake to the wrong local queue.
thread_local WorkSharingExecutor* t_owning_executor = nullptr;
thread_local int                  t_worker_index    = -1;

WorkSharingExecutor::WorkSharingExecutor(Runtime* runtime, std::size_t num_threads)
    : m_local_queues(num_threads)
    , m_runtime(runtime)
{
    m_workers.reserve(num_threads);
    for (std::size_t i = 0; i < num_threads; ++i)
        m_workers.emplace_back([this, i] { worker_loop(static_cast<int>(i)); });
}

WorkSharingExecutor::~WorkSharingExecutor() {
    bool unpark_driver;
    {
        std::lock_guard lock(m_mutex);
        // RACE CONDITION NOTE: m_stop must be set *inside* m_mutex before notify_all().
        // If set outside the lock, a worker can evaluate the cv predicate as false,
        // then we set m_stop=true and call notify_all(), and the worker enters wait()
        // and sleeps forever (lost wakeup).
        m_stop = true;
        // A worker takes the driver only under m_mutex after checking m_stop, so
        // either it saw m_stop (and won't block), or we see it holding the driver.
        unpark_driver = m_driver_held;
    }
    m_cv.notify_all();
    // The Runtime destroys its executor before its driver, so this is safe.
    if (unpark_driver) m_runtime->io_driver().unpark();
    for (auto& t : m_workers)
        t.join();
}

void WorkSharingExecutor::schedule(std::shared_ptr<detail::TaskBase> task) {
    task->owning_executor = this;
    task->scheduling_state.store(
        detail::SchedulingState::Notified, std::memory_order_relaxed);
    {
        std::lock_guard lock(m_owned_mutex);
        m_owned_tasks.insert(task);
    }
    enqueue(std::move(task));
}

void WorkSharingExecutor::enqueue(std::shared_ptr<detail::TaskBase> task) {
    const int idx = t_worker_index;
    if (idx >= 0 && t_owning_executor == this) {
        // Local path: this is a worker of *this* executor — push to its own queue, no lock.
        m_local_queues[idx].push(std::move(task));
    } else {
        // Remote path: external or main thread — use shared injection queue.
        bool unpark_driver;
        {
            std::lock_guard lock(m_mutex);
            m_injection_queue.push_back(std::move(task));
            // m_driver_held is set under this lock before the holder leaves it to
            // block, so either the holder's re-check sees the task or we unpark it.
            unpark_driver = m_driver_held;
        }
        m_cv.notify_one();
        // Both: the notified condvar waiter (if any) takes the task, and the holder
        // only loses a turn. Unpark before the holder blocks is not lost: the
        // driver's eventfd stays readable until its poll consumes it.
        // Race (benign): the holder may leave turn() for another reason first; the
        // unpark then makes its next turn return at once.
        if (unpark_driver) m_runtime->io_driver().unpark();
    }
}

void WorkSharingExecutor::wait_for_completion(detail::TaskStateBase& state) {
    state.wait_until_done();
}

void WorkSharingExecutor::worker_loop(int worker_index) {
    t_owning_executor = this;
    t_worker_index    = worker_index;
    set_current_runtime(m_runtime);
    // I/O primitives not yet on the IoDriver still reach the uv loop through this.
    set_current_uv_executor(&m_runtime->uv_executor());

    while (true) {
        std::shared_ptr<detail::TaskBase> task;

        // Try the local queue first — no lock needed.
        if (auto local = m_local_queues[worker_index].pop()) {
            task = std::move(*local);
        } else {
            // Local queue empty: wait for the injection queue or shutdown, turning the
            // driver if no other worker is. Returns a task, or null on shutdown.
            task = wait_for_task(worker_index);
            if (task == nullptr && !m_local_queues[worker_index].empty()) {
                // A driver turn woke tasks onto this worker's local queue.
                continue;
            }
        }

        if (!task) {
            // m_stop was set and injection queue was empty. Check local queue
            // one more time (a concurrent enqueue may have just pushed to it).
            if (auto local = m_local_queues[worker_index].pop())
                task = std::move(*local);
            else
                break; // truly nothing left — exit
        }

        // CAS Notified → Running. Failure is a bug (double-dequeue or bad state).
        auto expected = detail::SchedulingState::Notified;
        if (!task->scheduling_state.compare_exchange_strong(
                expected, detail::SchedulingState::Running,
                std::memory_order_acq_rel,
                std::memory_order_relaxed))
        {
            std::cerr << "[coro] WorkSharingExecutor: unexpected scheduling_state "
                      << static_cast<int>(expected)
                      << " during Notified→Running transition (expected Notified=2)\n";
            std::abort();
        }

        detail::Context ctx(std::static_pointer_cast<detail::Waker>(task));
        detail::TaskBase::current = task.get();
        bool done = task->poll(ctx);
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
            // Try Running → Idle: park the task; executor's owned map keeps it alive.
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
                    std::cerr << "[coro] WorkSharingExecutor: unexpected scheduling_state "
                              << static_cast<int>(expected)
                              << " after Running→Idle CAS failure (expected RunningAndNotified=3)\n";
                    std::abort();
                }
                if (!task->scheduling_state.compare_exchange_strong(
                        expected, detail::SchedulingState::Notified,
                        std::memory_order_acq_rel,
                        std::memory_order_relaxed))
                {
                    std::cerr << "[coro] WorkSharingExecutor: unexpected scheduling_state "
                              << static_cast<int>(expected)
                              << " during RunningAndNotified→Notified transition\n";
                    std::abort();
                }
                // Re-enqueue via local path (we are a worker thread).
                enqueue(std::move(task));
            }
        }
    }

    set_current_runtime(nullptr);
    set_current_uv_executor(nullptr);
    t_owning_executor = nullptr;
    t_worker_index    = -1;
}

std::shared_ptr<detail::TaskBase> WorkSharingExecutor::wait_for_task(int worker_index) {
    std::unique_lock lock(m_mutex);
    for (;;) {
        if (!m_injection_queue.empty()) {
            auto task = std::move(m_injection_queue.front());
            m_injection_queue.pop_front();
            return task;
        }
        if (m_stop) return nullptr;
        if (m_driver_held) {
            // Another worker is in the driver; it notifies m_cv when it leaves.
            m_cv.wait(lock);
            continue;
        }

        m_driver_held = true;
        lock.unlock();
        // Wakes from I/O events and timers fire on this thread, so they land in this
        // worker's local queue (enqueue()'s local path).
        m_runtime->io_driver().turn(std::nullopt);
        lock.lock();
        m_driver_held = false;

        if (!m_local_queues[worker_index].empty() || !m_injection_queue.empty() || m_stop) {
            // Leaving the driver to run tasks (or exit): let a condvar waiter take it
            // over. Otherwise loop and take it again ourselves.
            // Race (benign): the notified worker may find the driver already re-taken
            // by another, and go back to waiting.
            m_cv.notify_one();
            if (!m_local_queues[worker_index].empty()) return nullptr;
        }
    }
}

} // namespace coro
