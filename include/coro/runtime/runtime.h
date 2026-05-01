#pragma once

#include <coro/runtime/executor.h>
#include <coro/runtime/single_threaded_uv_executor.h>
#include <coro/task/spawn_on.h>
#include <coro/runtime/uv_future.h>
#include <coro/future.h>
#include <coro/detail/poll_result.h>
#include <coro/task/spawn_builder.h>
#include <coro/task/spawn_blocking.h>
#include <coro/stream.h>
#include <coro/detail/task.h>
#include <coro/detail/task_state.h>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>

namespace coro {

// Forward declarations — set_current_runtime must be visible inside block_on's body
// because non-dependent names in templates are resolved at the point of definition.
class Runtime;
void set_current_runtime(Runtime* rt);
Runtime& current_runtime();

/**
 * @brief Top-level runtime object. Entry point for all async execution.
 *
 * Owns the @ref Executor (and, in a future phase, the thread pool and libuv event loop).
 * Construct one `Runtime` per application and call `block_on()` to drive async work
 * from a synchronous context (e.g. `main()`).
 *
 * `Runtime` is not copyable or movable.
 */
class Runtime {
public:
    /// @brief Constructs a Runtime with the default executor for the given thread count.
    /// `num_threads <= 1` → SingleThreadedExecutor; otherwise → WorkStealingExecutor.
    explicit Runtime(std::size_t num_threads = std::thread::hardware_concurrency());

    /// @brief Constructs a Runtime with an explicit executor type.
    ///
    /// The executor is constructed as `ExecutorType(args..., this)` — `this` is appended
    /// automatically so callers do not need to pass the Runtime pointer explicitly.
    ///
    /// Example:
    /// @code
    /// Runtime rt(std::in_place_type<WorkSharingExecutor>, 4);
    /// @endcode
    template<typename ExecutorType, typename... Args>
    explicit Runtime(std::in_place_type_t<ExecutorType>, Args&&... args)
        : m_blocking_pool(this),
          m_executor(std::make_unique<ExecutorType>(this, std::forward<Args>(args)...))
    {}

    ~Runtime();

    Runtime(const Runtime&)            = delete;
    Runtime& operator=(const Runtime&) = delete;

    /**
     * @brief Runs `future` on the calling thread, blocking until it completes.
     *
     * Sets the thread-local current runtime for the duration of the call so that
     * free `spawn()` calls inside the future resolve to this runtime.
     *
     * @tparam F A type satisfying @ref Future.
     * @param future The top-level future to drive to completion.
     * @return The value produced by `future` (void for `Future<void>`).
     * @throws Any exception propagated out of the future.
     */
    template<Future F>
    typename F::OutputType block_on(F future) {
        set_current_runtime(this);
        set_current_uv_executor(&m_uv_executor);

        auto impl = std::make_shared<detail::TaskImpl<F>>(std::move(future));
        // Category 2 (doc/task_ownership.md): aliased shared_ptr into the same
        // TaskImpl allocation as the executor's ref. Acts as the lifetime anchor for
        // the root task — the sole persistent strong reference once the executor parks
        // it. Lives on this call stack until wait_for_completion() returns.
        std::shared_ptr<detail::TaskState<typename F::OutputType>> state = impl;
        m_executor->schedule(std::shared_ptr<detail::TaskBase>(impl));

        m_executor->wait_for_completion(*state);

        set_current_runtime(nullptr);
        set_current_uv_executor(nullptr);

        if (state->exception)
            std::rethrow_exception(state->exception);
        if constexpr (!std::is_void_v<typename F::OutputType>)
            return std::move(*state->result);
    }

    /// @brief Submits a pre-constructed task directly. Used internally by @ref JoinSet.
    void schedule_task(std::shared_ptr<detail::TaskBase> task) {
        m_executor->schedule(std::move(task));
    }

    /**
     * @brief Spawns `future` as a background task and returns a @ref JoinHandle.
     *
     * Use `build_task().name("...").spawn(future)` to set a task name.
     *
     * @warning Spawned futures may capture references to data owned by the spawning
     *          coroutine. The coroutine must outlive the spawned task — use
     *          `co_invoke` + `JoinSet::drain()` to enforce the lifetime boundary.
     */
    template<Future F>
    [[nodiscard]] JoinHandle<typename F::OutputType> spawn(F future) {
        return SpawnBuilder(m_executor.get()).spawn(std::move(future));
    }

    /**
     * @brief Spawns `stream` as a background task and returns a @ref StreamHandle.
     *
     * Use `build_task().name("...").buffer(N).spawn(stream)` to configure the task.
     * The stream runs as a `StreamDriver` task that pushes items into a bounded channel.
     */
    template<Stream S>
    [[nodiscard]] StreamHandle<typename S::ItemType> spawn(S stream) {
        return SpawnBuilder(m_executor.get()).spawn(std::move(stream));
    }

    /**
     * @brief Returns a @ref SpawnBuilder for configuring a task before spawning it.
     *
     * Use to set a task name or stream buffer size before spawning:
     * @code
     * auto h = rt.build_task().name("worker").spawn(my_future);
     * auto s = rt.build_task().name("reader").buffer(128).spawn(my_stream);
     * @endcode
     */
    [[nodiscard]] SpawnBuilder build_task() {
        return SpawnBuilder(m_executor.get());
    }

    /// @brief Returns the runtime's SingleThreadedUvExecutor.
    SingleThreadedUvExecutor& uv_executor() { return m_uv_executor; }

    /// @brief Returns the runtime's BlockingPool. Used by spawn_blocking().
    BlockingPool& blocking_pool() { return m_blocking_pool; }

private:
    // Declaration order matters for destruction (members destroyed in reverse order):
    //   m_uv_executor — owns the uv thread and loop; must outlive everything else.
    //   m_blocking_pool — must outlive m_executor so blocking threads can still
    //                     call current_runtime() during their final work item.
    //   m_executor    — worker threads may call waker->wake() which routes through
    //                   m_uv_executor; destroyed first so all wakes land before
    //                   m_uv_executor shuts down.
    SingleThreadedUvExecutor  m_uv_executor;
    BlockingPool              m_blocking_pool;
    std::unique_ptr<Executor> m_executor;
};


/// @brief Sets the thread-local current runtime. Called by `Runtime::block_on()` and worker threads.
void set_current_runtime(Runtime* rt);

/// @brief Returns the thread-local current runtime.
/// @throws std::runtime_error if called outside a `Runtime::block_on()` context.
Runtime& current_runtime();

/**
 * @brief Spawns a @ref Future on the current runtime and returns a @ref JoinHandle.
 *
 * Equivalent to `current_runtime().spawn(future)`. May only be called from within
 * a `Runtime::block_on()` context. Use `build_task().name("...").spawn(future)` to
 * set a task name.
 */
template<Future F>
[[nodiscard]] JoinHandle<typename F::OutputType> spawn(F future) {
    return current_runtime().spawn(std::move(future));
}

/**
 * @brief Spawns a @ref Stream on the current runtime and returns a @ref StreamHandle.
 *
 * Equivalent to `current_runtime().spawn(stream)`. May only be called from within
 * a `Runtime::block_on()` context.
 */
template<Stream S>
[[nodiscard]] StreamHandle<typename S::ItemType> spawn(S stream) {
    return current_runtime().spawn(std::move(stream));
}

/**
 * @brief Returns a @ref SpawnBuilder for configuring a task on the current runtime.
 *
 * May only be called from within a `Runtime::block_on()` context.
 * @code
 * auto h = build_task().name("worker").spawn(my_future);
 * @endcode
 */
[[nodiscard]] inline SpawnBuilder build_task() {
    return current_runtime().build_task();
}

} // namespace coro
