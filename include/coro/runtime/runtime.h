#pragma once

#include <coro/runtime/executor.h>
#include <coro/runtime/io_service.h>
#include <coro/future.h>
#include <coro/detail/poll_result.h>
#include <coro/task/spawn_builder.h>
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
        : m_executor(std::make_unique<ExecutorType>(this, std::forward<Args>(args)...))
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
        set_current_io_service(&m_io_service);

        auto state = std::make_shared<detail::TaskState<typename F::OutputType>>();
        m_executor->schedule(
            std::make_unique<detail::Task>(std::move(future), state));

        m_executor->wait_for_completion(*state);

        set_current_runtime(nullptr);
        set_current_io_service(nullptr);

        if (state->exception)
            std::rethrow_exception(state->exception);
        if constexpr (!std::is_void_v<typename F::OutputType>)
            return std::move(*state->result);
    }

    /// @brief Submits a pre-constructed task directly. Used internally by @ref Synchronize.
    void schedule_task(std::unique_ptr<detail::Task> task) {
        m_executor->schedule(std::move(task));
    }

    /**
     * @brief Returns a builder for spawning a @ref Future as a background task.
     *
     * Call `.submit()` on the returned @ref SpawnBuilder to enqueue the task and
     * receive a @ref JoinHandle.
     *
     * @warning Spawned futures may capture references to data owned by the spawning
     *          coroutine. The coroutine must outlive the spawned task, or use
     *          @ref Synchronize to enforce the lifetime boundary explicitly.
     */
    template<Future F>
    [[nodiscard]] SpawnBuilder<F> spawn(F future) {
        return SpawnBuilder<F>(std::move(future), m_executor.get());
    }

    /**
     * @brief Returns a builder for spawning a @ref Stream as a background task.
     *
     * The stream runs as a `StreamDriver` task that pushes items into a bounded channel.
     * Call `.submit()` on the returned @ref StreamSpawnBuilder to receive a @ref StreamHandle.
     */
    template<Stream S>
    [[nodiscard]] StreamSpawnBuilder<S> spawn(S stream) {
        return StreamSpawnBuilder<S>(std::move(stream), m_executor.get());
    }

    /// @brief Returns the runtime's IoService. Used by worker threads to set their thread-local.
    IoService& io_service() { return m_io_service; }

private:
    // m_io_service must be declared before m_executor so it is destroyed *after* the executor.
    // Worker threads call set_current_io_service(&m_io_service); they must all have joined
    // (executor destructor) before the IoService destructor fires its stop signal.
    IoService                 m_io_service;
    std::unique_ptr<Executor> m_executor;
};


/// @brief Sets the thread-local current runtime. Called by `Runtime::block_on()` and worker threads.
void set_current_runtime(Runtime* rt);

/// @brief Returns the thread-local current runtime.
/// @throws std::runtime_error if called outside a `Runtime::block_on()` context.
Runtime& current_runtime();

/**
 * @brief Spawns a @ref Future on the current runtime. Equivalent to `current_runtime().spawn(future)`.
 *
 * May only be called from within a `Runtime::block_on()` context.
 * @return A @ref SpawnBuilder; call `.submit()` to enqueue the task.
 */
template<Future F>
[[nodiscard]] SpawnBuilder<F> spawn(F future) {
    return current_runtime().spawn(std::move(future));
}

/**
 * @brief Spawns a @ref Stream on the current runtime. Equivalent to `current_runtime().spawn(stream)`.
 *
 * May only be called from within a `Runtime::block_on()` context.
 * @return A @ref StreamSpawnBuilder; call `.submit()` to receive a @ref StreamHandle.
 */
template<Stream S>
[[nodiscard]] StreamSpawnBuilder<S> spawn(S stream) {
    return current_runtime().spawn(std::move(stream));
}

} // namespace coro
