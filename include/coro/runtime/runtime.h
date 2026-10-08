#pragma once

#include <coro/runtime/executor.h>
#include <coro/runtime/clock.h>
#include <coro/detail/timer_queue.h>
#ifndef CORO_PICO
#include <coro/runtime/io_driver.h>
#include <coro/task/spawn_on.h>
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
#include <coro/detail/rc.h>
#ifdef CORO_PICO
#include <coro/detail/isr_flag.h>
#endif
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace coro {

#ifdef CORO_PICO
class CurrentThreadExecutor;  // forward declaration for Runtime::m_current_thread_executor
#endif

// Forward declarations — must be visible inside template bodies below because
// non-dependent names are resolved at the point of definition.
class Runtime;
void set_current_runtime(Runtime* rt);
Runtime& current_runtime();

/**
 * @brief Top-level runtime object. Entry point for all async execution.
 *
 * Owns the @ref Executor and, in the standard build, the epoll I/O driver and the
 * blocking pool. Construct one `Runtime` per application and call `block_on()` to
 * drive async work from a synchronous context (e.g. `main()`).
 *
 * `Runtime` is not copyable or movable.
 *
 * In the Pico port (`CORO_PICO`), the Runtime owns a @ref CurrentThreadExecutor and
 * exposes `poll()` to service the ready queue from the firmware event loop.
 */
class Runtime {
public:
#ifdef CORO_PICO
    /// @brief Constructs a Runtime backed by CurrentThreadExecutor.
    ///
    /// `enable_network` gates whether the event loop calls `cyw43_arch_poll()`
    /// each iteration. Pass `false` for firmware that never touches WiFi/lwIP
    /// (`coro::pico`/`coro::pico_hal` still unconditionally link
    /// `pico_cyw43_arch_lwip_poll` -- see cmake/platforms/pico.cmake -- so the
    /// symbol is always present; this only controls whether it's ever called).
    /// Skipping the call matters on boards with no CYW43 chip wired up at all
    /// (a plain, non-W Pico): `cyw43_arch_poll()` touches driver state that was
    /// never initialized there, since `cyw43_arch_init()` was never called --
    /// calling it anyway is undefined behavior, not just a wasted poll.
    ///
    /// Leave at the default (`true`) for any firmware that does call
    /// `cyw43_arch_init()` (Pico W boards) -- including firmware that connects
    /// to WiFi from a coroutine after the Runtime has already started, since
    /// unlike `cyw43_arch_wifi_connect_blocking()`'s hand-rolled wait loop, an
    /// async connect (`cyw43_arch_wifi_connect_async()`) depends on this
    /// same per-iteration `cyw43_arch_poll()` to make progress.
    explicit Runtime(bool enable_network = true);
    ~Runtime() = default;

    /// @brief Adds a timer that wakes `waker` once `deadline` has passed, on the
    /// CurrentThreadExecutor's queue. Used by SleepFuture.
    /// @return The id to pass to cancel_timer().
    detail::TimerId add_timer(Instant deadline, detail::Weak<detail::Waker> waker);

    /// @brief Cancels a timer added by add_timer(). Does nothing if it has already
    /// fired or been cancelled.
    void cancel_timer(detail::TimerId id) noexcept;

    /// @brief Registers an ISR-safe waiter to be peeked once per event loop iteration.
    ///
    /// `entry`'s non-mutating is_ready() is checked once per loop iteration; when it
    /// returns true, the waker is fired (the registration is NOT removed here -- see
    /// doc/design/isr_safety.md, "Multiple waiters"). Called by IsrWaitFuture and
    /// friends — do not call directly. Must be called from the executor thread (i.e.
    /// from inside a coroutine), never from an ISR.
    void register_isr_poll(IsrPollEntry* entry, detail::Rc<detail::Waker> waker);

    /// @brief Removes an ISR poll registration.
    ///
    /// Called by the owning waiter's destructor/move-assignment once its wait
    /// completes or is cancelled. Prevents the executor from dereferencing an
    /// IsrPollEntry* whose backing object may have been destroyed. Matched by
    /// `entry`'s own identity, so this always removes exactly the caller's own
    /// registration, even when other waiters share the same underlying flag/count.
    void remove_isr_poll(IsrPollEntry* entry);

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
    /// `num_threads <= 1` → CurrentThreadExecutor (tasks run on the thread calling
    /// `block_on()`, which also turns the I/O driver); otherwise → WorkStealingExecutor.
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

    /// @brief Returns the runtime's epoll I/O driver. See doc/design/io_driver.md.
    IoDriver& io_driver() { return m_io_driver; }

    /// @brief True if the executor turns io_driver(). Driver-backed I/O primitives
    /// (e.g. `UdpSocket::bind()`) throw `std::logic_error` when it is false.
    bool turns_io_driver() const noexcept { return m_executor->turns_io_driver(); }

    /// @brief Adds a timer that wakes `waker` once `deadline` has passed, on the
    /// driver's queue. Used by SleepFuture.
    /// @return The id to pass to cancel_timer().
    /// @throws std::logic_error if the executor doesn't turn the driver, where the
    ///         timer could never fire.
    detail::TimerId add_timer(Instant deadline, detail::Weak<detail::Waker> waker);

    /// @brief Cancels a timer added by add_timer(). Thread-safe. Does nothing if it
    /// has already fired or been cancelled.
    ///
    /// Safe to call while the executor is being destroyed, which is when the futures
    /// of unfinished tasks cancel their timers.
    void cancel_timer(detail::TimerId id) noexcept;

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
        auto impl = detail::make_rc<detail::TaskImpl<F>>(std::move(future));
        // Category 2 (doc/task_ownership.md): aliased shared_ptr into the same
        // TaskImpl allocation. Provides typed access to the result and waker slot.
        // The executor's owned map (Category 1) anchors the task's lifetime until
        // it reaches a terminal state. Lives on this call stack until
        // wait_for_completion() returns.
        detail::Rc<detail::TaskState<typename F::OutputType>> state = impl;
#ifdef CORO_PICO
        impl->set_self(detail::Weak<detail::TaskBase>(impl));
#endif
        m_executor->schedule(detail::Rc<detail::TaskBase>(impl));

        m_executor->wait_for_completion(*state);

        set_current_runtime(nullptr);
        if (state->exception)
            std::rethrow_exception(state->exception);
        if constexpr (!std::is_void_v<typename F::OutputType>)
            return std::move(*state->result);
    }

    /// @brief Submits a pre-constructed task directly. Used internally by @ref JoinSet.
    void schedule_task(detail::Rc<detail::TaskBase> task) {
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
    CurrentThreadExecutor*    m_current_thread_executor = nullptr;
    std::unique_ptr<Executor> m_executor;
#else
    // Declaration order matters for destruction (members destroyed in reverse order):
    //   m_io_driver   — must outlive m_executor: tasks own futures that own
    //                   IoRegistrations, which deregister on destruction, and the
    //                   executor's IoDriverParker unparks it. Only executor threads
    //                   turn it, so nothing dispatches once m_executor is gone.
    //                   Declared before m_blocking_pool so a late wake from a
    //                   blocking thread can still unpark it.
    //   m_blocking_pool — must outlive m_executor so blocking threads can still
    //                     call current_runtime() during their final work item.
    //   m_executor    — destroyed first: joins its worker threads, so no task runs
    //                   once the members above start going away.
    IoDriver                  m_io_driver;
    BlockingPool              m_blocking_pool;
    std::unique_ptr<Executor> m_executor;
#endif
};

/// @brief Sets the thread-local current runtime. Called by `Runtime::block_on()` and worker threads.
void set_current_runtime(Runtime* rt);

/// @brief Schedules a pre-constructed task on the current runtime.
/// Used internally by `JoinSet::spawn()`.
inline void schedule_task(detail::Rc<detail::TaskBase> task) {
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
