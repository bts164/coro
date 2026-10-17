#include <coro/detail/task.h>
#include <coro/runtime/executor.h>
#ifndef CORO_PICO
#include <coro/detail/blocking_waker.h>
#endif

namespace coro::detail {

void TaskBase::wake() {
    // Loop so that a CAS failure retries with the freshly-observed state.
    // compare_exchange_strong updates `expected` on failure, so each iteration
    // starts from the actual current state.
    auto expected = SchedulingState::Idle;
    while (true) {
        switch (expected) {
        case SchedulingState::Idle:
            // Common path: task is suspended. Transition to Notified and enqueue.
            if (scheduling_state.compare_exchange_strong(
                    expected, SchedulingState::Notified,
                    std::memory_order_acq_rel,
                    std::memory_order_relaxed))
            {
                owning_executor->enqueue(shared_from_this());
                return;
            }
            break; // expected updated by CAS; retry with the new state

        case SchedulingState::Running:
            // Task is mid-poll. Mark it RunningAndNotified so the worker
            // re-enqueues it after poll() returns instead of parking.
            if (scheduling_state.compare_exchange_strong(
                    expected, SchedulingState::RunningAndNotified,
                    std::memory_order_acq_rel,
                    std::memory_order_relaxed))
            {
                return;
            }
            break; // expected updated by CAS; retry with the new state

        case SchedulingState::Notified:
        case SchedulingState::RunningAndNotified:
            // Already queued or already flagged for re-enqueue — no-op.
            return;

        case SchedulingState::Done:
            // Task completed before this wake() arrived — no-op.
            return;

        default:
            std::abort(); // unknown state — bug in executor
        }
    }
}

Rc<Waker> TaskBase::clone() {
    // shared_from_this() increments the existing refcount — no allocation.
    return shared_from_this();
}

Weak<Waker> TaskBase::weak_clone() {
    return weak_from_this();
}

// static member definitions
std::function<void(const TaskBase&)> TaskBase::on_resume_hook;
std::function<void(const TaskBase&)> TaskBase::on_suspend_hook;

#ifndef CORO_PICO
void TaskStateBase::wait_until_done() {
    // The parker is ref-counted, not a local: the terminal methods call wake() after
    // releasing `mutex`, through a strong ref taken from the Weak slot. A stack
    // object could be destroyed by this thread returning while that wake() is
    // still inside notify_one().
    auto parker = make_rc<BlockingWaker>();
    {
        // RACE (missed wake): checking `terminated` and storing the waker happen in
        // one critical section, and the terminal methods set `terminated` and take
        // the waker in one too. Either this sees the task finished, or the task
        // finishes later and wakes the parker.
        std::lock_guard lock(mutex);
        if (terminated) return;
        waker = parker;
    }
    // Only a terminal transition fires the slot; loop anyway so a stray wake is harmless.
    while (true) {
        parker->wait_for_wake();
        std::lock_guard lock(mutex);
        if (terminated) return;
    }
}
#endif

} // namespace coro::detail

namespace coro {

void set_task_lifecycle_hooks(
    std::function<void(const Task&)> on_resume,
    std::function<void(const Task&)> on_suspend)
{
    detail::TaskBase::on_resume_hook = std::move(on_resume);
    detail::TaskBase::on_suspend_hook = std::move(on_suspend);
}

} //namespace coro
