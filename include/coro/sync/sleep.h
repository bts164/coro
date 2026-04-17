#pragma once

#include <coro/detail/poll_result.h>
#include <coro/detail/context.h>
#include <coro/detail/waker.h>
#include <coro/runtime/single_threaded_uv_executor.h>
#include <coro/task/join_set.h>
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
 * @note Timer resolution is **milliseconds** (libuv limitation). The remaining
 *       duration is converted with `std::chrono::ceil<milliseconds>` so the
 *       timer never fires before the deadline. Actual wake latency is subject
 *       to I/O-thread scheduling jitter; sub-millisecond precision is not
 *       achievable regardless. See doc/libuv_integration.md § Timer resolution.
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
            // Always submit CancelRequest — its execute() uses fired.exchange(true)
            // to avoid a double-close if the timer already fired before we get there.
            // Pass m_state as a shared_ptr so State stays alive until the I/O thread
            // processes the request, even after we release our own reference.
            // Use the cached m_uv_exec rather than current_uv_executor() because
            // the thread-local may have been cleared by the time the destructor runs.
            m_uv_exec->submit(std::make_unique<CancelRequest>(m_state));
        }
    }

    SleepFuture(const SleepFuture&)            = delete;
    SleepFuture& operator=(const SleepFuture&) = delete;
    SleepFuture(SleepFuture&&)                 = default;
    SleepFuture& operator=(SleepFuture&&)      = default;

    PollResult<void> poll(detail::Context& ctx) {
        auto now = std::chrono::steady_clock::now();

        // Check the fired flag before the clock: libuv and steady_clock both use
        // CLOCK_MONOTONIC on Linux, but OS jitter can cause the timer callback to
        // fire a few microseconds before steady_clock::now() crosses m_deadline.
        // Treating fired==true as authoritative avoids a liveness bug where the
        // clock check returns PollPending with the one-shot timer already consumed.
        if (m_state &&m_state->fired.load(std::memory_order_acquire)) {
            if (now < m_deadline) {
                // std::cerr << "SleepFuture: fired was set before deadline (now=" << now.time_since_epoch().count()
                //             << "ns, deadline=" << m_deadline.time_since_epoch().count() << "ns)\n";
                //std::abort();
            }
            return PollReady;
        }

        if (std::chrono::steady_clock::now() >= m_deadline)
            return PollReady;

        if (!m_state) {
            // First poll: allocate shared state and register the timer.
            // Cache the uv executor pointer so the destructor can cancel the timer
            // even if the thread-local has been cleared before this future is destroyed.
            m_uv_exec = &current_uv_executor();
            m_state = std::make_shared<State>();
            m_state->waker.store(ctx.getWaker());
            m_uv_exec->submit(std::make_unique<StartRequest>(m_state, m_deadline));
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
    // StartRequest — arms the one-shot libuv timer on the I/O thread.
    //
    // Holds a shared_ptr<State> so State stays alive from submission until
    // close_cb fires, regardless of when SleepFuture is destroyed.
    // handle.data stores a heap-allocated shared_ptr<State> wrapper so that
    // close_cb (which only receives a void*) can decrement the ref count.
    // -----------------------------------------------------------------------
    struct StartRequest : IoRequest {
        std::shared_ptr<State>                                              state;
        std::chrono::time_point<std::chrono::steady_clock,
                                std::chrono::milliseconds>                  deadline;

        StartRequest(std::shared_ptr<State> s,
                     std::chrono::time_point<std::chrono::steady_clock,
                                             std::chrono::milliseconds> d)
            : state(std::move(s)), deadline(d) {}

        void execute(uv_loop_t* loop) override {
            uv_timer_init(loop, &state->handle);
            // Store a heap-allocated shared_ptr wrapper in handle->data.
            // close_cb deletes this wrapper, decrementing the ref count.
            state->handle.data = new std::shared_ptr<State>(state);
            // m_deadline is already in whole milliseconds (ceiled at construction),
            // so this subtraction is exact — no rounding needed here.
            auto now_ms = std::chrono::floor<std::chrono::milliseconds>(
                std::chrono::steady_clock::now());
            auto ms = (deadline - now_ms).count();
            uv_timer_start(&state->handle, timer_cb,
                           static_cast<uint64_t>(std::max<int64_t>(0, ms)), 0);
        }
    };

    // -----------------------------------------------------------------------
    // CancelRequest — stops and closes the timer handle on the I/O thread.
    //
    // Holds a shared_ptr<State> so State stays alive until this request is
    // processed, even if SleepFuture has already been destroyed.
    // -----------------------------------------------------------------------
    struct CancelRequest : IoRequest {
        std::shared_ptr<State> state;

        explicit CancelRequest(std::shared_ptr<State> s) : state(std::move(s)) {}

        void execute(uv_loop_t* /*loop*/) override {
            // Atomically claim the uv_close. If timer_cb already won the exchange
            // (fired==true), it owns the close — this is a safe no-op.
            if (!state->fired.exchange(true)) {
                uv_timer_stop(&state->handle);
                uv_close(reinterpret_cast<uv_handle_t*>(&state->handle), close_cb);
            }
        }
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
