#pragma once

#include <coro/detail/context.h>
#include <coro/detail/waker.h>
#include <coro/detail/poll_result.h>
#include <coro/sync/channel_error.h>

#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

namespace coro::oneshot {

namespace detail {

/// `std::optional<void>` is ill-formed, so map void → monostate for the slot.
template<typename T>
using SlotType = std::conditional_t<std::is_void_v<T>, std::monostate, T>;

template<typename T>
struct OneshotShared {
    std::mutex                           mutex;
    std::optional<SlotType<T>>           slot;           ///< Filled by send().
    bool                                 sender_alive   = true;
    bool                                 receiver_alive = true;
    std::shared_ptr<coro::detail::Waker> receiver_waker; ///< Set when receiver suspends.
};

} // namespace detail

/**
 * @brief Future returned by `OneshotReceiver<T>::recv()`.
 *
 * Satisfies `Future<std::expected<T, ChannelError>>`. Holds a reference to the
 * channel's shared state but does **not** own the receiver; the originating
 * `OneshotReceiver` must remain alive (or at least not destroyed) while this
 * future is live. Dropping this future while suspended clears the registered
 * waker so no spurious wake reaches a dead coroutine.
 *
 * @tparam T The value type to receive.
 */
template<typename T>
class OneshotRecvFuture {
public:
    using OutputType = std::expected<T, ChannelError>;

    explicit OneshotRecvFuture(std::shared_ptr<detail::OneshotShared<T>> shared)
        : m_shared(std::move(shared)) {}

    OneshotRecvFuture(const OneshotRecvFuture&)            = delete;
    OneshotRecvFuture& operator=(const OneshotRecvFuture&) = delete;
    OneshotRecvFuture(OneshotRecvFuture&&)                 = default;
    OneshotRecvFuture& operator=(OneshotRecvFuture&&)      = default;

    ~OneshotRecvFuture() {
        if (!m_shared) return;
        // Clear any waker we may have registered so a late sender wake doesn't
        // reach a dead coroutine frame.
        std::lock_guard lock(m_shared->mutex);
        m_shared->receiver_waker = nullptr;
    }

    /**
     * @brief Advances the receive operation.
     *
     * - Returns `Ready(value)` if the value has been sent.
     * - Returns `Ready(unexpected(Closed))` if the sender was dropped without sending.
     * - Returns `Pending` and registers the waker if nothing has happened yet.
     */
    PollResult<OutputType> poll(coro::detail::Context& ctx) {
        std::lock_guard lock(m_shared->mutex);

        if (m_shared->slot.has_value()) {
            if constexpr (std::is_void_v<T>) {
                return OutputType{};
            } else {
                return OutputType(std::move(*m_shared->slot));
            }
        }
        if (!m_shared->sender_alive) {
            return OutputType(std::unexpected(ChannelError::Closed));
        }

        m_shared->receiver_waker = ctx.getWaker();
        return PollPending;
    }

private:
    std::shared_ptr<detail::OneshotShared<T>> m_shared;
};

/**
 * @brief Synchronous sender half of a oneshot channel.
 *
 * `send()` may be called from any thread, including non-async contexts.
 * The sender is move-only; calling `send()` consumes it.
 *
 * **Thread safety:** each instance must be used by at most one thread at a time.
 *
 * @tparam T The value type to send.
 */
template<typename T>
class OneshotSender {
public:
    explicit OneshotSender(std::shared_ptr<detail::OneshotShared<T>> shared)
        : m_shared(std::move(shared)) {}

    OneshotSender(const OneshotSender&)            = delete;
    OneshotSender& operator=(const OneshotSender&) = delete;
    OneshotSender(OneshotSender&&)                 = default;
    OneshotSender& operator=(OneshotSender&&)      = default;

    ~OneshotSender() {
        if (!m_shared) return;
        // Dropping without send() closes the channel; wake any waiting receiver.
        std::shared_ptr<coro::detail::Waker> waker;
        {
            std::lock_guard lock(m_shared->mutex);
            m_shared->sender_alive = false;
            waker = std::move(m_shared->receiver_waker);
        }
        if (waker) waker->wake();
    }

    /**
     * @brief Sends @p value to the receiver (non-void T).
     *
     * Synchronous and non-blocking. Returns `std::unexpected(value)` if the
     * receiver has already been dropped, giving the caller ownership of the
     * unsent value back.
     *
     * @param value The value to send.
     * @return `{}` on success; `std::unexpected<U>(std::forward<U>(value))` if the receiver is gone.
     * @note This is a template primarily because this overload needs to be removed using SFINAE when
     * `T` is `void`. Allowing perfect forwarding and avoiding unnecessary moves is just an added bonus.
     */
    template<typename U> requires (
        !std::is_void_v<T> && std::convertible_to<U, T>
    )
    std::expected<void, std::decay_t<U>> send(U &&value) {
        std::shared_ptr<coro::detail::Waker> waker;
        {
            std::lock_guard lock(m_shared->mutex);
            if (!m_shared->receiver_alive) {
                return std::unexpected<std::decay_t<U>>(std::forward<U>(value));
            }
            m_shared->slot = std::forward<std::decay_t<U>>(value);
            waker = std::move(m_shared->receiver_waker);
            m_shared->sender_alive = false; // consumed
        }
        m_shared = nullptr; // release before waking to avoid lock contention
        if (waker) waker->wake();
        return {};
    }

    /**
     * @brief Signals the receiver (void T). Takes no arguments.
     *
     * Returns `{}` on success; `std::unexpected(ChannelError::Closed)` if the
     * receiver has already been dropped.
     */
    std::expected<void, ChannelError> send() requires std::is_void_v<T> {
        std::shared_ptr<coro::detail::Waker> waker;
        {
            std::lock_guard lock(m_shared->mutex);
            if (!m_shared->receiver_alive)
                return std::unexpected(ChannelError::Closed);
            m_shared->slot = std::monostate{};
            waker = std::move(m_shared->receiver_waker);
            m_shared->sender_alive = false;
        }
        m_shared = nullptr;
        if (waker) waker->wake();
        return {};
    }

private:
    std::shared_ptr<detail::OneshotShared<T>> m_shared;
};

/**
 * @brief Receiver half of a oneshot channel.
 *
 * Call `recv()` to obtain an `OneshotRecvFuture<T>` that can be `co_await`-ed
 * or passed to `select()`. The receiver is move-only; creating a new future
 * each time via `recv()` keeps the receiver alive so it can be reused after a
 * cancelled `select()` branch.
 *
 * **Thread safety:** each instance must be used by at most one thread at a time.
 *
 * @tparam T The value type to receive. Use `void` for pure signalling with no value.
 */
template<typename T>
class OneshotReceiver {
public:
    explicit OneshotReceiver(std::shared_ptr<detail::OneshotShared<T>> shared)
        : m_shared(std::move(shared)) {}

    OneshotReceiver(const OneshotReceiver&)            = delete;
    OneshotReceiver& operator=(const OneshotReceiver&) = delete;
    OneshotReceiver(OneshotReceiver&&)                 = default;
    OneshotReceiver& operator=(OneshotReceiver&&)      = default;

    ~OneshotReceiver() {
        if (!m_shared) return;
        std::lock_guard lock(m_shared->mutex);
        m_shared->receiver_alive = false;
        m_shared->receiver_waker = nullptr;
    }

    /**
     * @brief Returns a future that resolves when the sender sends a value or is dropped.
     *
     * The returned `OneshotRecvFuture<T>` can be `co_await`-ed directly or passed
     * to `select()`. The receiver remains valid after the future completes or is
     * cancelled, though a oneshot channel can only be received once.
     */
    [[nodiscard]] OneshotRecvFuture<T> recv() {
        return OneshotRecvFuture<T>(m_shared);
    }

private:
    std::shared_ptr<detail::OneshotShared<T>> m_shared;
};

/**
 * @brief Creates a linked (sender, receiver) pair.
 *
 * @tparam T The value type to transport.
 * @return A pair `{OneshotSender<T>, OneshotReceiver<T>}`.
 */
template<typename T>
[[nodiscard]] std::pair<OneshotSender<T>, OneshotReceiver<T>> channel() {
    auto shared = std::make_shared<detail::OneshotShared<T>>();
    return { OneshotSender<T>(shared), OneshotReceiver<T>(shared) };
}

} // namespace coro::oneshot
