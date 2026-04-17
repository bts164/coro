#pragma once

#include <coro/detail/context.h>
#include <coro/detail/poll_result.h>
#include <coro/future.h>
#include <functional>
#include <memory>
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
 * Allocate one of these on the heap, store its raw pointer in `req.data` or a
 * handle's `.data` field, then pass the `shared_ptr` to @ref UvFuture. The uv
 * callback calls `complete(args...)`, which stores the result and wakes the
 * awaiting coroutine.
 *
 * ### Thread safety
 * `complete()` can be called from any thread. It acquires the internal mutex
 * before writing `value` and reading `waker`, so there is no data race with
 * @ref UvFuture::poll() running on the uv thread.
 *
 * ### Lifetime
 * The `shared_ptr` held by `UvFuture` keeps the object alive until the
 * future is destroyed. Store the raw pointer in `req.data` **before** moving
 * the `shared_ptr` into `UvFuture`, so the pointer is always valid when the
 * callback fires.
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
 * Pair with @ref UvCallbackResult: allocate the result, pass its raw pointer to
 * `req.data`, start the uv operation, then `co_await UvFuture<Args...>(result)`.
 *
 * The coroutine using this future **must run on the uv executor** (via
 * `with_context`) so that the callback fires on the same thread that drains
 * the ready queue. This is not enforced at compile time.
 *
 * **OutputType** is `std::tuple<Args...>`. Use structured bindings to unpack:
 * @code
 * auto [nbytes] = co_await UvFuture<ssize_t>(result);
 * @endcode
 *
 * **Cancellation:** pass a cancel callable as the second constructor argument.
 * It is invoked from the `UvFuture` destructor if the callback has not yet
 * fired. Use this to call `uv_cancel()` or `uv_close()` on the underlying
 * handle so the event loop can drain cleanly.
 *
 * @tparam Args  Argument types passed to `UvCallbackResult::complete()`.
 *               Must match exactly.
 *
 * Example (file read on the uv executor):
 * @code
 * auto result = std::make_shared<UvCallbackResult<ssize_t>>();
 * uv_fs_t req;
 * req.data = result.get();
 * uv_fs_read(loop, &req, fd, &buf, 1, offset, [](uv_fs_t* r) {
 *     static_cast<UvCallbackResult<ssize_t>*>(r->data)->complete(r->result);
 * });
 * auto [nbytes] = co_await UvFuture<ssize_t>(result);
 * @endcode
 */
template<typename... Args>
class UvFuture {
public:
    using OutputType = std::tuple<Args...>;

    /// Construct without a cancel function. The uv operation runs to completion
    /// even if this future is dropped before the callback fires.
    explicit UvFuture(std::shared_ptr<UvCallbackResult<Args...>> result)
        : m_result(std::move(result))
    {}

    /// Construct with a cancel function invoked if the future is destroyed before
    /// the callback fires. Use to call `uv_cancel()` / `uv_close()` on the handle.
    UvFuture(std::shared_ptr<UvCallbackResult<Args...>> result,
             std::function<void()> cancel_fn)
        : m_result(std::move(result))
        , m_cancel_fn(std::move(cancel_fn))
    {}

    UvFuture(UvFuture&&) noexcept            = default;
    UvFuture& operator=(UvFuture&&) noexcept = default;
    UvFuture(const UvFuture&)                = delete;
    UvFuture& operator=(const UvFuture&)     = delete;

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
    std::shared_ptr<UvCallbackResult<Args...>> m_result;
    std::function<void()>                      m_cancel_fn;  // null if no cancellation
};

} // namespace coro
