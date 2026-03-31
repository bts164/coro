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

template<typename T>
struct OneshotShared {
    std::mutex                        mutex;
    std::optional<T>                  slot;           ///< Filled by send().
    bool                              sender_alive   = true;
    bool                              receiver_alive = true;
    std::shared_ptr<coro::detail::Waker> receiver_waker; ///< Set when receiver suspends.
};

} // namespace detail

template<typename T> class OneshotReceiver;

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
     * @brief Sends @p value to the receiver.
     *
     * Synchronous and non-blocking. Returns `std::unexpected(value)` if the
     * receiver has already been dropped, giving the caller ownership of the
     * unsent value back.
     *
     * @param value The value to send.
     * @return `{}` on success; `std::unexpected(std::move(value))` if the receiver is gone.
     */
    std::expected<void, T> send(T value) {
        std::shared_ptr<coro::detail::Waker> waker;
        {
            std::lock_guard lock(m_shared->mutex);
            if (!m_shared->receiver_alive) {
                return std::unexpected(std::move(value));
            }
            m_shared->slot = std::move(value);
            waker = std::move(m_shared->receiver_waker);
            m_shared->sender_alive = false; // consumed
        }
        m_shared = nullptr; // release before waking to avoid lock contention
        if (waker) waker->wake();
        return {};
    }

private:
    std::shared_ptr<detail::OneshotShared<T>> m_shared;
};

/**
 * @brief Async receiver half of a oneshot channel. Satisfies `Future<std::expected<T, ChannelError>>`.
 *
 * Awaiting the receiver suspends until `send()` is called or the sender is dropped.
 * On sender drop without a send, resolves with `std::unexpected(ChannelError::Closed)`.
 *
 * **Thread safety:** each instance must be used by at most one thread at a time.
 *
 * @tparam T The value type to receive.
 */
template<typename T>
class OneshotReceiver {
public:
    using OutputType = std::expected<T, ChannelError>;

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
        m_shared->receiver_waker = nullptr; // clear stale waker
    }

    /**
     * @brief Advances the receiver toward completion.
     *
     * - Returns `Ready(value)` if the value has been sent.
     * - Returns `Ready(unexpected(Closed))` if the sender was dropped without sending.
     * - Returns `Pending` and registers the waker if nothing has happened yet.
     */
    PollResult<OutputType> poll(coro::detail::Context& ctx) {
        std::lock_guard lock(m_shared->mutex);

        if (m_shared->slot.has_value()) {
            return OutputType(std::move(*m_shared->slot));
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
