#pragma once

#include <coro/detail/context.h>
#include <coro/detail/poll_result.h>
#include <optional>
#include <type_traits>
#include <utility>

namespace coro {

/**
 * @brief Future returned by `JoinHandle::cancel_and_join()` and
 * `BlockingHandle::cancel_and_join()`.
 *
 * Owns the handle it was made from. The cancellation has already been requested by the
 * time this future exists; awaiting it waits for the task to finish and reports how it
 * ended:
 *
 * | Task ended                                   | `JoinHandle<T>` / `BlockingHandle<T>` | `T = void`         |
 * |----------------------------------------------|---------------------------------------|--------------------|
 * | cancelled before producing a result          | `std::nullopt`                        | `false`            |
 * | finished before the cancel hit               | the value                             | `true`             |
 * | threw                                        | rethrows                              | rethrows           |
 *
 * Which of the first two happens is a race between the cancel and the task finishing, so
 * a caller must handle both. The value is offered rather than discarded because a task
 * that did finish has done its work, and its result may still be worth using.
 *
 * `T = void` reports the same outcome as a `bool`, in place of an `std::optional<void>`.
 *
 * Cancellable when the handle is (`JoinHandle`): a coroutine cancelled while awaiting this
 * waits for the task to drain, as it would awaiting the handle itself. `BlockingHandle` is
 * not Cancellable (see its comment), and neither is this future around one.
 *
 * @tparam Handle `JoinHandle<T>` or `BlockingHandle<T>`.
 */
template<typename Handle>
class [[nodiscard]] CancelAndJoin {
    using T = typename Handle::OutputType;

public:
    using OutputType = std::conditional_t<std::is_void_v<T>, bool, std::optional<T>>;

    explicit CancelAndJoin(Handle handle) noexcept : m_handle(std::move(handle)) {}

    CancelAndJoin(CancelAndJoin&&) noexcept            = default;
    CancelAndJoin& operator=(CancelAndJoin&&) noexcept = default;

    /// @brief Forwards to the handle's cancel(). Present only when the handle has one.
    void cancel() noexcept requires requires(Handle& h) { h.cancel(); } { m_handle.cancel(); }

    PollResult<OutputType> poll(detail::Context& ctx) {
        auto r = m_handle.poll(ctx);
        if (r.isPending()) return PollPending;
        if (r.isError()) return PollError(r.error());
        // Dropped: the task was cancelled before it produced a result.
        if constexpr (std::is_void_v<T>) {
            return !r.isDropped();
        } else {
            if (r.isDropped()) return std::optional<T>(std::nullopt);
            return std::optional<T>(std::move(r).value());
        }
    }

private:
    Handle m_handle;
};

} // namespace coro
