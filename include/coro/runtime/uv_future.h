#pragma once

#include <coro/detail/context.h>
#include <coro/detail/poll_result.h>
#include <coro/future.h>
#include <functional>
#include <mutex>
#include <optional>
#include <tuple>

namespace coro {

// ---------------------------------------------------------------------------
// UvCallbackResult<Args...>
// ---------------------------------------------------------------------------

/**
 * @brief Shared state between a libuv callback and a @ref UvFuture.
 *
 * Allocate one of these on the coroutine frame (stack), store its raw pointer
 * in `req.data` or a handle's `.data` field, then `co_await wait(result)`.
 * The uv callback calls `complete(args...)`, which stores the result and wakes
 * the awaiting coroutine.
 *
 * ### Lifetime
 * The object must outlive the libuv callback. `co_await wait(result)` keeps
 * the coroutine suspended (and therefore the frame alive) until `complete()`
 * fires, so a stack-allocated `UvCallbackResult` is safe for the common case
 * of an immediate `co_await`.
 *
 * ### Thread safety
 * `complete()` may be called from any thread (the uv I/O thread). It acquires
 * the internal mutex before writing `value` and reading `waker`, so there is
 * no data race with @ref UvFuture::poll() running on the executor thread.
 */
template<typename... Args>
struct UvCallbackResult {
    std::mutex                                mutex;
    std::shared_ptr<detail::Waker>            waker;   // GUARDED BY mutex
    std::optional<std::tuple<Args...>>        value;   // GUARDED BY mutex; set once by complete()

    /// Called from the uv callback. Thread-safe.
    void complete(Args... args) {
        std::shared_ptr<detail::Waker> to_wake;
        {
            std::lock_guard lock(mutex);
            value.emplace(std::forward<Args>(args)...);
            to_wake = std::move(waker);
        }
        if (to_wake) to_wake->wake();
    }
};

// ---------------------------------------------------------------------------
// UvFuture<Args...>
// ---------------------------------------------------------------------------

/**
 * @brief One-shot Future that completes when the associated libuv callback fires.
 *
 * Pair with @ref UvCallbackResult: declare one on the coroutine frame, store
 * its address in `req.data`, start the uv operation, then `co_await wait(result)`.
 *
 * The coroutine using this future **must run on the uv executor** (via
 * `with_context`) so that the callback fires on the same thread that drains
 * the ready queue. This is not enforced at compile time.
 *
 * **OutputType** is `std::tuple<Args...>`. Use structured bindings to unpack:
 * @code
 * UvCallbackResult<ssize_t> result;
 * req.data = &result;
 * uv_fs_read(loop, &req, fd, &buf, 1, offset, [](uv_fs_t* r) {
 *     static_cast<UvCallbackResult<ssize_t>*>(r->data)->complete(r->result);
 * });
 * auto [nbytes] = co_await wait(result);
 * @endcode
 *
 * **Cancellation:** pass a cancel callable as the second constructor argument.
 * It is invoked from the destructor if the callback has not yet fired. Use
 * this to call `uv_cancel()` on the underlying request so the event loop can
 * drain cleanly.
 *
 * @tparam Args  Argument types passed to `UvCallbackResult::complete()`.
 *               Must match exactly.
 */
template<typename... Args>
class UvFuture {
public:
    using OutputType = std::tuple<Args...>;

    /// Construct without a cancel function. The uv operation runs to completion
    /// even if this future is dropped before the callback fires.
    explicit UvFuture(UvCallbackResult<Args...>& result)
        : m_result(&result)
    {}

    /// Construct with a cancel function invoked if the future is destroyed before
    /// the callback fires. Use to call `uv_cancel()` on the request handle.
    UvFuture(UvCallbackResult<Args...>& result, std::function<void()> cancel_fn)
        : m_result(&result)
        , m_cancel_fn(std::move(cancel_fn))
    {}

    UvFuture(UvFuture&& other) noexcept
        : m_result(std::exchange(other.m_result, nullptr))
        , m_cancel_fn(std::move(other.m_cancel_fn))
    {}
    UvFuture& operator=(UvFuture&& other) noexcept {
        m_result    = std::exchange(other.m_result, nullptr);
        m_cancel_fn = std::move(other.m_cancel_fn);
        return *this;
    }
    UvFuture(const UvFuture&)            = delete;
    UvFuture& operator=(const UvFuture&) = delete;

    ~UvFuture() {
        if (!m_result || !m_cancel_fn) return;
        std::lock_guard lock(m_result->mutex);
        if (!m_result->value.has_value())
            m_cancel_fn();
    }

    PollResult<OutputType> poll(detail::Context& ctx) {
        std::lock_guard lock(m_result->mutex);
        if (m_result->value.has_value())
            return std::move(*m_result->value);
        m_result->waker = ctx.getWaker();
        return PollPending;
    }

private:
    UvCallbackResult<Args...>* m_result = nullptr;
    std::function<void()>      m_cancel_fn;  // null if no cancellation
};

/// Factory function — preferred spelling at call sites.
template<typename... Args>
UvFuture<Args...> wait(UvCallbackResult<Args...>& result) {
    return UvFuture<Args...>(result);
}

} // namespace coro
