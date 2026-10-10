#include <coro/runtime/current_thread_executor.h>
#include <coro/detail/context.h>
#include <coro/detail/waker.h>
#include <cassert>
#include <cstdlib>
#include <iostream>
#include <vector>

#ifdef CORO_PICO
#include <coro/detail/fiber_context.h>
#else
#include <coro/runtime/io_driver.h>
#include <coro/runtime/runtime.h>
#endif

namespace coro {

namespace {
// The executor whose loop is inside park() on this thread, if any. Lets
// enqueue() tell a wake fired by the parker itself (an I/O event dispatched
// inside IoDriver::turn() on this thread) from a wake by another thread: only
// the latter needs unpark(). Plain static on Pico, like runtime.cpp's
// t_current_runtime: single core, no threads.
#ifdef CORO_PICO
CurrentThreadExecutor* t_parked_executor = nullptr;
#else
thread_local CurrentThreadExecutor* t_parked_executor = nullptr;
#endif
} // namespace

CurrentThreadExecutor::CurrentThreadExecutor(std::unique_ptr<Parker> parker)
    : m_parker(std::move(parker))
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
    // limit, and only enqueue() unparks it. A bare push here was a lost wake-up.
    enqueue(std::move(task));
}

// May be called from outside the executor thread: an ISR on Pico or an
// external thread on multi-threaded platforms. m_ready_mutex provides the
// appropriate serialisation for the current platform (see detail/mutex.h).
void CurrentThreadExecutor::enqueue(detail::Rc<detail::TaskBase> task) {
    bool unpark;
    {
        std::lock_guard lock(m_ready_mutex);
        m_ready.push(std::move(task));
        // m_parked is set under this lock together with park_once()'s empty
        // check, so either that check sees this task (and parks with a zero
        // wait), or we see m_parked and unpark. A wake fired from inside park()
        // on the executor's own thread needs no unpark: the loop runs again as
        // soon as park() returns.
        unpark = m_parked && t_parked_executor != this;
    }
    // Race (benign): the executor may leave park() for another reason between
    // the unlock above and this call. The unpark then makes its NEXT park()
    // return early — one extra loop iteration, nothing lost.
    if (unpark) m_parker->unpark();
}

bool CurrentThreadExecutor::empty() const {
    std::lock_guard lock(m_ready_mutex);
    return m_ready.empty();
}

bool CurrentThreadExecutor::poll_ready_tasks() {
    // Snapshot count under the lock, then pop one task at a time.
    // Holding the lock only for the pop (not across the poll) keeps the
    // ISR-disable window as short as possible.
    std::size_t count;
    {
        std::lock_guard lock(m_ready_mutex);
        count = m_ready.size();
    }
    if (count == 0) return false;

    for (std::size_t i = 0; i < count; ++i) {
        detail::Rc<detail::TaskBase> task;
        {
            std::lock_guard lock(m_ready_mutex);
            if (m_ready.empty()) break;
            task = std::move(m_ready.front());
            m_ready.pop();
        }

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

        detail::Context ctx(task);
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
        } else {
            expected = detail::SchedulingState::Running;
            if (task->scheduling_state.compare_exchange_strong(
                    expected, detail::SchedulingState::Idle,
                    std::memory_order_acq_rel,
                    std::memory_order_relaxed))
            {
                task.reset(); // executor's owned map keeps the task alive while parked
            } else {
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
                {
                    std::lock_guard lock(m_ready_mutex);
                    m_ready.push(std::move(task));
                }
            }
        }
    }
    return true;
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
        std::lock_guard lock(m_ready_mutex);
        if (!m_ready.empty()) {
            max_wait = std::chrono::nanoseconds::zero();
        } else {
            // From here until end_wait(), an add_timer() with an earlier deadline
            // unparks us; see TimerQueue::begin_wait().
            max_wait = m_timers.begin_wait(std::nullopt);
            // From here until park() returns, a remote enqueue() must unpark us.
            m_parked = true;
        }
    }
    t_parked_executor = this;
    m_parker->park(max_wait);
    t_parked_executor = nullptr;
    m_timers.end_wait();
    {
        std::lock_guard lock(m_ready_mutex);
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
    // Outside m_owned_mutex: cancel_task() wakes the task, which takes m_ready_mutex.
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
    while (true) {
        if (done()) break;
        poll_ready_tasks();
        check_expired_timers();
        // The poll above may have made done() true. Without this check an empty
        // queue would park with no limit, forever.
        if (done()) break;
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
