#include <coro/detail/task.h>
#include <coro/runtime/executor.h>

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

std::shared_ptr<Waker> TaskBase::clone() {
    // shared_from_this() increments the existing refcount — no allocation.
    return shared_from_this();
}

} // namespace coro::detail
