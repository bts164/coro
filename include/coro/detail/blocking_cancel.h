#pragma once

// Desktop only (excluded under CORO_PICO, like spawn_blocking itself). The part of
// blocking-task cancellation that coro/future.h needs for blocking_wait().
//
// Kept separate from detail/blocking_task.h because that header needs TaskBase
// (detail/task.h), which itself includes coro/future.h. Everything here goes through an
// opaque BlockingTaskBase and is defined out of line in src/task/blocking_pool.cpp.
// See doc/design/spawn_blocking.md and doc/design/blocking_wait.md.

#include <coro/detail/rc.h>
#include <coro/detail/waker.h>

namespace coro {

/**
 * @brief Thrown at a cancellation point of a cancelled blocking task (see
 * @ref spawn_blocking), to unwind the callable.
 *
 * Deliberately not derived from `std::exception`, so `catch (const std::exception&)`
 * does not swallow it. A callable that catches it (or `catch (...)`) to clean up must
 * rethrow. Cancellation is sticky: every later cancellation point throws again.
 *
 * Also thrown by `blocking_wait()`, on any thread, when the future it waits on reports
 * `PollDropped`.
 */
struct BlockingCancelled {};

/**
 * @brief Throws @ref BlockingCancelled if the calling blocking task has been asked to
 * cancel. For callables that work for long stretches without waiting on anything.
 *
 * No-op on a thread that is not running a `spawn_blocking` callable, and while a
 * `BlockingCancelShield` is alive on the thread.
 */
void blocking_cancellation_point();

namespace detail {

struct BlockingTaskBase;

/// The blocking task whose callable is running on this thread, or nullptr on any
/// thread that is not a blocking pool thread inside a callable.
BlockingTaskBase* current_blocking_task() noexcept;

/// True if `task` has been asked to cancel and no BlockingCancelShield is alive.
/// Called only from the task's own thread.
bool blocking_cancel_pending(const BlockingTaskBase& task) noexcept;

/// `task` as a waker (a reference-count increment, no allocation).
Rc<Waker> blocking_task_waker(BlockingTaskBase& task);

/// BlockingTaskBase::park(): blocks the calling thread, which must be the one running
/// `task`, until the task is woken. A return means "poll again", not "ready".
void blocking_task_park(BlockingTaskBase& task);

} // namespace detail

} // namespace coro
