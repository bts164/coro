#include <coro/task/spawn_blocking.h>
#include <coro/runtime/runtime.h>
#include <cstdlib>
#include <stdexcept>
#include <thread>
#include <vector>

namespace coro {

namespace {

// The blocking task whose callable is running on this thread. Set by
// BlockingPool::worker_loop() around each task; null everywhere else. Separate
// from TaskBase::current, which stays null on pool threads: code that asks
// "am I inside an executor task?" must keep getting "no" here.
thread_local detail::BlockingTaskBase* t_current_blocking_task = nullptr;

} // namespace

// ---------------------------------------------------------------------------
// BlockingTaskBase
// ---------------------------------------------------------------------------

namespace detail {

bool BlockingTaskBase::poll(Context&) {
    std::abort();  // a blocking task is run, never polled
}

void BlockingTaskBase::park() {
    // Called only by the task's own thread, in state Running or
    // RunningAndNotified (it has just polled a future that returned Pending).
    auto expected = SchedulingState::Running;
    if (!scheduling_state.compare_exchange_strong(
            expected, SchedulingState::Idle,
            std::memory_order_acq_rel, std::memory_order_relaxed))
    {
        // RunningAndNotified: a wake arrived during the poll (or earlier, from a
        // stale waker or a cancel). Consume it and poll again without parking.
        //
        // RACE: no other thread writes the state while it is RunningAndNotified
        // (wake() is a no-op there), so a plain store cannot lose a wake. A wake
        // that lands right after it sees Running and sets RunningAndNotified
        // again, which the next park() consumes.
        assert(expected == SchedulingState::RunningAndNotified);
        scheduling_state.store(SchedulingState::Running, std::memory_order_release);
        return;
    }

    // RACE (missed wake): wake() moves Idle → Notified WITHOUT the mutex, then
    // calls ParkingExecutor::enqueue() → unpark(), which takes the mutex before
    // notifying. The predicate below is evaluated under that mutex before every
    // wait, so a wake that comes after the CAS above is either seen by the
    // predicate, or its unpark() blocks on the mutex until this thread is inside
    // wait() and then notifies it.
    TaskStateBase& state = park_state();
    std::unique_lock lock(state.mutex);
    state.cv.wait(lock, [this] {
        return scheduling_state.load(std::memory_order_acquire) == SchedulingState::Notified;
    });
    // Notified → Running. wake() is a no-op while Notified, so nothing else can
    // have changed the state since the predicate saw it.
    scheduling_state.store(SchedulingState::Running, std::memory_order_release);
}

void BlockingTaskBase::unpark() {
    TaskStateBase& state = park_state();
    std::lock_guard lock(state.mutex);
    // notify_all: the condition variable is TaskStateBase's, shared with
    // wait_until_done().
    state.cv.notify_all();
}

BlockingTaskBase* current_blocking_task() noexcept {
    return t_current_blocking_task;
}

bool blocking_cancel_pending(const BlockingTaskBase& task) noexcept {
    return task.cancel_pending();
}

Rc<Waker> blocking_task_waker(BlockingTaskBase& task) {
    return task.shared_from_this();
}

void blocking_task_park(BlockingTaskBase& task) {
    task.park();
}

void submit_blocking_task(Rc<BlockingTaskBase> task) {
    current_runtime().blocking_pool().submit(std::move(task));
}

} // namespace detail

void blocking_cancellation_point() {
    detail::BlockingTaskBase* task = t_current_blocking_task;
    if (task != nullptr && task->cancel_pending())
        throw BlockingCancelled{};
}

// ---------------------------------------------------------------------------
// ParkingExecutor
// ---------------------------------------------------------------------------

void BlockingPool::ParkingExecutor::schedule(detail::Rc<detail::TaskBase>) {
    std::abort();  // blocking tasks are submitted to the pool, never scheduled
}

void BlockingPool::ParkingExecutor::enqueue(detail::Rc<detail::TaskBase> task) {
    // Only BlockingPool::submit() installs this executor, and it takes an
    // Rc<BlockingTaskBase>, so the downcast is safe.
    static_cast<detail::BlockingTaskBase&>(*task).unpark();
}

void BlockingPool::ParkingExecutor::wait_for_completion(detail::TaskStateBase& state) {
    state.wait_until_done();
}

// ---------------------------------------------------------------------------
// BlockingPool
// ---------------------------------------------------------------------------

BlockingPool::BlockingPool(Runtime* rt, std::size_t max_threads)
    : m_runtime(rt), m_max_threads(max_threads)
{}

BlockingPool::~BlockingPool() {
    // The Runtime has already done both by the time its pool member is destroyed;
    // a pool that stands alone does them here.
    begin_shutdown();
    stop();
}

void BlockingPool::begin_shutdown() {
    // Strong references, so each task outlives the cancel_task() call below
    // even if its thread finishes it meanwhile.
    std::vector<detail::Rc<detail::TaskBase>> live;
    {
        std::lock_guard lock(m_mutex);
        if (m_closed) return;
        m_closed = true;
        // A linked task is alive: it is unlinked, under this mutex, before the
        // worker running it lets go of it.
        for (auto* task = m_live_head; task != nullptr; task = task->live_next)
            live.push_back(task->shared_from_this());
    }

    // Ask every queued or running callable to stop: a thread parked in
    // blocking_wait() wakes and unwinds, a queued task is skipped. Outside
    // m_mutex, because cancel_task() takes the task's own mutex to unpark it.
    //
    // RACE: a task submitted after m_closed was set is not in `live`; submit()
    // cancels it itself. A callable that ignores cancellation or holds a
    // BlockingCancelShield keeps running, and has_tasks() stays true until it
    // returns.
    for (auto& task : live)
        task->cancel_task();
}

bool BlockingPool::has_tasks() const {
    std::lock_guard lock(m_mutex);
    return m_live_head != nullptr;
}

void BlockingPool::stop() noexcept {
    {
        std::lock_guard lock(m_mutex);
        m_stop = true;
    }
    m_cv.notify_all();

    // Wait for all threads to exit. Threads are detached so we can't join them;
    // instead each thread decrements m_total_threads and notifies before exiting.
    // A thread still inside a callable finishes it, and whatever is queued, first.
    std::unique_lock lock(m_mutex);
    m_cv.wait(lock, [this] { return m_total_threads == 0; });
}

bool BlockingPool::on_pool_thread() const noexcept {
    // owning_executor is written once, in submit(), before the task can run.
    const detail::BlockingTaskBase* task = t_current_blocking_task;
    return task != nullptr && task->owning_executor == &m_parking_executor;
}

void BlockingPool::link_live(detail::BlockingTaskBase& task) noexcept {
    task.live_prev = nullptr;
    task.live_next = m_live_head;
    if (m_live_head != nullptr) m_live_head->live_prev = &task;
    m_live_head = &task;
}

void BlockingPool::unlink_live(detail::BlockingTaskBase& task) noexcept {
    if (task.live_prev != nullptr) task.live_prev->live_next = task.live_next;
    else                           m_live_head = task.live_next;
    if (task.live_next != nullptr) task.live_next->live_prev = task.live_prev;
    task.live_prev = nullptr;
    task.live_next = nullptr;
}

void BlockingPool::submit(detail::Rc<detail::BlockingTaskBase> task) {
    // Set before the task is shared with anything else (its handle has not been
    // returned yet), and never changed afterwards.
    task->owning_executor = &m_parking_executor;

    bool closed;
    {
        std::lock_guard lock(m_mutex);

        if (m_stop)
            throw std::runtime_error("coro::spawn_blocking(): the runtime has been shut down");

        // Shutdown has already cancelled the tasks it could see; this one arrived
        // after that (spawned by a task or a callable that is still draining). The
        // task is Notified, so cancel_task() only sets the flag and run() skips the
        // callable.
        closed = m_closed;
        if (closed) task->cancel_task();

        link_live(*task);
        m_queue.push_back(std::move(task));

        if (m_idle_threads > 0) {
            // An idle thread will pick this up.
            m_cv.notify_one();
        } else if (m_total_threads < m_max_threads) {
            // Spin up a new thread.
            ++m_total_threads;
            std::thread([this] { worker_loop(); }).detach();
        }
        // else: at capacity — work will be picked up when a thread becomes idle.
    }

    // RACE: see Runtime::shutdown(). A task that arrives during shutdown is
    // announced under the runtime's shutdown mutex while its spawner is still a
    // live task, so the thread waiting for "no tasks left" cannot look at the
    // executor and the pool on either side of this hand-over and find both empty.
    // Outside m_mutex: the waiting thread takes the shutdown mutex first, then
    // m_mutex.
    if (closed && m_runtime != nullptr) m_runtime->shutdown_progress();
}

void BlockingPool::worker_loop() {
    // Set the thread-local runtime so recursive spawn_blocking calls and futures
    // that reach the runtime (sleep_for's timer, sockets on the driver) work from
    // within this blocking thread, e.g. under blocking_wait(). Not reset before
    // the thread exits below: the OS thread terminates right after, so nothing can
    // read it stale.
    set_current_runtime(m_runtime);

    while (true) {
        detail::Rc<detail::BlockingTaskBase> task;
        {
            std::unique_lock lock(m_mutex);
            ++m_idle_threads;

            // Wait for work, shutdown, or keep-alive timeout.
            m_cv.wait_for(lock, kKeepAlive, [this] {
                return !m_queue.empty() || m_stop;
            });

            --m_idle_threads;

            if (!m_queue.empty()) {
                // Also after begin_shutdown() and stop(): queued tasks have been
                // cancelled, so run() skips their callables but still completes
                // their handles.
                task = std::move(m_queue.front());
                m_queue.pop_front();
            } else {
                // Timed out with no work, or stop was set — exit.
                --m_total_threads;
                m_cv.notify_all();  // wake destructor if it's waiting on m_total_threads==0
                return;
            }
        }

        // Run outside the lock.
        t_current_blocking_task = task.get();
        // Notified → Running. A plain store is enough: wake() is a no-op while the
        // task is Notified, and this thread is the only one that dequeued it.
        //
        // RACE: a cancel_task() racing with this store either ran its wake()
        // before it (no-op) or after it (Running → RunningAndNotified). Either
        // way the cancelled flag was set first, and run() and every cancellation
        // point read the flag, not the scheduling state.
        task->scheduling_state.store(detail::SchedulingState::Running, std::memory_order_release);
        task->run();
        t_current_blocking_task = nullptr;

        bool closed;
        {
            // After run() has published the result: shutdown may already have
            // begun, and finds a finished task still linked. Cancelling it is a
            // no-op, and the task stays alive through `task` until it is
            // unlinked here.
            std::lock_guard lock(m_mutex);
            unlink_live(*task);
            closed = m_closed;
        }
        // Tell the thread in Runtime::shutdown() that the pool has shrunk.
        // Outside m_mutex (lock order: shutdown mutex, then m_mutex). The runtime
        // and this pool are still alive: stop() waits for this thread to exit.
        if (closed && m_runtime != nullptr) m_runtime->shutdown_progress();
        // `task` is dropped at the end of the iteration, outside the lock, if the
        // handle is already gone.
    }
}

} // namespace coro
