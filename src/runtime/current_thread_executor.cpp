#include <coro/runtime/current_thread_executor.h>
#include <coro/detail/context.h>
#include <coro/detail/waker.h>
#include <cassert>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <vector>

#ifdef CORO_PICO
#include <coro/detail/fiber_context.h>
#else
#include <coro/runtime/io_driver.h>
#include <coro/runtime/runtime.h>
#endif

namespace coro {

namespace {
// The executor whose loop is running on this thread, if any. enqueue() uses it to
// tell a wake made by the loop's own thread (a task being polled, a timer fired by
// the loop, an I/O event dispatched inside park()) from any other: only the former
// may push onto the unlocked local queue. Plain static on Pico, like runtime.cpp's
// t_current_runtime: single core, no threads.
#ifdef CORO_PICO
CurrentThreadExecutor* t_running_executor = nullptr;
#else
thread_local CurrentThreadExecutor* t_running_executor = nullptr;
#endif

} // namespace

// Puts the previous running executor back on the way out, so that a nested
// block_on() (on this executor or on another one) is handled.
class CurrentThreadExecutor::LoopScope {
public:
    explicit LoopScope(CurrentThreadExecutor& ex) : m_ex(ex), m_prev(t_running_executor) {
#ifndef CORO_PICO
        std::lock_guard lock(m_ex.m_inject_mutex);
        if (m_ex.m_loop_depth != 0 && m_ex.m_loop_thread != std::this_thread::get_id()) {
            // Two threads in the loop would both use m_local, which has no lock.
            throw std::logic_error(
                "coro: CurrentThreadExecutor is already being run by another thread "
                "(block_on() from a second thread on a single-threaded Runtime)");
        }
        m_ex.m_loop_thread = std::this_thread::get_id();
        ++m_ex.m_loop_depth;
#endif
        t_running_executor = &m_ex;
    }

    ~LoopScope() {
        t_running_executor = m_prev;
#ifndef CORO_PICO
        std::lock_guard lock(m_ex.m_inject_mutex);
        --m_ex.m_loop_depth;
#endif
    }

    LoopScope(const LoopScope&)            = delete;
    LoopScope& operator=(const LoopScope&) = delete;

private:
    CurrentThreadExecutor& m_ex;
    CurrentThreadExecutor* m_prev;
};

CurrentThreadExecutor::CurrentThreadExecutor(std::unique_ptr<Parker> parker,
                                             unsigned event_interval,
                                             unsigned global_queue_interval)
    : m_parker(std::move(parker))
    , m_event_interval(event_interval == 0 ? 1 : event_interval)
    , m_global_queue_interval(global_queue_interval == 0 ? 1 : global_queue_interval)
{
    assert(m_parker && "CurrentThreadExecutor needs a Parker");
}

#ifndef CORO_PICO
CurrentThreadExecutor::CurrentThreadExecutor(Runtime* rt)
    : CurrentThreadExecutor(std::make_unique<IoDriverParker>(rt->io_driver()))
{
    m_turns_io_driver = true;
    m_runtime         = rt;
}
#endif

void CurrentThreadExecutor::schedule(detail::Rc<detail::TaskBase> task) {
    task->owning_executor = this;
    task->scheduling_state.store(
        detail::SchedulingState::Notified, std::memory_order_relaxed);
    bool closed;
    {
        std::lock_guard lock(m_owned_mutex);
        m_owned_tasks.insert(task);
        closed = m_closed;
    }
    if (closed) {
        // Spawned during shutdown: born cancelled. The task is Notified, so
        // cancel_task() only sets the flag; its first poll shuts the future down.
        task->cancel_task();
        notify_shutdown_progress();
    }
    // Through enqueue(), not a bare push: spawn() may run on another thread (e.g. a
    // blocking-pool thread driving a stream) while this executor is parked with no
    // limit, and only enqueue() routes it to the inject queue and unparks. A bare
    // push here was a lost wake-up, and would now also be an unlocked push onto
    // m_local from the wrong thread.
    enqueue(std::move(task));
}

void CurrentThreadExecutor::enqueue(detail::Rc<detail::TaskBase> task) {
    if (t_running_executor == this) {
        // The loop's own thread: no lock, and no unpark, because the loop looks at
        // m_local again before it next blocks (a wake fired from inside park() is
        // found as soon as park() returns).
        //
        // RACE (by contract, not by lock): this is safe only because one thread
        // runs the loop at a time and nothing else reaches this branch. On Pico an
        // interrupt handler would reach it; see the WARNING on enqueue() in the
        // header.
        m_local.push_back(std::move(task));
        return;
    }
    bool unpark;
    {
        std::lock_guard lock(m_inject_mutex);
        m_inject.push_back(std::move(task));
        // m_parked is set under this lock together with park_once()'s empty
        // check, so either that check sees this task (and parks with a zero
        // wait), or we see m_parked and unpark.
        unpark = m_parked;
    }
    // Race (benign): the executor may leave park() for another reason between
    // the unlock above and this call. The unpark then makes its NEXT park()
    // return early — one extra loop iteration, nothing lost.
    if (unpark) m_parker->unpark();
}

bool CurrentThreadExecutor::empty() const {
    std::lock_guard lock(m_inject_mutex);
    // m_local is not protected by the lock; see the header.
    return m_local.empty() && m_inject.empty();
}

bool CurrentThreadExecutor::poll_ready_tasks() {
    LoopScope running(*this);
    return run_batch();
}

bool CurrentThreadExecutor::run_batch() {
    bool polled = false;
    for (unsigned i = 0; i < m_event_interval; ++i) {
        detail::Rc<detail::TaskBase> task = next_task();
        if (!task) break;
        polled = true;
        // A poll that yielded (its budget ran out, or it called yield_now()) ends
        // the batch: the task is asking for timers, I/O and wakes from other
        // threads to be looked at before it runs again.
        if (poll_task(std::move(task))) break;
    }
    return polled;
}

detail::Rc<detail::TaskBase> CurrentThreadExecutor::next_task() {
    // Tasks that keep each other runnable never let m_local run empty, so the
    // inject queue is also looked at every m_global_queue_interval-th poll.
    ++m_tick;
    if (m_local.empty() || m_tick % m_global_queue_interval == 0)
        take_injected();
    if (m_local.empty()) return nullptr;
    detail::Rc<detail::TaskBase> task = std::move(m_local.front());
    m_local.pop_front();
    return task;
}

void CurrentThreadExecutor::take_injected() {
    // All of them under one lock, in order, behind the local tasks already queued.
    std::lock_guard lock(m_inject_mutex);
    while (!m_inject.empty()) {
        m_local.push_back(std::move(m_inject.front()));
        m_inject.pop_front();
    }
}

bool CurrentThreadExecutor::poll_task(detail::Rc<detail::TaskBase> task) {
    auto expected = detail::SchedulingState::Notified;
    if (!task->scheduling_state.compare_exchange_strong(
            expected, detail::SchedulingState::Running,
            std::memory_order_acq_rel,
            std::memory_order_relaxed))
    {
        std::cerr << "[coro] CurrentThreadExecutor: unexpected scheduling_state "
                  << static_cast<int>(expected)
                  << " during Notified→Running transition\n";
        std::abort();
    }

    detail::Context ctx(task, task_budget());
    detail::TaskBase::current = task.get();
    bool done = task->poll(ctx);
    detail::TaskBase::current = nullptr;

    if (done) {
        task->scheduling_state.store(
            detail::SchedulingState::Done, std::memory_order_relaxed);
        bool closed;
        {
            std::lock_guard lock(m_owned_mutex);
            m_owned_tasks.erase(task);
            closed = m_closed;
        }
        if (closed) notify_shutdown_progress();
        return ctx.yielded();
    }

    expected = detail::SchedulingState::Running;
    if (task->scheduling_state.compare_exchange_strong(
            expected, detail::SchedulingState::Idle,
            std::memory_order_acq_rel,
            std::memory_order_relaxed))
    {
        // The executor's owned set keeps the task alive while it is idle. A wake
        // from another thread after this CAS goes through enqueue().
        return ctx.yielded();
    }

    // wake() fired during poll() — RunningAndNotified → Notified, re-enqueue
    if (expected != detail::SchedulingState::RunningAndNotified) {
        std::cerr << "[coro] CurrentThreadExecutor: unexpected scheduling_state "
                  << static_cast<int>(expected)
                  << " after Running→Idle CAS failure\n";
        std::abort();
    }
    if (!task->scheduling_state.compare_exchange_strong(
            expected, detail::SchedulingState::Notified,
            std::memory_order_acq_rel,
            std::memory_order_relaxed))
    {
        std::cerr << "[coro] CurrentThreadExecutor: unexpected scheduling_state "
                  << static_cast<int>(expected)
                  << " during RunningAndNotified→Notified transition\n";
        std::abort();
    }
    // Behind every task already on m_local, whichever thread made the wake: this
    // is the loop's thread doing the push.
    m_local.push_back(std::move(task));
    return ctx.yielded();
}

detail::TimerId CurrentThreadExecutor::add_timer(Instant deadline,
                                                 detail::Weak<detail::Waker> waker) {
    const auto inserted = m_timers.insert(deadline, std::move(waker));
    // True only while park_once() is parked for a later deadline, which a timer
    // added from the executor's own thread can never see.
    // Race (benign): park() may return for another reason before this unpark(); it
    // then makes the next park() return early, one extra loop iteration.
    if (inserted.unpark) m_parker->unpark();
    return inserted.id;
}

void CurrentThreadExecutor::cancel_timer(detail::TimerId id) noexcept {
    m_timers.cancel(id);
}

void CurrentThreadExecutor::park_once() {
    std::optional<std::chrono::nanoseconds> max_wait;
    {
        std::lock_guard lock(m_inject_mutex);
        // m_local is read here only because "is anything ready" is one decision;
        // the lock does not protect it. Nothing can push onto it between this check
        // and park(): this thread is the only one that does, and it wakes no task
        // on the way there.
        if (!m_local.empty() || !m_inject.empty()) {
            max_wait = std::chrono::nanoseconds::zero();
        } else {
            // From here until end_wait(), an add_timer() with an earlier deadline
            // unparks us; see TimerQueue::begin_wait().
            max_wait = m_timers.begin_wait(std::nullopt);
            // From here until park() returns, a remote enqueue() must unpark us.
            m_parked = true;
        }
    }
    // Wakes fired by the parker on this thread (the driver dispatching inside
    // turn()) go onto m_local without an unpark; see enqueue().
    m_parker->park(max_wait);
    m_timers.end_wait();
    {
        std::lock_guard lock(m_inject_mutex);
        m_parked = false;
    }
}

void CurrentThreadExecutor::check_expired_timers() {
    m_timers.fire_expired();
}

#ifdef CORO_PICO
void CurrentThreadExecutor::add_isr_poll(IsrPollEntry*               entry,
                                         detail::Rc<detail::Waker>  waker) {
    m_isr_polls.push_back({entry, std::move(waker)});
}

void CurrentThreadExecutor::remove_isr_poll(IsrPollEntry* entry) {
    // Matched by entry identity, not by any state it reads -- so with multiple
    // waiters sharing the same underlying flag/count, each has a distinct
    // IsrPollEntry* and removing one can never deregister another. See
    // doc/design/isr_safety.md, "Multiple waiters".
    auto it = std::find_if(m_isr_polls.begin(), m_isr_polls.end(),
                           [entry](const IsrPollRegistration& e) { return e.entry == entry; });
    if (it != m_isr_polls.end()) {
        *it = std::move(m_isr_polls.back());
        m_isr_polls.pop_back();
    }
}

void CurrentThreadExecutor::check_isr_events() {
    // Pure peek: is_ready() takes the entry's paired hardware spin lock
    // internally (see doc/design/isr_safety.md, "Cross-core ISR delivery")
    // but never mutates anything. It only decides whether to fire this tick's
    // wake -- the real, consuming claim happens exactly once, inside the real
    // Future::poll(), invoked only via the woken task's own top-down
    // traversal (Coro<T>::poll()'s spurious-wake guard). So this loop never
    // removes entries itself: nothing here resolves a wait, so there's
    // nothing to react to by removing one. Removal stays with the owning
    // waiter's destructor, exactly as before.
    for (auto& reg : m_isr_polls) {
        if (reg.entry->is_ready())
            reg.waker->wake();
    }
}
#endif // CORO_PICO

void CurrentThreadExecutor::begin_shutdown() {
    std::vector<detail::Rc<detail::TaskBase>> tasks;
    {
        std::lock_guard lock(m_owned_mutex);
        m_closed = true;
        tasks.assign(m_owned_tasks.begin(), m_owned_tasks.end());
    }
    // Outside m_owned_mutex: cancel_task() wakes the task, which may take
    // m_inject_mutex.
    //
    // RACE: a schedule() on another thread (a blocking-pool thread) either inserted
    // its task before m_closed was set, and the task is in `tasks`, or after, and
    // schedule() cancels it itself.
    for (auto& task : tasks)
        task->cancel_task();
}

bool CurrentThreadExecutor::has_tasks() const {
    std::lock_guard lock(m_owned_mutex);
    return !m_owned_tasks.empty();
}

void CurrentThreadExecutor::notify_shutdown_progress() noexcept {
#ifndef CORO_PICO
    if (m_runtime != nullptr) m_runtime->shutdown_progress();
#endif
}

void CurrentThreadExecutor::recheck_run_until() noexcept {
    // unpark() is remembered if the loop is not parked yet, so a call that lands
    // between the loop's done() check and its park() is not lost.
    m_parker->unpark();
}

#if defined(CORO_PICO) && defined(__arm__)
namespace {
// Called at the top of the event loop. A function of its own, not part of run_loop():
// run_loop() is a template, and the `static` flag below must exist once, not once per
// instantiation.
void enable_psp_stack_once() {
    // One-time CONTROL/PSP switch, before this thread can ever switch_context()
    // into a fiber -- see doc/design/fiber.md's "Stack model (Pico backend)".
    // Everything before this point (SDK init, main()) ran on MSP and is never a
    // switch_context() target, so it needed no separate stack. From here on,
    // this stack -- whatever wait_for_completion() is already running on --
    // permanently becomes the "caller-side" FiberContext every switch_context()
    // call assumes, and MSP is left as the dedicated, shared ISR stack for the
    // rest of the program's life. Guarded to run only once: wait_for_completion()
    // can be re-entered (nested block_on()), and coro_pico_enable_psp_stack() is
    // only meant to be called the first time -- see its own comment in
    // fiber_context_pico.S. No synchronization needed: CORO_PICO is single-core,
    // single-threaded.
    //
    // Gated on __arm__, not just CORO_PICO: test/CMakeLists.txt's coro_pico_core
    // also builds this file with CORO_PICO defined, but as a host-side x86
    // double for testing core logic without real hardware -- it can never link
    // fiber_context_pico.S (real ARM instructions), so coro_pico_enable_psp_stack()
    // doesn't exist there. Real device firmware (cmake/platforms/pico.cmake)
    // compiles for an actual ARM target, so __arm__ is defined there and not on
    // the host double.
    static bool psp_stack_enabled = false;
    if (!psp_stack_enabled) {
        // Root-caused on real hardware via on-device bisection: the freeze
        // was coro_pico_enable_psp_stack() aliasing MSP and PSP to the same
        // address instead of giving MSP its own dedicated buffer -- see
        // fiber_context_pico.S's big comment at coro_pico_enable_psp_stack_raw()
        // for the full mechanism. Fixed there; safe to call unconditionally now.
        detail::coro_pico_enable_psp_stack();
        psp_stack_enabled = true;
    }
}
} // namespace
#endif

template<typename Done>
void CurrentThreadExecutor::run_loop(Done&& done) {
#if defined(CORO_PICO) && defined(__arm__)
    enable_psp_stack_once();
#endif
    // For the whole loop, not per batch: timers fired by check_expired_timers() and
    // wakers fired inside park() are local wakes too.
    LoopScope running(*this);
    while (true) {
        if (done()) break;
        run_batch();
        check_expired_timers();
        // The batch above may have made done() true. Without this check empty
        // queues would park with no limit, forever.
        if (done()) break;
        // Always reaches the parker, with a zero wait while tasks are queued: a
        // batch is the unit between two looks at outside events.
        park_once();
#ifdef CORO_PICO
        check_isr_events();
#endif
    }
}

void CurrentThreadExecutor::wait_for_completion(detail::TaskStateBase& state) {
    run_loop([&state] {
        std::lock_guard lock(state.mutex);
        return state.terminated;
    });
}

void CurrentThreadExecutor::run_until(const std::function<bool()>& done) {
    run_loop(done);
}

} // namespace coro
