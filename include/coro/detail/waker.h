#pragma once

#include <memory>
#include <coro/detail/rc.h>

namespace coro::detail {

/**
 * @brief Abstract notification handle used to reschedule a suspended task.
 *
 * A `Waker` is obtained from a `Context` and stored by leaf futures (e.g. I/O callbacks,
 * timers) that need to notify the executor when they become ready. Calling `wake()` moves
 * the associated task from the Suspended state back into the executor's ready queue.
 *
 * **Ownership:** `Waker` is reference-counted via `Rc` (`shared_ptr` off Pico). A leaf future
 * typically stores a clone (`clone()`) so the executor retains the original.
 *
 * **Thread safety:** `wake()` must be safe to call from any thread, including
 * blocking-pool threads and another Runtime's workers.
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
    virtual Rc<Waker> clone() = 0;

    /// @brief Returns a weak reference to the waker `clone()` returns.
    ///
    /// The default goes through `clone()`, a strong reference taken and dropped. A waker
    /// that can hand out a weak reference to itself directly should override this. The
    /// default is only right when `clone()` returns this waker itself: a weak reference
    /// to a fresh copy expires at once. A non-owning `Context` calls this.
    virtual Weak<Waker> weak_clone() { return clone(); }
};

} // namespace coro::detail
