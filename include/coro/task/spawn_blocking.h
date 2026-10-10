#pragma once

// Desktop only: spawn_blocking() and the blocking thread pool behind it.
// See doc/design/spawn_blocking.md.

#include <coro/detail/blocking_cancel.h>
#include <coro/detail/blocking_task.h>
#include <coro/detail/context.h>
#include <coro/detail/poll_result.h>
#include <coro/detail/rc.h>
#include <coro/detail/task_state.h>
#include <coro/detail/waker.h>
#include <coro/runtime/executor.h>
#include <cassert>
#include <chrono>
#include <concepts>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <exception>
#include <functional>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace coro {

// ---------------------------------------------------------------------------
// BlockingHandle<T> — Future<T> returned by spawn_blocking().
//
// Move-only. Polling a moved-from handle throws std::logic_error.
//
// Dropping the handle (or assigning over it) requests cancellation and returns
// at once: nothing waits for the callable, and the task is not registered with
// the enclosing coroutine scope. detach() turns that off. See request_cancel()
// for what a cancellation request does and does not guarantee.
//
// Deliberately NOT Cancellable: the method is request_cancel(), not cancel().
// A Cancellable future must be polled until it has drained, which would hold a
// cancelled coroutine until the blocking thread reached a cancellation point —
// possibly never.
// ---------------------------------------------------------------------------
template<typename T>
class [[nodiscard]] BlockingHandle {
public:
    using OutputType = T;

    BlockingHandle(detail::Rc<detail::TaskState<T>> state, detail::Weak<detail::TaskBase> task)
        : m_state(std::move(state)), m_task(std::move(task)) {}

    BlockingHandle(const BlockingHandle&)            = delete;
    BlockingHandle& operator=(const BlockingHandle&) = delete;

    BlockingHandle(BlockingHandle&& other) noexcept
        : m_state(std::move(other.m_state)),
          m_task(std::move(other.m_task)),
          m_detached(other.m_detached),
          m_finished(other.m_finished) {}

    BlockingHandle& operator=(BlockingHandle&& other) noexcept {
        if (this != &other) {
            close();  // the task this handle referred to is being dropped
            m_state    = std::move(other.m_state);
            m_task     = std::move(other.m_task);
            m_detached = other.m_detached;
            m_finished = other.m_finished;
        }
        return *this;
    }

    // Requests cancellation unless detached. Never waits for the callable.
    ~BlockingHandle() { close(); }

    // Same body as JoinHandle::poll(). A task that ended with neither a result
    // nor an exception (cancelled, or unwound by BlockingCancelled) reports
    // PollDropped.
    PollResult<T> poll(detail::Context& ctx) {
        if (!m_state)
            throw std::logic_error("BlockingHandle::poll called on a moved-from handle");

        std::lock_guard lock(m_state->mutex);
        if (m_state->terminated) {
            // Lets close() skip the cancellation request: nothing is left to cancel.
            m_finished = true;
            if (m_state->exception)
                return PollError(m_state->exception);
            if constexpr (std::is_void_v<T>) {
                if (m_state->result) {
                    m_state->result = false;
                    return PollReady;
                }
            } else {
                if (m_state->result.has_value())
                    return std::move(*m_state->result);
            }
            return PollDropped;
        }
        // Weak, as in JoinHandle: the stored waker must not keep the awaiting task alive.
        m_state->waker = ctx.get_weak_waker();
        return PollPending;
    }

    // Asks the callable to stop. It sets the task's cancelled flag and wakes the
    // thread if it is parked in blocking_wait(); the callable then unwinds with
    // BlockingCancelled at its current or next cancellation point (blocking_wait,
    // blocking_next, blocking_cancellation_point). A callable that never reaches
    // one runs to completion. A task still queued is skipped. Idempotent; does
    // nothing once the task has finished, or on a moved-from handle.
    //
    // RACE: this may run at any moment relative to the callable, including just
    // after it returned. Then the flag is set on a finished task and the wake is
    // a no-op (state Done). If it lands between the callable returning a value
    // and run() storing it, the value is discarded and the handle reports
    // PollDropped — the same outcome as cancelling slightly earlier.
    void request_cancel() noexcept {
        if (auto task = m_task.lock())
            task->cancel_task();
    }

    // request_cancel(), then hands the handle back to be awaited. The await
    // completes once the callable has returned or unwound.
    [[nodiscard]] BlockingHandle cancel_and_join() && {
        request_cancel();
        return std::move(*this);
    }

    // Dropping the handle no longer requests cancellation: the callable runs to
    // completion and its result is discarded. Unlike JoinHandle::detach(), the
    // handle stays usable and can still be awaited.
    BlockingHandle& detach() & {
        m_detached = true;
        return *this;
    }
    BlockingHandle&& detach() && {
        m_detached = true;
        return std::move(*this);
    }

private:
    // Shared by the destructor and move-assignment.
    void close() noexcept {
        if (m_state && !m_detached && !m_finished)
            request_cancel();
        m_state.reset();
        m_task.reset();
    }

    // Typed access to the result, the completion waker slot and `terminated`.
    // Aliases the BlockingTaskImpl allocation, so it also keeps the task alive.
    detail::Rc<detail::TaskState<T>> m_state;
    // The same task, type-erased, for cancel_task().
    detail::Weak<detail::TaskBase>   m_task;
    bool m_detached = false;
    bool m_finished = false;  // poll() has seen the task terminated
};

namespace detail {

// ---------------------------------------------------------------------------
// BlockingTaskImpl<F> — the single allocation behind one spawn_blocking() call:
// the scheduling state and waker (TaskBase), the result, completion waker and
// cancelled flag (TaskState<T>), and the callable itself.
// ---------------------------------------------------------------------------
template<typename F>
class BlockingTaskImpl final : public BlockingTaskBase,
                               public TaskState<std::invoke_result_t<F&>> {
public:
    using OutputType = std::invoke_result_t<F&>;

    explicit BlockingTaskImpl(F callable) : m_callable(std::move(callable)) {
        // Born queued: nothing can see the task before it is submitted.
        this->scheduling_state.store(SchedulingState::Notified, std::memory_order_relaxed);
    }

    bool is_complete() const override {
        std::lock_guard lock(this->mutex);
        return this->terminated;
    }

    void set_waker(Weak<Waker> waker) override {
        std::lock_guard lock(this->mutex);
        this->waker = std::move(waker);
    }

    // Called by BlockingHandle::request_cancel() and by the pool at shutdown,
    // from any thread.
    //
    // RACE: the flag is set before wake(), and the task's thread re-reads it
    // after every return from park(), so a cancel is never lost:
    //  - thread parked (Idle): wake() moves it to Notified and unparks it;
    //  - thread running or polling (Running): wake() leaves RunningAndNotified,
    //    so the next park() returns at once and the flag is seen then, or at
    //    the next cancellation point, whichever comes first;
    //  - still queued (Notified): wake() is a no-op and run() checks the flag
    //    before calling the callable;
    //  - finished (Done): no-op.
    void cancel_task() noexcept override {
        assert(this->owning_executor != nullptr);
        this->cancelled.store(true, std::memory_order_relaxed);
        this->wake();
    }

    bool is_cancelled() const noexcept override {
        return this->cancelled.load(std::memory_order_relaxed);
    }

    TaskStateBase& park_state() noexcept override { return *this; }

    void run() override {
        // Cancelled while still queued: skip the callable.
        if (!is_cancelled()) {
            try {
                if constexpr (std::is_void_v<OutputType>) {
                    (*m_callable)();
                    // A cancelled task has no result, even if the callable finished.
                    if (!is_cancelled()) {
                        std::lock_guard lock(this->mutex);
                        this->result = true;
                    }
                } else {
                    OutputType value = (*m_callable)();
                    if (!is_cancelled()) {
                        std::lock_guard lock(this->mutex);
                        this->result.emplace(std::move(value));
                    }
                }
            } catch (const BlockingCancelled&) {
                // Cancelled and unwound, or a future it waited on was dropped:
                // neither a result nor an exception. The handle reports PollDropped.
            } catch (...) {
                // Reported even if the task was cancelled meanwhile.
                std::lock_guard lock(this->mutex);
                this->exception = std::current_exception();
            }
        }
        // Destroy the callable on this thread, before the awaiter can learn the
        // task is finished: whatever it owns is released by the time the handle
        // completes.
        m_callable.reset();
        // Terminal. Later wakes (stale wakers from futures this task waited on,
        // or a late cancel) see Done and do nothing.
        this->scheduling_state.store(SchedulingState::Done, std::memory_order_release);
        this->mark_done();
    }

private:
    std::optional<F> m_callable;
};

// Non-template helper defined in blocking_pool.cpp. Submits the task to the
// BlockingPool owned by the current runtime. Keeping this out of the template
// body means spawn_blocking.h needs no knowledge of Runtime, eliminating the
// circular-include dependency.
void submit_blocking_task(Rc<BlockingTaskBase> task);

} // namespace detail

// Forward declaration — BlockingPool stores a Runtime* to set the thread-local
// on each worker thread. Cannot include runtime.h here (circular dependency).
class Runtime;

// ---------------------------------------------------------------------------
// BlockingPool — owned by Runtime. Manages a pool of detached threads that
// run blocking tasks. Threads are created lazily and exit after a keep-alive
// timeout with no work, shrinking the pool back down.
// ---------------------------------------------------------------------------
class BlockingPool {
public:
    static constexpr std::size_t kDefaultMaxThreads = 512;
    static constexpr auto        kKeepAlive = std::chrono::seconds(10);

    // rt must be the owning Runtime. It is used by worker threads to set their
    // thread-local current_runtime so recursive spawn_blocking calls work.
    explicit BlockingPool(Runtime* rt, std::size_t max_threads = kDefaultMaxThreads);

    // begin_shutdown(), then stop(): requests cancellation of every live task and
    // waits for all threads to exit. Still waits for a callable that ignores
    // cancellation or holds a BlockingCancelShield.
    ~BlockingPool();

    // Runtime shutdown, in the order the Runtime calls them. See
    // doc/design/runtime_shutdown.md.

    // Requests cancellation of every queued or running task. From now on a
    // submitted task is cancelled before it is queued (its callable is skipped),
    // and the pool calls Runtime::shutdown_progress() each time a task leaves it.
    // Does not wait. Idempotent.
    void begin_shutdown();

    // True while a task is queued or running.
    bool has_tasks() const;

    // Makes the threads exit once the queue is empty and waits until they have.
    // submit() throws std::runtime_error afterwards. Idempotent.
    void stop() noexcept;

    // True if the calling thread is one of this pool's threads, inside a callable.
    bool on_pool_thread() const noexcept;

    BlockingPool(const BlockingPool&)            = delete;
    BlockingPool& operator=(const BlockingPool&) = delete;

    // Queues a task to be run on a blocking pool thread. Called by
    // spawn_blocking(). A task submitted after begin_shutdown() is cancelled
    // before it is queued, so its callable is skipped. Throws std::runtime_error
    // after stop(): no thread is left to run it.
    void submit(detail::Rc<detail::BlockingTaskBase> task);

private:
    // The `owning_executor` of every blocking task. A blocking task is never
    // in a ready queue: "enqueue" means "wake the thread parked on it".
    class ParkingExecutor final : public Executor {
    public:
        void schedule(detail::Rc<detail::TaskBase> task) override;  // never called; aborts
        void enqueue(detail::Rc<detail::TaskBase> task) override;
        void wait_for_completion(detail::TaskStateBase& state) override;
        // The pool does its own shutdown bookkeeping (begin_shutdown() below); the
        // parking executor owns nothing.
        void begin_shutdown() override {}
        bool has_tasks() const override { return false; }
    };

    void worker_loop();
    void link_live(detail::BlockingTaskBase& task) noexcept;    // m_mutex held
    void unlink_live(detail::BlockingTaskBase& task) noexcept;  // m_mutex held

    Runtime*                m_runtime;
    ParkingExecutor         m_parking_executor;
    mutable std::mutex      m_mutex;
    std::condition_variable m_cv;
    // Everything below is protected by m_mutex.
    std::deque<detail::Rc<detail::BlockingTaskBase>> m_queue;
    // Every task that is queued or running, so that shutdown can cancel them.
    // Intrusive (BlockingTaskBase::live_prev/live_next); owns nothing: a linked
    // task is kept alive by m_queue or by the worker thread running it.
    detail::BlockingTaskBase* m_live_head{nullptr};
    std::size_t             m_total_threads{0};
    std::size_t             m_idle_threads{0};
    std::size_t             m_max_threads;
    // Set by begin_shutdown(): new tasks are born cancelled, departures are reported.
    bool                    m_closed{false};
    // Set by stop(): idle threads exit.
    bool                    m_stop{false};
};

// ---------------------------------------------------------------------------
// BlockingCancelShield — while one is alive on a blocking pool thread,
// cancellation points on that thread do not throw. A request that arrives
// meanwhile stays pending and is delivered at the first cancellation point
// after the outermost shield is gone. Nests. No effect on other threads.
//
// For a stretch that must not be abandoned half way, or for cleanup that has
// to wait on a future while a BlockingCancelled is already unwinding.
// ---------------------------------------------------------------------------
class BlockingCancelShield {
public:
    BlockingCancelShield() noexcept : m_task(detail::current_blocking_task()) {
        if (m_task) ++m_task->shield_depth;
    }
    ~BlockingCancelShield() {
        if (m_task) --m_task->shield_depth;
    }

    BlockingCancelShield(const BlockingCancelShield&)            = delete;
    BlockingCancelShield& operator=(const BlockingCancelShield&) = delete;

private:
    // Touched only by the task's own thread: a shield must be destroyed on the
    // thread, and inside the callable, that created it.
    detail::BlockingTaskBase* m_task;
};

// ---------------------------------------------------------------------------
// spawn_blocking() — free function entry point.
//
// Submits the callable to the BlockingPool owned by the current Runtime and
// returns a BlockingHandle<T> the caller can co_await (or, from another
// blocking thread, pass to blocking_wait()).
//
// Requires a running Runtime (same as coro::spawn()).
//
// OWNERSHIP: the callable must own all its data. Do NOT capture references
// or pointers into the spawning coroutine's stack frame — dropping the handle
// only asks the callable to stop and does not wait for it, so the coroutine
// may destroy those locals while the thread is still running.
// ---------------------------------------------------------------------------
template<typename F>
    requires std::invocable<std::decay_t<F>&>
[[nodiscard]] BlockingHandle<std::invoke_result_t<std::decay_t<F>&>>
spawn_blocking(F&& f) {
    using Fn = std::decay_t<F>;
    using T  = std::invoke_result_t<Fn&>;

    auto impl = detail::make_rc<detail::BlockingTaskImpl<Fn>>(std::forward<F>(f));
    // Aliased pointers into the same allocation, as for TaskImpl/JoinHandle.
    detail::Rc<detail::TaskState<T>> state = impl;
    detail::Weak<detail::TaskBase>   task  = detail::Rc<detail::TaskBase>(impl);

    detail::submit_blocking_task(std::move(impl));

    return BlockingHandle<T>(std::move(state), std::move(task));
}

} // namespace coro
