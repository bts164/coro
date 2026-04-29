#pragma once

#include <memory>

namespace coro::detail {

/**
 * @brief Abstract notification handle used to reschedule a suspended task.
 *
 * A `Waker` is obtained from a `Context` and stored by leaf futures (e.g. I/O callbacks,
 * timers) that need to notify the executor when they become ready. Calling `wake()` moves
 * the associated task from the Suspended state back into the executor's ready queue.
 *
 * **Ownership:** `Waker` is reference-counted via `shared_ptr`. A leaf future typically
 * stores a clone (`clone()`) so the executor retains the original.
 *
 * **Thread safety:** `wake()` must be safe to call from any thread, including libuv
 * callback threads.
 *
 * Implementors: `TaskBase` is the concrete `Waker` — tasks are their own wakers.
 */
class Waker {
public:
    virtual ~Waker();

    /// @brief Reschedules the associated task. Safe to call from any thread.
    virtual void wake() = 0;

    /// @brief Returns a new `Waker` that wakes the same task.
    /// Used when a future needs to hand off the waker to multiple waiters.
    virtual std::shared_ptr<Waker> clone() = 0;
};

} // namespace coro::detail
