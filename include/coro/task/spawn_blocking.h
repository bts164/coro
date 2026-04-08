#pragma once

#include <coro/detail/context.h>
#include <coro/detail/poll_result.h>
#include <coro/detail/waker.h>
#include <chrono>
#include <concepts>
#include <condition_variable>
#include <exception>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>

namespace coro {

// ---------------------------------------------------------------------------
// BlockingState<T> — heap-allocated shared state between BlockingHandle<T>
// and the blocking pool thread executing the callable.
//
// All fields are accessed under `mutex`. Using a mutex rather than atomics
// avoids the subtle missed-wakeup race that arises when the waker is stored
// between the blocking thread's complete check and notify.
// ---------------------------------------------------------------------------
template<typename T>
struct BlockingState {
    std::mutex              mutex;
    std::condition_variable cv;       // for blocking_get()
    std::shared_ptr<detail::Waker>   waker;    // stored by poll(), read by blocking thread
    // std::expected<void, E> is valid; using optional<expected<>> so the result
    // starts empty and is set exactly once. This also correctly handles T =
    // std::exception_ptr without ambiguity (result vs. captured exception).
    std::optional<std::expected<T, std::exception_ptr>> result;
    bool complete{false};
};

// ---------------------------------------------------------------------------
// BlockingHandle<T> — Future<T> returned by spawn_blocking().
//
// Move-only. Using a moved-from or null handle throws std::logic_error.
// Dropping the handle detaches the blocking thread — it continues to
// completion and the result is discarded.
// ---------------------------------------------------------------------------
template<typename T>
class [[nodiscard]] BlockingHandle {
public:
    using OutputType = T;

    explicit BlockingHandle(std::shared_ptr<BlockingState<T>> state)
        : m_state(std::move(state)) {}

    BlockingHandle(const BlockingHandle&)            = delete;
    BlockingHandle& operator=(const BlockingHandle&) = delete;

    BlockingHandle(BlockingHandle&&) noexcept            = default;
    BlockingHandle& operator=(BlockingHandle&&) noexcept = default;

    // Destructor detaches — the blocking thread runs to completion and the
    // result is discarded. Does NOT wait for the thread to finish.
    ~BlockingHandle() = default;

    // poll() — called by the executor when the coroutine is resumed.
    // Acquires mutex so the waker store and the blocking thread's complete
    // check are in the same critical section — no missed-wakeup gap.
    PollResult<T> poll(detail::Context& ctx) {
        if (!m_state)
            throw std::logic_error("BlockingHandle::poll called on a moved-from handle");

        std::lock_guard lock(m_state->mutex);
        if (m_state->complete) {
            auto result = std::move(*m_state->result);
            if (!result.has_value())
                return PollError(result.error());
            if constexpr (std::is_void_v<T>)
                return PollReady;
            else
                return std::move(result.value());
        }
        m_state->waker = ctx.getWaker();
        return PollPending;
    }

    // blocking_get() — synchronous wait for use from blocking pool threads
    // (recursive spawn_blocking) or other non-coroutine contexts.
    //
    // WARNING: do NOT call from an executor worker thread — it will block the
    // worker and starve all other tasks on that thread.
    T blocking_get() {
        if (!m_state)
            throw std::logic_error("BlockingHandle::blocking_get called on a moved-from handle");

        std::unique_lock lock(m_state->mutex);
        m_state->cv.wait(lock, [this] { return m_state->complete; });

        auto result = std::move(*m_state->result);
        if (!result.has_value())
            std::rethrow_exception(result.error());
        if constexpr (!std::is_void_v<T>)
            return std::move(result.value());
    }

private:
    std::shared_ptr<BlockingState<T>> m_state;
};

// Forward declaration — BlockingPool stores a Runtime* to set the thread-local
// on each worker thread. Cannot include runtime.h here (circular dependency).
class Runtime;

// ---------------------------------------------------------------------------
// BlockingPool — owned by Runtime. Manages a pool of detached threads that
// execute blocking work items. Threads are created lazily and exit after
// a keep-alive timeout with no work, shrinking the pool back down.
// ---------------------------------------------------------------------------
class BlockingPool {
public:
    static constexpr std::size_t kDefaultMaxThreads = 512;
    static constexpr auto        kKeepAlive = std::chrono::seconds(10);

    // rt must be the owning Runtime. It is used by worker threads to set their
    // thread-local current_runtime so recursive spawn_blocking calls work.
    explicit BlockingPool(Runtime* rt, std::size_t max_threads = kDefaultMaxThreads);
    ~BlockingPool();

    BlockingPool(const BlockingPool&)            = delete;
    BlockingPool& operator=(const BlockingPool&) = delete;

    // Submit a work item to be executed on a blocking pool thread.
    // Called by spawn_blocking() after allocating BlockingState.
    void submit(std::function<void()> work_item);

private:
    void worker_loop();

    Runtime*                m_runtime;
    std::mutex              m_mutex;
    std::condition_variable m_cv;
    std::vector<std::function<void()>> m_queue;
    std::size_t             m_total_threads{0};
    std::size_t             m_idle_threads{0};
    std::size_t             m_max_threads;
    bool                    m_stop{false};
};

// ---------------------------------------------------------------------------
// spawn_blocking() — free function entry point.
//
// Submits the callable to the BlockingPool owned by the current Runtime and
// returns a BlockingHandle<T> the caller can co_await.
//
// Requires a running Runtime (same as coro::spawn()).
//
// OWNERSHIP: the callable must own all its data. Do NOT capture references
// or pointers into the spawning coroutine's stack frame — dropping the handle
// detaches the thread immediately and the coroutine may destroy those locals
// before the thread finishes.
// ---------------------------------------------------------------------------

namespace detail {
// Non-template helper defined in blocking_pool.cpp. Submits a type-erased work
// item to the BlockingPool owned by the current runtime. Keeping this out of the
// template body means spawn_blocking.h needs no knowledge of Runtime, eliminating
// the circular-include dependency.
void submit_blocking_work(std::function<void()> work);
} // namespace detail

template<std::invocable F>
[[nodiscard]] BlockingHandle<std::invoke_result_t<F>>
spawn_blocking(F&& f) {
    using T = std::invoke_result_t<F>;

    auto state = std::make_shared<BlockingState<T>>();

    detail::submit_blocking_work(
        [state, f = std::forward<F>(f)]() mutable {
            try {
                if constexpr (std::is_void_v<T>) {
                    f();
                    std::lock_guard lock(state->mutex);
                    state->result = std::expected<T, std::exception_ptr>();
                } else {
                    auto value = f();
                    std::lock_guard lock(state->mutex);
                    state->result = std::move(value);
                }
            } catch (...) {
                std::lock_guard lock(state->mutex);
                state->result = std::unexpected(std::current_exception());
            }

            std::shared_ptr<detail::Waker> w;
            {
                std::lock_guard lock(state->mutex);
                state->complete = true;
                w = state->waker;
            }
            state->cv.notify_one();
            if (w) w->wake();
        });

    return BlockingHandle<T>(std::move(state));
}

} // namespace coro
