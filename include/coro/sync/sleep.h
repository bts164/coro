#pragma once

#include <coro/detail/poll_result.h>
#include <coro/detail/context.h>
#include <coro/detail/waker.h>
#include <coro/runtime/single_threaded_uv_executor.h>
#include <coro/runtime/uv_future.h>
#include <coro/task/spawn_on.h>
#include <coro/coro.h>
#include <uv.h>
#include <atomic>
#include <chrono>
#include <memory>

namespace coro {

/**
 * @brief Future that completes once a wall-clock deadline has passed.
 *
 * Satisfies @ref Future<void>. On the first `poll()` call after the deadline
 * has not yet passed, registers a one-shot libuv timer via the uv executor.
 * The I/O thread fires the timer at the deadline and calls `waker->wake()`,
 * which re-enqueues the task. The next `poll()` then sees `fired == true`
 * and returns `PollReady`.
 *
 * @note Timer resolution is **milliseconds** (libuv limitation). The deadline is
 *       stored in whole milliseconds (ceiled at construction). Because libuv schedules
 *       timers relative to `loop->time` (frozen at the start of each loop iteration),
 *       which can lag `steady_clock` by up to 1ms, the timer may fire marginally before
 *       the deadline. `poll()` detects this, discards the state, and reschedules for the
 *       remaining ~1ms; the rescheduled timer is guaranteed not to fire early because
 *       `loop->time` has already advanced past the original deadline by then. Actual
 *       wake latency is subject to I/O-thread scheduling jitter; sub-millisecond
 *       precision is not achievable regardless.
 *
 * @note `SleepFuture` must not be shared across threads. It is intended to
 *       live inside a coroutine frame and be polled by a single executor thread.
 *
 * All libuv state, request types, and callbacks are private implementation
 * details of this class.
 *
 * Prefer the @ref sleep_for factory function over constructing this directly.
 */
class SleepFuture {
public:
    using OutputType = void;

    explicit SleepFuture(std::chrono::nanoseconds duration)
        : m_deadline(std::chrono::ceil<std::chrono::milliseconds>(
              std::chrono::steady_clock::now() + duration)) {}

    ~SleepFuture() {
        if (m_state) {
            // Use the cached m_uv_exec rather than current_uv_executor() because
            // the thread-local may have been cleared by the time the destructor runs.
            with_context(*m_uv_exec,
                [](std::shared_ptr<State> state) -> Coro<void> {
                    // fired.exchange(true) avoids double-close if timer_cb already won.
                    if (!state->fired.exchange(true)) {
                        uv_timer_stop(&state->handle);
                        uv_close(reinterpret_cast<uv_handle_t*>(&state->handle), close_cb);
                    }
                    co_return;
                }(std::move(m_state))
            ).detach();
        }
    }

    SleepFuture(const SleepFuture&)            = delete;
    SleepFuture& operator=(const SleepFuture&) = delete;
    SleepFuture(SleepFuture&&)                 = default;
    SleepFuture& operator=(SleepFuture&&)      = default;

    PollResult<void> poll(detail::Context& ctx) {
        if (m_state && m_state->fired.load(std::memory_order_acquire)) {
            if (std::chrono::steady_clock::now() >= m_deadline)
                return PollReady;
            // libuv fired the timer marginally early: loop->time (set at the start of the
            // current loop iteration) can lag steady_clock by up to 1ms, making the computed
            // delay 1ms too short. timer_cb already called uv_close so the handle is being
            // cleaned up — discard the state and fall through to reschedule for the remaining
            // ~1ms. The rescheduled timer cannot fire early again because by the time it fires,
            // loop->time will have advanced past the original deadline.
            m_state = nullptr;
        }

        if (std::chrono::steady_clock::now() >= m_deadline)
            return PollReady;

        if (!m_state) {
            // First poll (or reschedule after early firing): allocate shared state and register
            // the timer. Cache the uv executor pointer so the destructor can cancel the timer
            // even if the thread-local has been cleared before this future is destroyed.
            m_uv_exec = &current_uv_executor();
            m_state = std::make_shared<State>();
            m_state->waker.store(ctx.getWaker());
            using TimePoint = std::chrono::time_point<std::chrono::steady_clock,
                                                      std::chrono::milliseconds>;
            with_context(*m_uv_exec,
                [](std::shared_ptr<State> state, TimePoint deadline) -> Coro<void> {
                    uv_timer_init(current_uv_executor().loop(), &state->handle);
                    state->handle.data = new std::shared_ptr<State>(state);
                    auto now_ms = std::chrono::floor<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now());
                    auto ms = (deadline - now_ms).count();
                    uv_timer_start(&state->handle, timer_cb,
                                   static_cast<uint64_t>(std::max<int64_t>(0, ms)), 0);
                    co_return;
                }(m_state, m_deadline)
            ).detach();
        } else {
            // Re-polled before timer fired (e.g. woken by a select branch).
            // Atomically update the waker — timer_cb may read it concurrently on the I/O thread.
            m_state->waker.store(ctx.getWaker());
        }
        return PollPending;
    }

private:
    // -----------------------------------------------------------------------
    // State — shared between SleepFuture (worker thread) and the I/O thread.
    // Heap-allocated and reference-counted so either side can outlive the other.
    //
    // RACE: waker is written by the worker thread (poll()) and read by the I/O
    // thread (timer_cb). It is therefore std::atomic<shared_ptr<Waker>> (C++20).
    // fired is written by the I/O thread (timer_cb / CancelRequest::execute())
    // and read by the worker thread (poll()). Atomic exchange prevents double-close.
    // -----------------------------------------------------------------------
    struct State : std::enable_shared_from_this<State> {
        uv_timer_t                                  handle;  // must remain first field
        std::atomic<std::shared_ptr<detail::Waker>> waker;
        std::atomic<bool>                           fired{false};
    };

    // -----------------------------------------------------------------------
    // libuv callbacks — static so they satisfy the C function-pointer ABI.
    // Both run exclusively on the I/O thread.
    // -----------------------------------------------------------------------

    /// Fired by libuv when the timer deadline expires.
    static void timer_cb(uv_timer_t* handle) {
        auto* sp    = static_cast<std::shared_ptr<State>*>(handle->data);
        auto* state = sp->get();
        // Atomically claim uv_close. CancelRequest::execute() uses the same
        // exchange, so only one side will call uv_close.
        if (!state->fired.exchange(true)) {
            state->waker.load()->wake();
            uv_close(reinterpret_cast<uv_handle_t*>(handle), close_cb);
        }
    }

    /// Fired by libuv after uv_close() completes. Deletes the shared_ptr wrapper
    /// stored in handle->data, decrementing the State ref count.
    static void close_cb(uv_handle_t* handle) {
        delete static_cast<std::shared_ptr<State>*>(handle->data);
    }

    // -----------------------------------------------------------------------
    // SleepFuture data members
    // -----------------------------------------------------------------------
    // Stored in whole milliseconds, ceiled at construction. This makes the
    // timer resolution explicit regardless of the platform's steady_clock
    // precision, and means no rounding decisions are deferred to execute().
    std::chrono::time_point<std::chrono::steady_clock,
                            std::chrono::milliseconds> m_deadline;
    std::shared_ptr<State>                m_state;       // null until first poll()
    SingleThreadedUvExecutor*             m_uv_exec = nullptr;  // cached on first poll()
};


/**
 * @brief Returns a @ref SleepFuture that completes after `duration` has elapsed.
 *
 * Timer resolution is milliseconds; see @ref SleepFuture for details.
 */
[[nodiscard]] inline SleepFuture sleep_for(std::chrono::nanoseconds duration) {
    return SleepFuture(duration);
}

} // namespace coro
