#pragma once

#include <coro/future.h>
#include <coro/stream.h>
#include <coro/detail/poll_result.h>
#include <coro/detail/context.h>
#include <memory>
#include <optional>
#include <type_traits>

namespace coro {

/**
 * @brief Safe wrapper for a Future-returning capturing lambda.
 *
 * Stores the lambda on the heap so its address is stable across moves. The coroutine
 * frame created by `operator()` captures `this` (a pointer into the lambda object);
 * heap allocation ensures that pointer remains valid for the wrapper's entire lifetime.
 *
 * Do not construct directly — use the @ref co_invoke factory function.
 *
 * @tparam Lambda A callable with no arguments whose `operator()` returns a type
 *                satisfying @ref Future.
 */
template<typename Lambda>
class CoInvokeFuture {
    using CoroType = std::invoke_result_t<Lambda&>;
public:
    using OutputType = typename CoroType::OutputType;

    explicit CoInvokeFuture(Lambda lambda) {
        // m_lambda must be heap-allocated before m_coro is created.
        // The coroutine frame produced by (*m_lambda)() captures &(*m_lambda),
        // which must remain stable across any subsequent move of this wrapper.
        m_lambda = std::make_unique<Lambda>(std::move(lambda));
        m_coro   = std::make_unique<CoroType>((*m_lambda)());
    }

    CoInvokeFuture(CoInvokeFuture&&) noexcept            = default;
    CoInvokeFuture& operator=(CoInvokeFuture&&) noexcept = default;
    CoInvokeFuture(const CoInvokeFuture&)                = delete;
    CoInvokeFuture& operator=(const CoInvokeFuture&)     = delete;

    /// @brief Advances the inner coroutine. Delegates directly to `Coro<T>::poll()`.
    PollResult<OutputType> poll(detail::Context& ctx) {
        auto result = m_coro->poll(ctx);
        if (!result.isPending()) {
            // Coroutine is done (ready, error, or dropped). Release captures immediately
            // so that values captured by the lambda are destroyed as soon as the coroutine
            // completes, not when this wrapper is eventually destroyed. Without this reset,
            // captured values (e.g. shared_ptr, RAII guards) can outlive the coroutine
            // scope — violating the invariant that child tasks must fully clean up before
            // the parent scope can observe their completion.
            // m_coro is destroyed first because the coroutine frame may reference m_lambda.
            m_coro.reset();
            m_lambda.reset();
        }
        return result;
    }

    /// @brief Propagates cancellation to the inner coroutine.
    void cancel() noexcept { m_coro->cancel(); }

private:
    std::unique_ptr<Lambda>   m_lambda; ///< Heap-allocated for address stability.
    std::unique_ptr<CoroType> m_coro;   ///< Declared after m_lambda — destroyed first.
};


/**
 * @brief Safe wrapper for a Stream-returning capturing lambda.
 *
 * Same ownership model as @ref CoInvokeFuture but satisfies @ref Stream<T> rather
 * than @ref Future<T>.
 *
 * Do not construct directly — use the @ref co_invoke factory function.
 *
 * @tparam Lambda A callable with no arguments whose `operator()` returns a type
 *                satisfying @ref Stream.
 */
template<typename Lambda>
class CoInvokeStream {
    using StreamType = std::invoke_result_t<Lambda&>;
public:
    using ItemType = typename StreamType::ItemType;

    explicit CoInvokeStream(Lambda lambda) {
        m_lambda = std::make_unique<Lambda>(std::move(lambda));
        m_stream = std::make_unique<StreamType>((*m_lambda)());
    }

    CoInvokeStream(CoInvokeStream&&) noexcept            = default;
    CoInvokeStream& operator=(CoInvokeStream&&) noexcept = default;
    CoInvokeStream(const CoInvokeStream&)                = delete;
    CoInvokeStream& operator=(const CoInvokeStream&)     = delete;

    /// @brief Advances the inner stream. Delegates directly to `CoroStream<T>::poll_next()`.
    PollResult<std::optional<ItemType>> poll_next(detail::Context& ctx) {
        auto result = m_stream->poll_next(ctx);
        if (!result.isPending()) {
            // Stream is done when exhausted (Ready(nullopt)), errored, or dropped.
            // If it yielded an item (Ready(some)), more polls may follow — keep alive.
            const bool has_item = result.isReady() && result.value().has_value();
            if (!has_item) {
                // Same reasoning as CoInvokeFuture: release captures immediately on
                // completion so that captured values don't outlive the stream scope.
                m_stream.reset();
                m_lambda.reset();
            }
        }
        return result;
    }

    /// @brief Propagates cancellation to the inner stream.
    void cancel() noexcept { m_stream->cancel(); }

private:
    std::unique_ptr<Lambda>     m_lambda;
    std::unique_ptr<StreamType> m_stream;
};


/**
 * @brief Safely invokes a capturing lambda that returns a `Coro<T>` or `CoroStream<T>`.
 *
 * ### The problem
 *
 * Any capturing lambda that returns `Coro<T>` has a latent use-after-free when used as
 * an rvalue. The compiler lowers the lambda to an anonymous struct; `operator()` is a
 * member function that implicitly captures `this`. The struct is a temporary and is
 * destroyed at the end of the full expression — before the coroutine is ever polled.
 *
 * ```cpp
 * // DANGEROUS — lambda destroyed at ';', before first resumption
 * auto coro = [x]() -> Coro<void> {
 *     co_await something();
 *     use(x);          // this->x — 'this' is dangling
 * }();
 * co_await coro;       // use-after-free
 * ```
 *
 * ### The fix
 *
 * `co_invoke` moves the lambda onto the heap inside a @ref CoInvokeFuture (or
 * @ref CoInvokeStream) wrapper. The wrapper owns both the lambda and the coroutine it
 * produced, ensuring the lambda is alive for the coroutine's entire lifetime.
 *
 * ```cpp
 * // SAFE — lambda kept alive by co_invoke wrapper
 * co_await co_invoke([x]() -> Coro<void> {
 *     co_await something();
 *     use(x);    // safe
 * });
 *
 * // Also composes with spawn:
 * auto handle = spawn(co_invoke([x]() -> Coro<void> { ... }));
 * ```
 *
 * @param lambda A callable with no arguments returning a @ref Future or @ref Stream.
 * @return A @ref CoInvokeFuture or @ref CoInvokeStream wrapping the lambda and its coroutine.
 */
template<typename Lambda>
    requires Future<std::invoke_result_t<Lambda&>>
[[nodiscard]] CoInvokeFuture<Lambda> co_invoke(Lambda lambda) {
    return CoInvokeFuture<Lambda>(std::move(lambda));
}

template<typename Lambda>
    requires Stream<std::invoke_result_t<Lambda&>>
[[nodiscard]] CoInvokeStream<Lambda> co_invoke(Lambda lambda) {
    return CoInvokeStream<Lambda>(std::move(lambda));
}

} // namespace coro
