#pragma once

#include <coro/detail/context.h>
#include <coro/detail/waker.h>
#include <coro/detail/poll_result.h>
#include <coro/sync/channel_error.h>

#include <expected>
#include <memory>
#include <optional>
#include <coro/detail/mutex.h>
#include <utility>

namespace coro {

namespace detail {

/// `std::optional<void>` is ill-formed, so map void → monostate for the slot.
template<typename T>
using OneshotSlotType = std::conditional_t<std::is_void_v<T>, std::monostate, T>;

template<typename T>
struct OneshotShared {
    detail::Mutex                        mutex;
    detail::CondVar                      cv;             ///< Notified on send() and sender drop; used by blocking_recv.
    std::optional<OneshotSlotType<T>>    slot;           ///< Filled by send().
    bool                                 sender_alive   = true;
    bool                                 receiver_alive = true;
    detail::Rc<coro::detail::Waker> receiver_waker; ///< Set when receiver suspends.
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

    explicit OneshotRecvFuture(detail::Rc<detail::OneshotShared<T>> shared)
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
                m_shared->slot.reset();
                return OutputType{};
            } else {
                auto result = OutputType(std::move(*m_shared->slot));
                m_shared->slot.reset();  // prevent double-receive if future is re-polled
                return result;
            }
        }
        if (!m_shared->sender_alive) {
            return OutputType(std::unexpected(ChannelError::Closed));
        }

        m_shared->receiver_waker = ctx.getWaker();
        return PollPending;
    }

private:
    detail::Rc<detail::OneshotShared<T>> m_shared;
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
    explicit OneshotSender(detail::Rc<detail::OneshotShared<T>> shared)
        : m_shared(std::move(shared)) {}

    OneshotSender(const OneshotSender&)            = delete;
    OneshotSender& operator=(const OneshotSender&) = delete;
    OneshotSender(OneshotSender&&)                 = default;

    // Hand-written rather than defaulted: a defaulted move-assignment would
    // just overwrite m_shared, dropping the old Rc without running the
    // sender_alive-clear-and-wake protocol below. std::swap is NOT
    // sufficient — if the right-hand side is a named object moved via
    // std::move rather than a genuine temporary, swapping just stashes the
    // old state inside that named object, deferring the close until it
    // happens to go out of scope. Explicitly close the old channel via the
    // same helper the destructor uses, then take over the new state.
    OneshotSender& operator=(OneshotSender&& other) noexcept {
        if (this != &other) {
            close();
            m_shared = std::move(other.m_shared);
        }
        return *this;
    }

    ~OneshotSender() { close(); }

    /**
     * @brief Returns `true` if the receiver has been dropped.
     *
     * A `true` result guarantees that any subsequent `send()` will fail.
     * A `false` result is a snapshot — the receiver may be dropped concurrently
     * before `send()` is called, so `send()` may still return an error.
     */
    [[nodiscard]] bool is_closed() const {
        std::lock_guard lock(m_shared->mutex);
        return !m_shared->receiver_alive;
    }

    /**
     * @brief Sends @p value to the receiver (non-void T).
     *
     * Synchronous and non-blocking. Returns `std::unexpected(value)` if the
     * receiver has already been dropped, giving the caller ownership of the
     * unsent value back.
     *
     * NOT ISR-SAFE: calls waker->wake() which touches shared_ptr ref-counts and
     * the executor queue. Use IsrEvent::signal_from_isr() from ISR context instead.
     *
     * @param value The value to send.
     * @return `{}` on success; `std::unexpected<U>(std::forward<U>(value))` if the receiver is gone.
     */
    template<typename U> requires (
        !std::is_void_v<T> && std::convertible_to<U, T>
    )
    std::expected<void, std::decay_t<U>> send(U &&value) {
        detail::Rc<coro::detail::Waker> waker;
        {
            std::lock_guard lock(m_shared->mutex);
            if (!m_shared->receiver_alive) {
                return std::unexpected<std::decay_t<U>>(std::forward<U>(value));
            }
            m_shared->slot = std::forward<std::decay_t<U>>(value);
            m_shared->cv.notify_all();
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
     *
     * NOT ISR-SAFE: calls waker->wake() which touches shared_ptr ref-counts and
     * the executor queue. Use IsrEvent::signal_from_isr() from ISR context instead.
     */
    std::expected<void, ChannelError> send() requires std::is_void_v<T> {
        detail::Rc<coro::detail::Waker> waker;
        {
            std::lock_guard lock(m_shared->mutex);
            if (!m_shared->receiver_alive)
                return std::unexpected(ChannelError::Closed);
            m_shared->slot = std::monostate{};
            m_shared->cv.notify_all();
            waker = std::move(m_shared->receiver_waker);
            m_shared->sender_alive = false;
        }
        m_shared = nullptr;
        if (waker) waker->wake();
        return {};
    }

private:
    // Clears sender_alive and wakes any waiting receiver. Shared between the
    // destructor and move-assignment so both close the channel through the
    // same protocol.
    void close() {
        if (!m_shared) return;
        detail::Rc<coro::detail::Waker> waker;
        {
            std::lock_guard lock(m_shared->mutex);
            m_shared->sender_alive = false;
            m_shared->cv.notify_all();
            waker = std::move(m_shared->receiver_waker);
        }
        if (waker) waker->wake();
        m_shared = nullptr;
    }

    detail::Rc<detail::OneshotShared<T>> m_shared;
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
    explicit OneshotReceiver(detail::Rc<detail::OneshotShared<T>> shared)
        : m_shared(std::move(shared)) {}

    OneshotReceiver(const OneshotReceiver&)            = delete;
    OneshotReceiver& operator=(const OneshotReceiver&) = delete;
    OneshotReceiver(OneshotReceiver&&)                 = default;

    // Hand-written rather than defaulted: a defaulted move-assignment would
    // just overwrite m_shared, dropping the old Rc without running the
    // receiver_alive-clear protocol below. std::swap is NOT sufficient — if
    // the right-hand side is a named object moved via std::move rather than
    // a genuine temporary, swapping just stashes the old state inside that
    // named object, deferring the close until it happens to go out of scope.
    // Explicitly close the old channel via the same helper the destructor
    // uses, then take over the new state.
    OneshotReceiver& operator=(OneshotReceiver&& other) noexcept {
        if (this != &other) {
            close();
            m_shared = std::move(other.m_shared);
        }
        return *this;
    }

    ~OneshotReceiver() { close(); }

    /**
     * @brief Returns `true` if the sender has been dropped without sending a value.
     *
     * Returns `false` if the sender is still alive or has already sent a value
     * (in which case `recv()` will resolve immediately with that value).
     * A `true` result means `recv()` will resolve with `ChannelError::Closed`.
     */
    [[nodiscard]] bool sender_dropped() const {
        std::lock_guard lock(m_shared->mutex);
        return !m_shared->sender_alive && !m_shared->slot.has_value();
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

    /**
     * @brief Blocks the calling OS thread until the sender sends a value or is dropped.
     *
     * Intended for use on threads created by `spawn_blocking`. **Do not call from a
     * coroutine or executor thread** — it will block the thread and stall the executor.
     *
     * @return `T` (or `void`) on success; `std::unexpected(ChannelError::Closed)` if the
     *         sender was dropped without sending.
     */
    std::expected<T, ChannelError> blocking_recv() {
        std::unique_lock lock(m_shared->mutex);
        m_shared->cv.wait(lock, [this] {
            return m_shared->slot.has_value() || !m_shared->sender_alive;
        });
        if (m_shared->slot.has_value()) {
            if constexpr (std::is_void_v<T>) {
                m_shared->slot.reset();
                return {};
            } else {
                auto result = std::move(*m_shared->slot);
                m_shared->slot.reset();  // prevent double-receive on second blocking_recv
                return result;
            }
        }
        return std::unexpected(ChannelError::Closed);
    }

private:
    // Clears receiver_alive and the stored waker. Shared between the
    // destructor and move-assignment so both close the channel through the
    // same protocol.
    void close() {
        if (!m_shared) return;
        // Lock scoped to a nested block: resetting m_shared below may drop the
        // last reference to OneshotShared, destroying its mutex. That must
        // happen after the lock_guard has already unlocked and gone out of
        // scope — unlocking inline would otherwise touch freed memory.
        {
            std::lock_guard lock(m_shared->mutex);
            m_shared->receiver_alive = false;
            m_shared->receiver_waker = nullptr;
        }
        m_shared = nullptr;
    }

    detail::Rc<detail::OneshotShared<T>> m_shared;
};

/**
 * @brief Creates a linked (sender, receiver) pair.
 *
 * @tparam T The value type to transport.
 * @return A pair `{OneshotSender<T>, OneshotReceiver<T>}`.
 */
template<typename T>
[[nodiscard]] std::pair<OneshotSender<T>, OneshotReceiver<T>> oneshot_channel() {
    auto shared = detail::make_rc<detail::OneshotShared<T>>();
    return { OneshotSender<T>(shared), OneshotReceiver<T>(shared) };
}

} // namespace coro
