#pragma once
#ifndef CORO_PICO
#error "pico_executor.h requires CORO_PICO to be defined. This header is only for Raspberry Pi Pico builds."
#endif

// Pico-specific single-threaded executor that drives coroutine tasks alongside
// the CYW43 WiFi chip event loop.
//
// Dependencies: pico_cyw43_arch (provides cyw43_arch_poll()) must be linked.
//
// NOTE: std::condition_variable (used by detail::TaskStateBase) requires either
// FreeRTOS or a suitable C++ runtime polyfill. For bare-metal Pico without an
// RTOS, wait_for_completion() never actually blocks on the condvar — it polls in
// a tight loop — but the condvar must be linkable. Enabling FreeRTOS or using
// pico-sdk's pico_cxx_options target resolves this if you see link errors.

#include <coro/runtime/executor.h>
#include <coro/detail/task.h>
#include <coro/detail/task_state.h>
#include <memory>
#include <queue>

namespace coro {

/**
 * @brief Executor for Raspberry Pi Pico (RP2040/RP2350) using the Pico SDK.
 *
 * Internal implementation class — the public entry point is @ref Runtime.
 * All tasks run on the calling thread. `wait_for_completion()` drives both the
 * coroutine ready queue and the CYW43 WiFi chip event loop in alternation:
 *
 * @code
 * loop:
 *   drain_ready_tasks()     // poll all runnable coroutines
 *   cyw43_arch_poll()       // process I/O events; fires lwIP callbacks inline
 * @endcode
 *
 * Because everything runs on a single thread there is no remote injection queue
 * and no locking in `enqueue()`. The CYW43_ARCH_POLL mode guarantees that lwIP
 * callbacks fire synchronously inside `cyw43_arch_poll()`, so wakers set by a
 * @ref PicoCallbackResult or @ref PicoTcpStream are always called on this thread.
 */
class PicoExecutor : public Executor {
public:
    PicoExecutor()  = default;
    ~PicoExecutor() override = default;

    PicoExecutor(const PicoExecutor&)            = delete;
    PicoExecutor& operator=(const PicoExecutor&) = delete;
    PicoExecutor(PicoExecutor&&)                 = delete;
    PicoExecutor& operator=(PicoExecutor&&)      = delete;

    /// Takes ownership of `task` and enqueues it for its first poll.
    void schedule(std::shared_ptr<detail::TaskBase> task) override;

    /// Re-enqueues a woken task. Always called on the executor thread
    /// (CYW43_ARCH_POLL mode), so no locking is required.
    void enqueue(std::shared_ptr<detail::TaskBase> task) override;

    /// @brief Runs the event loop until `state.terminated`.
    ///
    /// Alternates between draining the coroutine ready queue and calling
    /// `cyw43_arch_poll()` to process WiFi and lwIP events. Never blocks
    /// — idles by polling rather than sleeping. For battery-sensitive
    /// applications, add a short `sleep_us(100)` when both queues are empty.
    void wait_for_completion(detail::TaskStateBase& state) override;

    /// @brief Drains the ready queue: polls each task once in FIFO order.
    /// @return `true` if at least one task was polled.
    bool poll_ready_tasks();

private:
    std::queue<std::shared_ptr<detail::TaskBase>> m_ready;
};

} // namespace coro
