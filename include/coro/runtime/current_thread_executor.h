#pragma once

// Single-threaded executor that drives coroutine tasks on the calling thread and
// waits for outside events through a Parker.
//
// The scheduling loop is platform-agnostic; the platform-specific piece is the
// injected Parker: PollingParker(cyw43_arch_poll) on Pico W (never blocks), or
// IoDriverParker on desktop (blocks in epoll when idle). Timers use coro::Clock.
// See doc/design/executor_design.md, "CurrentThreadExecutor".

#include <coro/runtime/executor.h>
#include <coro/runtime/parker.h>
#include <coro/detail/intrusive_list.h>
#include <coro/detail/mutex.h>
#include <coro/detail/task.h>
#include <coro/detail/task_state.h>
#include <coro/detail/waker.h>
#include <coro/detail/rc.h>
#include <coro/detail/isr_flag.h>
#include <coro/detail/timer_queue.h>
#include <coro/runtime/clock.h>
#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#ifndef CORO_PICO
#include <thread>
#endif
#include <vector>

namespace coro {

//! Forward declaration to avoid circular dependency with Runtime.
class Runtime;

/**
 * @brief Single-threaded executor that drives all coroutine tasks on the calling
 * thread.
 *
 * Internal implementation class — the public entry point is @ref Runtime
 * (`Runtime(1)` on desktop; the only executor on Pico). `wait_for_completion()`
 * loops:
 *
 * @code
 * loop:
 *   run a batch             // poll up to event_interval runnable tasks
 *   check_expired_timers()  // fire sleep_for / timeout wakers
 *   parker.park(max_wait)   // wait for outside events (see below)
 * @endcode
 *
 * A batch ends early when nothing is runnable, or when a task poll yielded (its
 * task budget ran out, or it awaited `yield_now()`), so that timers and outside
 * events are looked at before that task runs again.
 *
 * `max_wait` is zero while tasks are ready, otherwise the time until the next
 * timer in this executor's own queue, otherwise unlimited. On desktop the parker
 * is an @ref IoDriverParker, so an idle executor blocks in epoll and I/O events
 * are dispatched on this thread. A desktop Runtime puts its timers in the
 * driver's queue rather than this one, and `turn()` bounds its own wait by them;
 * this executor's queue serves Pico and a standalone executor. On Pico it is a @ref PollingParker around `cyw43_arch_poll()`, which
 * never blocks, so the loop busy-polls.
 *
 * There are two ready queues. A wake made on the thread that is running the loop
 * goes onto the local queue, which only that thread touches and which has no lock.
 * A wake from anywhere else (the blocking pool, another Runtime's workers) goes onto
 * the inject queue under `m_inject_mutex`, and unparks the executor if it is parked.
 * The loop moves the inject queue onto the local one when the local one is empty,
 * and on every `global_queue_interval`-th task poll otherwise. See
 * doc/design/executor_design.md, "The loop" and "Parking and its races".
 *
 * WARNING: only one thread may run the loop (wait_for_completion(), run_until(),
 * poll_ready_tasks()) at a time. The same thread may nest them. A second thread
 * that tries, for example a blocking callable that calls `block_on()` on the
 * `Runtime(1)` it belongs to, gets `std::logic_error`.
 */
class CurrentThreadExecutor : public Executor {
public:
    /// Task polls in one batch, between two parks. tokio's `event_interval` default.
    static constexpr unsigned kDefaultEventInterval = 61;
    /// Task polls between two looks at the inject queue while the local queue has
    /// work. tokio's current-thread `global_queue_interval` default.
    static constexpr unsigned kDefaultGlobalQueueInterval = 31;

    /// @param parker How the loop waits for outside events; must not be null.
    /// @param event_interval The most task polls in one batch; after a batch the
    ///        timers are checked and the parker is called. 0 is taken as 1.
    /// @param global_queue_interval How many task polls may pass before the inject
    ///        queue is looked at although the local queue has work. 0 is taken as 1.
    explicit CurrentThreadExecutor(std::unique_ptr<Parker> parker,
                                   unsigned event_interval        = kDefaultEventInterval,
                                   unsigned global_queue_interval = kDefaultGlobalQueueInterval);

    // Overload for Runtime(std::in_place_type<CurrentThreadExecutor>, parker).
    // The Runtime passes itself as the first argument; it is kept only to report
    // shutdown progress to.
    CurrentThreadExecutor(Runtime* rt, std::unique_ptr<Parker> parker,
                          unsigned event_interval        = kDefaultEventInterval,
                          unsigned global_queue_interval = kDefaultGlobalQueueInterval)
        : CurrentThreadExecutor(std::move(parker), event_interval, global_queue_interval)
    { m_runtime = rt; }

#ifndef CORO_PICO
    /// Desktop default, used by `Runtime(1)` and
    /// `Runtime(std::in_place_type<CurrentThreadExecutor>)`: an IoDriverParker on
    /// `rt->io_driver()`.
    explicit CurrentThreadExecutor(Runtime* rt);
#endif

    /// Releases every task still owned. Defined in the .cpp.
    ~CurrentThreadExecutor() override;

    CurrentThreadExecutor(const CurrentThreadExecutor&)            = delete;
    CurrentThreadExecutor& operator=(const CurrentThreadExecutor&) = delete;
    CurrentThreadExecutor(CurrentThreadExecutor&&)                 = delete;
    CurrentThreadExecutor& operator=(CurrentThreadExecutor&&)      = delete;

    /// Takes ownership of `task` and enqueues it for its first poll.
    void schedule(detail::Rc<detail::TaskBase> task) override;

    /// Re-enqueues a woken task. On the thread that is running this executor's loop
    /// the task goes onto the local queue, with no lock and no unpark. From any
    /// other thread it goes onto the inject queue under m_inject_mutex, and the
    /// executor is unparked if it is parked.
    ///
    /// WARNING: not callable from an interrupt handler on Pico. An interrupt taken
    /// while the loop runs would pass the "is this the loop's thread" test and push
    /// onto the local queue in the middle of the loop's own use of it. Interrupt
    /// handlers set an IsrEvent/IsrChannel flag instead; see doc/design/isr_safety.md.
    void enqueue(detail::Rc<detail::TaskBase> task) override;

    /// @brief Runs the event loop until `state.terminated`.
    ///
    /// Alternates between running a batch of ready tasks, firing expired
    /// timers, and parking. Whether parking blocks depends on the Parker: on
    /// Pico (PollingParker) it never does, so the loop busy-polls.
    void wait_for_completion(detail::TaskStateBase& state) override;

    /// True only when built by `CurrentThreadExecutor(Runtime*)`, whose parker turns
    /// the runtime's driver. A caller-supplied parker may not.
    bool turns_io_driver() const noexcept override { return m_turns_io_driver; }

    /// Cancels every owned task; later schedule() calls cancel their task too.
    void begin_shutdown() override;

    /// True while a scheduled task has not finished.
    bool has_tasks() const override;

    /// True: nothing runs unless a thread is in wait_for_completion() or run_until().
    bool runs_on_calling_thread() const noexcept override { return true; }

    /// @brief Runs the same loop as wait_for_completion() until `done()` returns
    /// true. Used by `Runtime::shutdown()` to drain the cancelled tasks.
    void run_until(const std::function<bool()>& done) override;

    /// Unparks the loop so that it evaluates its exit condition again.
    void recheck_run_until() noexcept override;

    /// @brief Runs one batch: polls ready tasks in FIFO order until none is left,
    /// `event_interval` polls were made, or a poll yielded. A task that wakes itself
    /// may be polled more than once in one call.
    /// @return `true` if at least one task was polled.
    bool poll_ready_tasks();

    /// @brief Returns `true` if no task is waiting in either ready queue. For the
    /// thread that runs the loop: it reads the local queue, which has no lock.
    bool empty() const;

    /// @brief Adds a timer to this executor's own queue: `waker` fires once
    /// `deadline` has passed. Thread-safe; unparks the executor if it is parked
    /// for a later deadline. Used by `Runtime::add_timer()` on Pico.
    /// @return The id to pass to cancel_timer().
    detail::TimerId add_timer(Instant deadline, detail::Weak<detail::Waker> waker);

    /// @brief Cancels a timer added by add_timer(). Thread-safe. Does nothing if it
    /// has already fired or been cancelled.
    void cancel_timer(detail::TimerId id) noexcept;

    /// @brief Fires wakers for any timers whose deadline has passed.
    /// Called from wait_for_completion() on every loop iteration.
    void check_expired_timers();

#ifdef CORO_PICO
    /// @brief Registers an ISR-safe waiter to be peeked once per event loop iteration.
    ///
    /// `entry` must outlive the registration (removed via remove_isr_poll() from the
    /// waiter's own destructor/move, same as today). Keyed by `entry`'s own identity,
    /// not by any state it reads — so multiple waiters sharing the same underlying
    /// flag/count each get their own, independently removable registration. See
    /// doc/design/isr_safety.md, "Multiple waiters". Safe to call from the executor
    /// thread only (not from an ISR — registration happens inside the coroutine, not
    /// the ISR).
    void add_isr_poll(IsrPollEntry* entry, detail::Rc<detail::Waker> waker);
    void remove_isr_poll(IsrPollEntry* entry);

    /// @brief Scans registered ISR waiters; fires wakers for any whose is_ready()
    /// peek returns true. Never removes entries itself -- nothing here resolves a
    /// wait, so there's nothing to react to by removing one; removal stays with the
    /// owning waiter's destructor. Called from wait_for_completion() on every loop
    /// iteration, after parking.
    void check_isr_events();
#endif // CORO_PICO

private:
    /// Held for as long as a thread runs the loop: claims the loop for that thread
    /// (see m_loop_thread) and marks the executor as the one running on it, which
    /// is what routes enqueue() to the local queue. Defined in the .cpp.
    class LoopScope;

    /// Picks the wait (zero / until next timer / unlimited), marks the executor
    /// parked, and parks once.
    void park_once();

    /// Polls up to m_event_interval tasks; stops early when none is ready or a poll
    /// yielded. Returns true if any task was polled. Loop thread only.
    bool run_batch();

    /// The next task to poll, or null. Moves the inject queue onto the local one
    /// first if the local one is empty or the inject queue is due a look.
    detail::Rc<detail::TaskBase> next_task();

    /// Moves every task in m_inject onto the back of m_local, in order.
    void take_injected();

    /// Polls `task` once and files it by the outcome (done, idle, or woken during
    /// the poll and so back onto m_local). Returns true if the poll yielded.
    bool poll_task(detail::Rc<detail::TaskBase> task);

    /// The event loop behind wait_for_completion() and run_until(). Defined in the
    /// .cpp, its only user.
    template<typename Done>
    void run_loop(Done&& done);

    /// Tells the owning Runtime, if any, that a task was spawned or finished while
    /// the executor is shutting down.
    void notify_shutdown_progress() noexcept;

    /// The owning Runtime. Null for a standalone executor and on Pico, where the
    /// executor is the only place tasks live and nobody needs telling.
    Runtime* m_runtime = nullptr;

    std::unique_ptr<Parker> m_parker;
    bool                    m_turns_io_driver = false;  // set once in the constructor

    // Set once in the constructor, never zero.
    unsigned m_event_interval;
    unsigned m_global_queue_interval;
    // Task polls so far; decides when the inject queue is due a look. Loop thread only.
    unsigned m_tick = 0;

    // Internally synchronized. Lock order: m_inject_mutex, then the queue's mutex
    // (park_once() calls begin_wait() under m_inject_mutex).
    //
    // Declared before the ready queues and m_owned_tasks so that it is destroyed
    // after them: destroying an unfinished task destroys its futures, and a
    // SleepFuture cancels its timer in this queue from its destructor.
    detail::TimerQueue m_timers;

    // Tasks made runnable on the thread that is running the loop. NOT GUARDED: only
    // that thread touches it, and only enqueue()'s `t_running_executor == this`
    // test lets a push through. See the WARNING on the class.
    std::deque<detail::Rc<detail::TaskBase>> m_local;

    // Tasks made runnable from anywhere else. GUARDED BY m_inject_mutex. On Pico
    // the mutex does nothing (see detail/mutex.h): there is one thread, and
    // interrupt handlers do not enqueue.
    mutable detail::Mutex m_inject_mutex;
    std::deque<detail::Rc<detail::TaskBase>> m_inject;
    // True from park_once()'s empty-queue check until park() returns. Read by a
    // remote enqueue() to decide whether to unpark. GUARDED BY m_inject_mutex.
    bool m_parked = false;

#ifndef CORO_PICO
    // The thread running the loop and how deeply it has nested it (a block_on()
    // inside a task). Only there to refuse a second thread, which would otherwise
    // share m_local unlocked. GUARDED BY m_inject_mutex. Pico has one thread.
    std::thread::id m_loop_thread;
    unsigned        m_loop_depth = 0;
#endif

    // Category 1 (doc/task_ownership.md): persistent lifetime anchor for every live task.
    // Linked in schedule(), unlinked after poll() returns true (task reached terminal
    // state). Intrusive through TaskBase::owned_prev/owned_next; the strong reference
    // is TaskBase::owned_self, set while linked. Neither linking nor unlinking
    // allocates. GUARDED BY m_owned_mutex.
    mutable detail::Mutex m_owned_mutex;
    detail::IntrusiveList<detail::TaskBase*, &detail::TaskBase::owned_prev,
                          &detail::TaskBase::owned_next> m_owned_tasks;
    // Set by begin_shutdown(), never cleared. GUARDED BY m_owned_mutex.
    bool                  m_closed = false;

#ifdef CORO_PICO
    struct IsrPollRegistration {
        IsrPollEntry*               entry;   // non-owning; entry outlives the registration
        detail::Rc<detail::Waker>   waker;
    };
    // Written from coroutine context (executor thread) only; read from
    // check_isr_events() on the same thread. No synchronisation needed.
    std::vector<IsrPollRegistration> m_isr_polls;
#endif // CORO_PICO
};

} // namespace coro
