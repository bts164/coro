#pragma once

#include <coro/detail/context.h>
#include <coro/detail/intrusive_list.h>
#include <coro/detail/poll_result.h>
#include <coro/detail/waker.h>
#include <coro/sync/channel_error.h>

#include <cassert>
#include <deque>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace coro::mpsc {

namespace detail {

/**
 * @brief Intrusive node for a suspended sender. Lives inside `SendFuture<T>`.
 *
 * The node is embedded in the sender's coroutine frame. When the sender suspends
 * because the buffer is full, the node is linked into `MpscShared::sender_waiters`.
 * The node holds the unsent value until the receiver transfers it.
 *
 * `std::optional<T>` is used for the value so that `T` does not need to be
 * default-constructible.
 */
template<typename T>
struct MpscSenderNode : coro::detail::IntrusiveListNode {
    std::optional<T>                     value;
    std::shared_ptr<coro::detail::Waker> waker;
};

/**
 * @brief Node for a suspended receiver. Lives inside `Receiver<T>` as a member.
 *
 * Set in `MpscShared::receiver_waiter` when the receiver suspends waiting for
 * a value. A sender that finds this set deposits its value into the buffer
 * (guaranteed to have space since the receiver only suspends when the buffer is
 * empty) and wakes the receiver.
 */
struct MpscReceiverNode {
    std::shared_ptr<coro::detail::Waker> waker;
};

/**
 * @brief Shared state for an mpsc channel.
 *
 * Protected by `mutex`. Allocated once at `channel()` construction and held
 * via `shared_ptr` by all Sender and Receiver handles.
 *
 * @note The ring buffer is currently implemented as a `std::deque` with a
 *       capacity cap. A fixed-size ring buffer can replace this later for
 *       better cache behaviour without changing the external API.
 */
template<typename T>
struct MpscShared {
    std::mutex                     mutex;
    std::deque<T>                  buffer;          ///< Bounded queue.
    size_t                         capacity;        ///< Maximum buffered values.
    size_t                         sender_count  = 0;
    bool                           receiver_alive = true;
    coro::detail::IntrusiveList    sender_waiters; ///< Suspended senders (MpscSenderNode<T>*).
    std::optional<MpscReceiverNode> receiver_waiter; ///< At most one (single consumer).

    explicit MpscShared(size_t cap) : capacity(cap) {}
};

} // namespace detail

template<typename T> class Sender;
template<typename T> class Receiver;

/**
 * @brief Future returned by `Sender<T>::send()`.
 *
 * Satisfies `Future<std::expected<void, T>>`. Resolves immediately if buffer
 * space is available; suspends if the buffer is full. If the receiver is dropped
 * while this future is suspended, it resolves with the unsent value in the error slot.
 *
 * **Cancellation:** if dropped while suspended, the destructor acquires the channel
 * mutex and unlinks the intrusive node before the coroutine frame is freed.
 *
 * **Move safety:** moving a `SendFuture` is only valid before `poll()` has been
 * called (i.e. before the node is linked into the channel's waiter list).
 *
 * @tparam T The value type being sent.
 */
template<typename T>
class SendFuture {
public:
    using OutputType = std::expected<void, T>;

    SendFuture(std::shared_ptr<detail::MpscShared<T>> shared, T value)
        : m_shared(std::move(shared))
    {
        m_node.value.emplace(std::move(value));
    }

    SendFuture(const SendFuture&)            = delete;
    SendFuture& operator=(const SendFuture&) = delete;

    // Only valid before poll() — the node must not be linked when moved.
    SendFuture(SendFuture&& other) noexcept
        : m_shared(std::move(other.m_shared))
        , m_node(std::move(other.m_node))
    {
        assert(!m_node.is_linked() && "SendFuture moved while linked — undefined behaviour");
    }

    SendFuture& operator=(SendFuture&&) = delete;

    ~SendFuture() {
        if (!m_shared) return;
        if (m_node.is_linked()) {
            std::lock_guard lock(m_shared->mutex);
            if (m_node.is_linked())
                m_shared->sender_waiters.remove(&m_node);
        }
    }

    /**
     * @brief Advances the send operation.
     *
     * 1. Receiver dropped → return unsent value as error.
     * 2. Receiver waiting → move value into buffer (guaranteed empty), wake receiver.
     * 3. Buffer has space → move value into buffer.
     * 4. Buffer full → link node into sender_waiters, return Pending.
     */
    PollResult<OutputType> poll(coro::detail::Context& ctx) {
        std::unique_lock lock(m_shared->mutex);

        // Woken because receiver was dropped while we were suspended.
        if (!m_shared->receiver_alive) {
            if (m_node.is_linked())
                m_shared->sender_waiters.remove(&m_node);
            return OutputType(std::unexpected(std::move(*m_node.value)));
        }

        // Receiver is waiting — buffer is empty, so there is guaranteed space.
        if (m_shared->receiver_waiter.has_value()) {
            m_shared->buffer.push_back(std::move(*m_node.value));
            auto waker = std::move(m_shared->receiver_waiter->waker);
            m_shared->receiver_waiter.reset();
            if (m_node.is_linked())
                m_shared->sender_waiters.remove(&m_node);
            lock.unlock();
            waker->wake();
            return OutputType{};
        }

        // Buffer has space.
        if (m_shared->buffer.size() < m_shared->capacity) {
            m_shared->buffer.push_back(std::move(*m_node.value));
            return OutputType{};
        }

        // Buffer full — suspend.
        m_node.waker = ctx.getWaker();
        if (!m_node.is_linked())
            m_shared->sender_waiters.push_back(&m_node);
        return PollPending;
    }

private:
    std::shared_ptr<detail::MpscShared<T>> m_shared;
    detail::MpscSenderNode<T>              m_node;
};

/**
 * @brief Producer handle for an mpsc channel. Cloneable; each clone is independent.
 *
 * **Thread safety:** each `Sender` instance must be used by at most one thread at a
 * time. For multi-threaded producers, give each thread its own clone.
 *
 * @tparam T The value type to send.
 */
template<typename T>
class Sender {
public:
    explicit Sender(std::shared_ptr<detail::MpscShared<T>> shared)
        : m_shared(std::move(shared))
    {
        std::lock_guard lock(m_shared->mutex);
        ++m_shared->sender_count;
    }

    Sender(const Sender&)            = delete;
    Sender& operator=(const Sender&) = delete;
    Sender(Sender&&)                 = default;
    Sender& operator=(Sender&&)      = default;

    ~Sender() {
        if (!m_shared) return;
        std::shared_ptr<coro::detail::Waker> waker;
        {
            std::lock_guard lock(m_shared->mutex);
            if (--m_shared->sender_count == 0) {
                // Last sender dropped — wake receiver so it can observe the close.
                if (m_shared->receiver_waiter.has_value())
                    waker = std::move(m_shared->receiver_waiter->waker);
            }
        }
        if (waker) waker->wake();
    }

    /**
     * @brief Sends @p value asynchronously.
     *
     * Returns a `SendFuture` that resolves immediately if buffer space is
     * available, or suspends until space opens up. If the receiver is dropped
     * while suspended, resolves with `std::unexpected(value)`.
     */
    [[nodiscard]] SendFuture<T> send(T value) {
        return SendFuture<T>(m_shared, std::move(value));
    }

    /**
     * @brief Attempts to send @p value without suspending.
     *
     * Returns `{}` on success. On failure returns `TrySendError<T>` carrying
     * the unsent value and the reason (`Full` or `Disconnected`).
     */
    std::expected<void, TrySendError<T>> trySend(T value) {
        std::unique_lock lock(m_shared->mutex);
        if (!m_shared->receiver_alive)
            return std::unexpected(TrySendError<T>(
                TrySendError<T>::Kind::Disconnected, std::move(value)));
        if (m_shared->buffer.size() >= m_shared->capacity)
            return std::unexpected(TrySendError<T>(
                TrySendError<T>::Kind::Full, std::move(value)));
        m_shared->buffer.push_back(std::move(value));
        // Wake a waiting receiver if present.
        if (m_shared->receiver_waiter.has_value()) {
            auto waker = std::move(m_shared->receiver_waiter->waker);
            m_shared->receiver_waiter.reset();
            lock.unlock();
            waker->wake();
        }
        return {};
    }

    /**
     * @brief Creates an independent sender clone that shares the same channel.
     *
     * Each clone increments the sender reference count. The channel closes from
     * the sender side only when the last clone is destroyed.
     */
    [[nodiscard]] Sender<T> clone() const {
        return Sender<T>(m_shared);
    }

private:
    std::shared_ptr<detail::MpscShared<T>> m_shared;
};

/**
 * @brief Consumer handle for an mpsc channel. Satisfies `Stream<T>`.
 *
 * Yields values in send order. Returns `nullopt` (stream exhausted) when all
 * senders have been dropped and the buffer is empty.
 *
 * **Thread safety:** each instance must be used by at most one thread at a time.
 *
 * @tparam T The value type to receive.
 */
template<typename T>
class Receiver {
public:
    using ItemType = T;

    explicit Receiver(std::shared_ptr<detail::MpscShared<T>> shared)
        : m_shared(std::move(shared)) {}

    Receiver(const Receiver&)            = delete;
    Receiver& operator=(const Receiver&) = delete;
    Receiver(Receiver&&)                 = default;
    Receiver& operator=(Receiver&&)      = default;

    ~Receiver() {
        if (!m_shared) return;
        // Wake all suspended senders so they observe ReceiverDropped.
        std::vector<std::shared_ptr<coro::detail::Waker>> wakers;
        {
            std::lock_guard lock(m_shared->mutex);
            m_shared->receiver_alive = false;
            while (auto* raw = m_shared->sender_waiters.pop_front()) {
                auto* node = static_cast<detail::MpscSenderNode<T>*>(raw);
                if (node->waker) wakers.push_back(std::move(node->waker));
            }
        }
        for (auto& w : wakers) w->wake();
    }

    /**
     * @brief Attempts to receive a value without suspending.
     *
     * Returns:
     * - `T` on success.
     * - `ChannelError::Empty` — channel open but nothing buffered yet.
     * - `ChannelError::SenderDropped` — all senders gone and buffer drained.
     */
    [[nodiscard]] std::expected<T, ChannelError> tryRecv() {
        std::unique_lock lock(m_shared->mutex);
        if (!m_shared->buffer.empty()) {
            T val = std::move(m_shared->buffer.front());
            m_shared->buffer.pop_front();
            auto waker = _tryPromoteSender();
            lock.unlock();
            if (waker) waker->wake();
            return val;
        }
        if (m_shared->sender_count == 0)
            return std::unexpected(ChannelError::SenderDropped);
        return std::unexpected(ChannelError::Empty);
    }

    /**
     * @brief Advances the stream by one item.
     *
     * Returns:
     * - `Ready(some(T))` — item available.
     * - `Ready(nullopt)`  — all senders dropped and buffer drained.
     * - `Pending`         — nothing available yet; waker registered.
     */
    PollResult<std::optional<T>> poll_next(coro::detail::Context& ctx) {
        std::unique_lock lock(m_shared->mutex);

        // Value in buffer — also promote a waiting sender into the freed slot.
        if (!m_shared->buffer.empty()) {
            T val = std::move(m_shared->buffer.front());
            m_shared->buffer.pop_front();
            auto waker = _tryPromoteSender();
            lock.unlock();
            if (waker) waker->wake();
            return std::optional<T>(std::move(val));
        }

        // No buffer but a sender is already waiting — take its value directly.
        if (auto* raw = m_shared->sender_waiters.pop_front()) {
            auto* node = static_cast<detail::MpscSenderNode<T>*>(raw);
            T val = std::move(*node->value);
            auto waker = std::move(node->waker);
            lock.unlock();
            if (waker) waker->wake();
            return std::optional<T>(std::move(val));
        }

        // All senders dropped and buffer empty → stream exhausted.
        if (m_shared->sender_count == 0)
            return std::optional<T>(std::nullopt);

        // Nothing available — register waker and suspend.
        m_recv_node.waker = ctx.getWaker();
        m_shared->receiver_waiter = m_recv_node;
        return PollPending;
    }

private:
    /// Moves the head waiting sender's value into the buffer and returns its waker.
    /// The waker must be called after the mutex is released.
    /// Assumes the mutex is held.
    std::shared_ptr<coro::detail::Waker> _tryPromoteSender() {
        if (auto* raw = m_shared->sender_waiters.pop_front()) {
            auto* node = static_cast<detail::MpscSenderNode<T>*>(raw);
            m_shared->buffer.push_back(std::move(*node->value));
            return std::move(node->waker);
        }
        return nullptr;
    }

    std::shared_ptr<detail::MpscShared<T>> m_shared;
    detail::MpscReceiverNode               m_recv_node; ///< Reused across poll_next calls.
};

/**
 * @brief Creates a linked (sender, receiver) pair with the given buffer capacity.
 *
 * @tparam T       The value type to transport.
 * @param capacity Maximum number of values buffered before senders suspend.
 * @return A pair `{Sender<T>, Receiver<T>}`.
 */
template<typename T>
[[nodiscard]] std::pair<Sender<T>, Receiver<T>> channel(size_t capacity) {
    auto shared = std::make_shared<detail::MpscShared<T>>(capacity);
    return { Sender<T>(shared), Receiver<T>(shared) };
}

} // namespace coro::mpsc
