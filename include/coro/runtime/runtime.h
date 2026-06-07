#pragma once

#include <coro/runtime/executor.h>
#ifndef CORO_PICO
#include <coro/runtime/single_threaded_uv_executor.h>
#include <coro/task/spawn_on.h>
#include <coro/runtime/uv_future.h>
#include <coro/task/spawn_blocking.h>
#include <mutex>
#include <thread>
#endif
#include <coro/future.h>
#include <coro/detail/poll_result.h>
#include <coro/task/spawn_builder.h>
#include <coro/stream.h>
#include <coro/detail/task.h>
#include <coro/detail/task_state.h>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace coro {

#ifdef CORO_PICO
class PicoExecutor;  // forward declaration for Runtime::m_pico_executor
#endif

// Forward declarations — must be visible inside template bodies below because
// non-dependent names are resolved at the point of definition.
class Runtime;
void set_current_runtime(Runtime* rt);
Runtime& current_runtime();

/**
 * @brief Top-level runtime object. Entry point for all async execution.
 *
 * Owns the @ref Executor and, in the standard build, the thread pool and libuv
 * event loop. Construct one `Runtime` per application and call `block_on()` to
 * drive async work from a synchronous context (e.g. `main()`).
 *
 * `Runtime` is not copyable or movable.
 *
 * In the Pico port (`CORO_PICO`), the Runtime owns a @ref PicoExecutor and
 * exposes `poll()` to service the ready queue from the firmware event loop.
 */
class Runtime {
public:
#ifdef CORO_PICO
    /// @brief Constructs a Runtime backed by PicoExecutor.
    explicit Runtime();
    ~Runtime() = default;

    /// @brief Drains the coroutine ready queue once. Returns true if any task was polled.
    ///
    /// Call this from the firmware main loop alongside `cyw43_arch_poll()`:
    /// @code
    /// while (true) {
    ///     rt.poll();
    ///     cyw43_arch_poll();
    /// }
    /// @endcode
    bool poll();
#else
    /// @brief Constructs a Runtime with the default executor for the given thread count.
    /// `num_threads <= 1` → SingleThreadedExecutor; otherwise → WorkStealingExecutor.
    explicit Runtime(std::size_t num_threads = std::thread::hardware_concurrency());

    /// @brief Constructs a Runtime with an explicit executor type.
    ///
    /// The executor is constructed as `ExecutorType(this, args...)` — `this` is passed
    /// first so callers do not need to pass the Runtime pointer explicitly.
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

    /// @brief Returns the runtime's SingleThreadedUvExecutor.
    SingleThreadedUvExecutor& uv_executor() { return m_uv_executor; }

    /// @brief Returns the runtime's BlockingPool. Used by spawn_blocking().
    BlockingPool& blocking_pool() { return m_blocking_pool; }
#endif

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
#ifndef CORO_PICO
        set_current_uv_executor(&m_uv_executor);
#endif
        auto impl = std::make_shared<detail::TaskImpl<F>>(std::move(future));
        // Category 2 (doc/task_ownership.md): aliased shared_ptr into the same
        // TaskImpl allocation as the executor's ref. Acts as the lifetime anchor for
        // the root task — the sole persistent strong reference once the executor parks
        // it. Lives on this call stack until wait_for_completion() returns.
        std::shared_ptr<detail::TaskState<typename F::OutputType>> state = impl;
        m_executor->schedule(std::shared_ptr<detail::TaskBase>(impl));

        m_executor->wait_for_completion(*state);

        set_current_runtime(nullptr);
#ifndef CORO_PICO
        set_current_uv_executor(nullptr);
#endif
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
     */
    template<Future F>
    [[nodiscard]] JoinHandle<typename F::OutputType> spawn(F future) {
        return SpawnBuilder(m_executor.get()).spawn(std::move(future));
    }

    /**
     * @brief Spawns `stream` as a background task and returns a @ref StreamHandle.
     */
    template<Stream S>
    [[nodiscard]] StreamHandle<typename S::ItemType> spawn(S stream) {
        return SpawnBuilder(m_executor.get()).spawn(std::move(stream));
    }

    /**
     * @brief Returns a @ref SpawnBuilder for configuring a task before spawning it.
     */
    [[nodiscard]] SpawnBuilder build_task() {
        return SpawnBuilder(m_executor.get());
    }

private:
#ifdef CORO_PICO
    PicoExecutor*             m_pico_executor = nullptr;
    std::unique_ptr<Executor> m_executor;
#else
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
#endif
};

/// @brief Sets the thread-local current runtime. Called by `Runtime::block_on()` and worker threads.
void set_current_runtime(Runtime* rt);

/// @brief Schedules a pre-constructed task on the current runtime.
/// Used internally by `JoinSet::spawn()`.
inline void schedule_task(std::shared_ptr<detail::TaskBase> task) {
    current_runtime().schedule_task(std::move(task));
}

/// @brief Returns the thread-local current runtime.
/// @throws std::runtime_error if called outside a `Runtime::block_on()` context.
Runtime& current_runtime();

/**
 * @brief Spawns a @ref Future as a background task and returns a @ref JoinHandle.
 *
 * Works inside any `block_on()` context.
 */
template<Future F>
[[nodiscard]] JoinHandle<typename F::OutputType> spawn(F future) {
    return current_runtime().spawn(std::move(future));
}

/**
 * @brief Spawns a @ref Stream as a background task and returns a @ref StreamHandle.
 *
 * Works inside any `block_on()` context.
 */
template<Stream S>
[[nodiscard]] StreamHandle<typename S::ItemType> spawn(S stream) {
    return current_runtime().spawn(std::move(stream));
}

/**
 * @brief Returns a @ref SpawnBuilder for configuring a task on the current runtime.
 *
 * @code
 * auto h = build_task().name("worker").spawn(my_future);
 * @endcode
 */
[[nodiscard]] inline SpawnBuilder build_task() {
    return current_runtime().build_task();
}

} // namespace coro
