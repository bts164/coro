#pragma once

#include <coro/runtime/executor.h>
#include <coro/task/spawn_builder.h>
#include <coro/task/join_handle.h>
#include <coro/future.h>

namespace coro {

/**
 * @brief Schedules `future` on `exec` and returns a builder for controlling the task.
 *
 * Unlike `coro::spawn()` (which targets the current runtime's executor), `spawn_on`
 * lets the caller choose the executor explicitly. Useful for routing work to a
 * specific executor — for example, dispatching I/O-bound coroutines to the
 * @ref SingleThreadedUvExecutor.
 *
 * Call `.submit()` on the returned @ref SpawnBuilder to enqueue the task and receive
 * a @ref JoinHandle.
 *
 * @param exec   The executor to schedule the task on.
 * @param future The future to schedule.
 * @return A @ref SpawnBuilder; call `.submit()` to enqueue the task.
 */
template<Future F>
[[nodiscard]] SpawnBuilder<F> spawn_on(Executor& exec, F future) {
    return SpawnBuilder<F>(std::move(future), &exec);
}

/**
 * @brief Schedules `future` on `exec`, suspends the caller until it completes,
 * and returns the result — Kotlin's `withContext` analogue.
 *
 * Equivalent to `co_await spawn_on(exec, future).submit()`. The coroutine runs
 * entirely on `exec`; when it completes its waker fires, resuming the caller on
 * whichever executor is driving the parent task (no explicit return-context switch
 * is needed — the parent's executor re-schedules the parent automatically via its
 * @ref TaskWaker).
 *
 * **Reference capture safety:** if `future` captures references to locals owned by
 * the calling coroutine, those locals must outlive the `co_await`. Since
 * `co_await with_context(...)` suspends the caller until the child completes, this
 * is always satisfied — the caller cannot return before the child finishes.
 *
 * Typical usage (from a coroutine running on a work-stealing executor):
 * @code
 * auto result = co_await with_context(rt.uv_executor(),
 *     co_invoke([&]() -> Coro<int> {
 *         // runs on the uv executor — safe to call libuv APIs here
 *         co_return 42;
 *     }));
 * // resumes on the original executor
 * @endcode
 *
 * @param exec   The executor to run `future` on.
 * @param future The future to execute in the target context.
 * @return A @ref JoinHandle; `co_await` it to wait for the result.
 */
template<Future F>
[[nodiscard]] JoinHandle<typename F::OutputType> with_context(Executor& exec, F future) {
    return spawn_on(exec, std::move(future)).submit();
}

} // namespace coro
