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
#include <optional>
#include <coro/detail/rc.h>
#include <coro/detail/mutex.h>
#include <utility>
#include <vector>

namespace coro {

namespace detail {

/**
 * @brief Intrusive node for a suspended sender. Lives inside `MpscSendFuture<T>`.
 *
 * The node is embedded in the sender's coroutine frame. When the sender suspends
 * because the buffer is full, the node is linked into `MpscShared::sender_waiters`.
 * The node holds the unsent value until the receiver transfers it.
 */
template<typename T>
struct MpscSenderNode : coro::detail::IntrusiveListNode {
    std::optional<T>                     value;
    detail::Rc<coro::detail::Waker> waker;
};

/**
 * @brief Node for a suspended receiver. Lives inside `MpscReceiver<T>` as a member.
 *
 * Set in `MpscShared::receiver_waiter` when the receiver suspends waiting for
 * a value. A sender that finds this set deposits its value into the buffer
 * (guaranteed to have space since the receiver only suspends when the buffer is
 * empty) and wakes the receiver.
 */
struct MpscReceiverNode {
    detail::Rc<coro::detail::Waker> waker;
};

/**
 * @brief Shared state for an mpsc channel.
 *
 * Protected by `mutex`. Allocated once at `mpsc_channel()` construction and held
 * via `shared_ptr` by all MpscSender and MpscReceiver handles.
 */
template<typename T>
struct MpscShared {
    detail::Mutex                   mutex;
    detail::CondVar                 cv;              ///< Notified on every push, pop, and close; used by blocking_recv / blocking_send.
    std::deque<T>                   buffer;          ///< Bounded queue.
    size_t                          capacity;        ///< Maximum buffered values.
    size_t                          sender_count  = 0;
    bool                            receiver_alive = true;
    coro::detail::IntrusiveList<>   sender_waiters; ///< Suspended senders (MpscSenderNode<T>*).
    std::optional<MpscReceiverNode> receiver_waiter; ///< At most one (single consumer).

    explicit MpscShared(size_t cap) : capacity(cap) {}
};

} // namespace detail

template<typename T> class MpscSender;
template<typename T> class MpscReceiver;

/**
 * @brief Future returned by `MpscSender<T>::send()`.
 *
 * Satisfies `Future<std::expected<void, T>>`. Resolves immediately if buffer
 * space is available; suspends if the buffer is full. If the receiver is dropped
 * while this future is suspended, it resolves with the unsent value in the error slot.
 *
 * **Cancellation:** if dropped while suspended, the destructor acquires the channel
 * mutex and unlinks the intrusive node before the coroutine frame is freed.
 *
 * **Move safety:** moving an `MpscSendFuture` is only valid before `poll()` has been
 * called (i.e. before the node is linked into the channel's waiter list).
 *
 * @tparam T The value type being sent.
 */
template<typename T>
class MpscSendFuture {
public:
    using OutputType = std::expected<void, T>;

    MpscSendFuture(detail::Rc<detail::MpscShared<T>> shared, T value)
        : m_shared(std::move(shared))
    {
        m_node.value.emplace(std::move(value));
    }

    MpscSendFuture(const MpscSendFuture&)            = delete;
    MpscSendFuture& operator=(const MpscSendFuture&) = delete;

    // Only valid before poll() — the node must not be linked when moved.
    MpscSendFuture(MpscSendFuture&& other) noexcept
        : m_shared(std::move(other.m_shared))
        , m_node(std::move(other.m_node))
    {
        assert(!m_node.is_linked() && "MpscSendFuture moved while linked — undefined behaviour");
    }

    MpscSendFuture& operator=(MpscSendFuture&&) = delete;

    ~MpscSendFuture() {
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
        // The zero-copy receive path may have moved our value out of m_node and
        // then woken us. If so, the send already completed — return Ready without
        // touching the buffer.
        if (!m_node.value.has_value())
            return OutputType{};

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
            m_shared->cv.notify_all();
            lock.unlock();
            waker->wake();
            return OutputType{};
        }

        // Buffer has space.
        if (m_shared->buffer.size() < m_shared->capacity) {
            m_shared->buffer.push_back(std::move(*m_node.value));
            m_shared->cv.notify_all();
            return OutputType{};
        }

        // Buffer full — suspend.
        m_node.waker = ctx.getWaker();
        if (!m_node.is_linked())
            m_shared->sender_waiters.push_back(&m_node);
        return PollPending;
    }

private:
    detail::Rc<detail::MpscShared<T>> m_shared;
    detail::MpscSenderNode<T>              m_node;
};

/**
 * @brief Future returned by `MpscReceiver<T>::recv()`.
 *
 * Satisfies `Future<std::optional<T>>`. Resolves with the next value from the
 * channel, or `nullopt` when all senders have been dropped and the buffer is
 * empty. Holding a shared_ptr to the channel state keeps it alive without
 * consuming the `MpscReceiver`, so the receiver can produce further futures after
 * a `select()` branch is cancelled.
 *
 * **Zero-copy fast path:** if a sender is already suspended waiting to send when
 * `poll()` runs, the value is moved directly from the sender's intrusive node into
 * the receiver without touching the ring buffer. The sender's waker is then invoked
 * to resume it.
 *
 * **Cancellation:** the receiver waiter is stored as a plain `MpscReceiverNode`
 * (waker + optional destination) in the shared state, not as an intrusive list node.
 * Since at most one receiver can be waiting at a time, the destructor simply clears
 * this slot under the channel mutex — no list unlinking required.
 *
 * @tparam T The value type to receive.
 */
template<typename T>
class MpscRecvFuture {
public:
    using OutputType = std::optional<T>;

    explicit MpscRecvFuture(detail::Rc<detail::MpscShared<T>> shared)
        : m_shared(std::move(shared)) {}

    MpscRecvFuture(const MpscRecvFuture&)            = delete;
    MpscRecvFuture& operator=(const MpscRecvFuture&) = delete;
    MpscRecvFuture(MpscRecvFuture&&)                 = default;
    MpscRecvFuture& operator=(MpscRecvFuture&&)      = default;

    ~MpscRecvFuture() {
        if (!m_shared) return;
        std::lock_guard lock(m_shared->mutex);
        m_shared->receiver_waiter.reset();
    }

    /**
     * @brief Advances the receive operation.
     *
     * - Returns `Ready(some(T))` — item available.
     * - Returns `Ready(nullopt)`  — all senders dropped and buffer drained.
     * - Returns `Pending`         — nothing available yet; waker registered.
     */
    PollResult<OutputType> poll(coro::detail::Context& ctx) {
        std::unique_lock lock(m_shared->mutex);

        if (!m_shared->buffer.empty()) {
            T val = std::move(m_shared->buffer.front());
            m_shared->buffer.pop_front();
            auto waker = _tryPromoteSender();
            m_shared->cv.notify_all();
            lock.unlock();
            if (waker) waker->wake();
            return std::optional<T>(std::move(val));
        }

        if (auto* raw = m_shared->sender_waiters.pop_front()) {
            auto* node = static_cast<detail::MpscSenderNode<T>*>(raw);
            T val = std::move(*node->value);
            node->value.reset();  // mark consumed so MpscSendFuture::poll() returns Ready on re-poll
            auto waker = std::move(node->waker);
            m_shared->cv.notify_all();
            lock.unlock();
            if (waker) waker->wake();
            return std::optional<T>(std::move(val));
        }

        if (m_shared->sender_count == 0)
            return std::optional<T>(std::nullopt);

        m_shared->receiver_waiter = detail::MpscReceiverNode{ctx.getWaker()};
        return PollPending;
    }

private:
    detail::Rc<detail::MpscShared<T>> m_shared;

    detail::Rc<coro::detail::Waker> _tryPromoteSender() {
        if (auto* raw = m_shared->sender_waiters.pop_front()) {
            auto* node = static_cast<detail::MpscSenderNode<T>*>(raw);
            m_shared->buffer.push_back(std::move(*node->value));
            node->value.reset();  // mark consumed so MpscSendFuture::poll() returns Ready on re-poll
            return std::move(node->waker);
        }
        return nullptr;
    }
};

/**
 * @brief Producer handle for an mpsc channel. Cloneable; each clone is independent.
 *
 * **Thread safety:** each `MpscSender` instance must be used by at most one thread at a
 * time. For multi-threaded producers, give each thread its own clone.
 *
 * @tparam T The value type to send.
 */
template<typename T>
class MpscSender {
public:
    explicit MpscSender(detail::Rc<detail::MpscShared<T>> shared)
        : m_shared(std::move(shared))
    {
        std::lock_guard lock(m_shared->mutex);
        ++m_shared->sender_count;
    }

    MpscSender(const MpscSender&)            = delete;
    MpscSender& operator=(const MpscSender&) = delete;
    MpscSender(MpscSender&&)                 = default;

    // Hand-written rather than defaulted: a defaulted move-assignment would
    // just overwrite m_shared, dropping the old Rc without running the
    // sender_count-decrement-and-wake protocol below (that protocol only
    // lives in the destructor). std::swap is NOT sufficient here — if the
    // right-hand side is a named object moved via std::move (not a genuine
    // temporary), swapping just stashes the old state inside that named
    // object, deferring the close until IT happens to go out of scope,
    // which may be arbitrarily late. Instead, explicitly close the old
    // channel right now via the same helper the destructor uses, then take
    // over the new state.
    MpscSender& operator=(MpscSender&& other) noexcept {
        if (this != &other) {
            close();
            m_shared = std::move(other.m_shared);
        }
        return *this;
    }

    ~MpscSender() { close(); }

    /**
     * @brief Sends @p value asynchronously.
     *
     * Returns an `MpscSendFuture` that resolves immediately if buffer space is
     * available, or suspends until space opens up. If the receiver is dropped
     * while suspended, resolves with `std::unexpected(value)`.
     *
     * NOT ISR-SAFE: calls waker->wake() which touches shared_ptr ref-counts and
     * the executor queue. Use IsrChannel<T>::send_from_isr() from ISR context instead.
     */
    [[nodiscard]] MpscSendFuture<T> send(T value) {
        return MpscSendFuture<T>(m_shared, std::move(value));
    }

    /**
     * @brief Attempts to send @p value without suspending.
     *
     * Returns `{}` on success. On failure returns `TrySendError<T>` carrying
     * the unsent value and the reason (`Full` or `Disconnected`).
     *
     * NOT ISR-SAFE: calls waker->wake() on success. Use IsrChannel<T>::send_from_isr()
     * from ISR context instead.
     */
    std::expected<void, TrySendError<T>> try_send(T value) {
        std::unique_lock lock(m_shared->mutex);
        if (!m_shared->receiver_alive)
            return std::unexpected(TrySendError<T>(
                TrySendError<T>::Kind::Disconnected, std::move(value)));
        if (m_shared->buffer.size() >= m_shared->capacity)
            return std::unexpected(TrySendError<T>(
                TrySendError<T>::Kind::Full, std::move(value)));
        m_shared->buffer.push_back(std::move(value));
        m_shared->cv.notify_all();
        if (m_shared->receiver_waiter.has_value()) {
            auto waker = std::move(m_shared->receiver_waiter->waker);
            m_shared->receiver_waiter.reset();
            lock.unlock();
            waker->wake();
        }
        return {};
    }

    /**
     * @brief Blocks the calling OS thread until @p value can be sent or the receiver is dropped.
     *
     * Intended for use on threads created by `spawn_blocking`. **Do not call from a
     * coroutine or executor thread** — it will block the thread and stall the executor.
     *
     * NOT ISR-SAFE: blocks on a condition variable and calls waker->wake(). Use
     * IsrChannel<T>::send_from_isr() from ISR context instead.
     *
     * @return `{}` on success. `std::unexpected(value)` if the receiver was dropped
     *         while waiting.
     */
    std::expected<void, T> blocking_send(T value) {
        std::unique_lock lock(m_shared->mutex);
        m_shared->cv.wait(lock, [this] {
            return !m_shared->receiver_alive
                || m_shared->buffer.size() < m_shared->capacity;
        });

        if (!m_shared->receiver_alive)
            return std::unexpected(std::move(value));

        m_shared->buffer.push_back(std::move(value));
        if (m_shared->receiver_waiter.has_value()) {
            auto waker = std::move(m_shared->receiver_waiter->waker);
            m_shared->receiver_waiter.reset();
            m_shared->cv.notify_all();
            lock.unlock();
            waker->wake();
        } else {
            m_shared->cv.notify_all();
        }
        return {};
    }

    /**
     * @brief Returns `true` if the receiver has been dropped.
     *
     * A `true` result guarantees that any subsequent `send()` or `try_send()` will
     * fail. A `false` result is a snapshot — the receiver may be dropped
     * concurrently before the send, so failure is still possible.
     */
    [[nodiscard]] bool is_closed() const {
        std::lock_guard lock(m_shared->mutex);
        return !m_shared->receiver_alive;
    }

    /**
     * @brief Creates an independent sender clone that shares the same channel.
     *
     * Each clone increments the sender reference count. The channel closes from
     * the sender side only when the last clone is destroyed.
     */
    [[nodiscard]] MpscSender<T> clone() const {
        return MpscSender<T>(m_shared);
    }

private:
    // Decrements sender_count and wakes the receiver if this was the last
    // sender. Shared between the destructor and move-assignment so both
    // close the channel through the same protocol.
    void close() {
        if (!m_shared) return;
        detail::Rc<coro::detail::Waker> waker;
        {
            std::lock_guard lock(m_shared->mutex);
            if (--m_shared->sender_count == 0) {
                if (m_shared->receiver_waiter.has_value())
                    waker = std::move(m_shared->receiver_waiter->waker);
                m_shared->cv.notify_all();
            }
        }
        if (waker) waker->wake();
        m_shared = nullptr;
    }

    detail::Rc<detail::MpscShared<T>> m_shared;
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
class MpscReceiver {
public:
    using ItemType = T;

    explicit MpscReceiver(detail::Rc<detail::MpscShared<T>> shared)
        : m_shared(std::move(shared)) {}

    MpscReceiver(const MpscReceiver&)            = delete;
    MpscReceiver& operator=(const MpscReceiver&) = delete;
    MpscReceiver(MpscReceiver&&)                 = default;

    // Hand-written rather than defaulted: a defaulted move-assignment would
    // just overwrite m_shared, dropping the old Rc without running the
    // receiver_alive-clear-and-wake protocol below. std::swap is NOT
    // sufficient — if the right-hand side is a named object moved via
    // std::move rather than a genuine temporary, swapping just stashes the
    // old state inside that named object, deferring the close until it
    // happens to go out of scope. Explicitly close the old channel via the
    // same helper the destructor uses, then take over the new state.
    MpscReceiver& operator=(MpscReceiver&& other) noexcept {
        if (this != &other) {
            close();
            m_shared    = std::move(other.m_shared);
            m_recv_node = std::move(other.m_recv_node);
        }
        return *this;
    }

    ~MpscReceiver() { close(); }

    /**
     * @brief Returns `true` if all senders have been dropped and the buffer is empty.
     */
    [[nodiscard]] bool all_senders_dropped() const {
        std::lock_guard lock(m_shared->mutex);
        return m_shared->sender_count == 0 && m_shared->buffer.empty();
    }

    /**
     * @brief Returns a future that resolves to the next value from the channel.
     *
     * Resolves with `nullopt` when all senders have been dropped and the buffer
     * is drained (stream exhausted).
     */
    [[nodiscard]] MpscRecvFuture<T> recv() {
        return MpscRecvFuture<T>(m_shared);
    }

    /**
     * @brief Blocks the calling OS thread until an item is available or the channel closes.
     *
     * Intended for use on threads created by `spawn_blocking`. **Do not call from a
     * coroutine or executor thread** — it will block the thread and stall the executor.
     *
     * @return The next item, or `std::nullopt` when all senders are dropped and the
     *         buffer is drained.
     */
    std::optional<T> blocking_recv() {
        std::unique_lock lock(m_shared->mutex);
        m_shared->cv.wait(lock, [this] {
            return !m_shared->buffer.empty()
                || !m_shared->sender_waiters.empty()
                || m_shared->sender_count == 0;
        });

        if (!m_shared->buffer.empty()) {
            T val = std::move(m_shared->buffer.front());
            m_shared->buffer.pop_front();
            auto waker = _tryPromoteSender();
            m_shared->cv.notify_all();
            lock.unlock();
            if (waker) waker->wake();
            return val;
        }

        if (auto* raw = m_shared->sender_waiters.pop_front()) {
            auto* node = static_cast<detail::MpscSenderNode<T>*>(raw);
            T val = std::move(*node->value);
            node->value.reset();  // mark consumed so MpscSendFuture::poll() returns Ready on re-poll
            auto waker = std::move(node->waker);
            m_shared->cv.notify_all();
            lock.unlock();
            if (waker) waker->wake();
            return val;
        }

        return std::nullopt;
    }

    /**
     * @brief Attempts to receive a value without suspending.
     *
     * Returns:
     * - `T` on success.
     * - `ChannelError::Empty` — channel open but nothing buffered yet.
     * - `ChannelError::SenderDropped` — all senders gone and buffer drained.
     */
    [[nodiscard]] std::expected<T, ChannelError> try_recv() {
        std::unique_lock lock(m_shared->mutex);
        if (!m_shared->buffer.empty()) {
            T val = std::move(m_shared->buffer.front());
            m_shared->buffer.pop_front();
            auto waker = _tryPromoteSender();
            m_shared->cv.notify_all();
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
     */
    PollResult<std::optional<T>> poll_next(coro::detail::Context& ctx) {
        std::unique_lock lock(m_shared->mutex);

        if (!m_shared->buffer.empty()) {
            T val = std::move(m_shared->buffer.front());
            m_shared->buffer.pop_front();
            auto waker = _tryPromoteSender();
            m_shared->cv.notify_all();
            lock.unlock();
            if (waker) waker->wake();
            return std::optional<T>(std::move(val));
        }

        if (auto* raw = m_shared->sender_waiters.pop_front()) {
            auto* node = static_cast<detail::MpscSenderNode<T>*>(raw);
            T val = std::move(*node->value);
            node->value.reset();  // mark consumed so MpscSendFuture::poll() returns Ready on re-poll
            auto waker = std::move(node->waker);
            lock.unlock();
            if (waker) waker->wake();
            return std::optional<T>(std::move(val));
        }

        if (m_shared->sender_count == 0)
            return std::optional<T>(std::nullopt);

        m_recv_node.waker = ctx.getWaker();
        m_shared->receiver_waiter = m_recv_node;
        return PollPending;
    }

private:
    detail::Rc<coro::detail::Waker> _tryPromoteSender() {
        if (auto* raw = m_shared->sender_waiters.pop_front()) {
            auto* node = static_cast<detail::MpscSenderNode<T>*>(raw);
            m_shared->buffer.push_back(std::move(*node->value));
            node->value.reset();  // mark consumed so MpscSendFuture::poll() returns Ready on re-poll
            return std::move(node->waker);
        }
        return nullptr;
    }

    // Clears receiver_alive and wakes any blocked senders. Shared between the
    // destructor and move-assignment so both close the channel through the
    // same protocol.
    void close() {
        if (!m_shared) return;
        std::vector<detail::Rc<coro::detail::Waker>> wakers;
        {
            std::lock_guard lock(m_shared->mutex);
            m_shared->receiver_alive = false;
            while (auto* raw = m_shared->sender_waiters.pop_front()) {
                auto* node = static_cast<detail::MpscSenderNode<T>*>(raw);
                if (node->waker) wakers.push_back(std::move(node->waker));
            }
            m_shared->cv.notify_all();
        }
        for (auto& w : wakers) w->wake();
        m_shared = nullptr;
    }

    detail::Rc<detail::MpscShared<T>> m_shared;
    detail::MpscReceiverNode               m_recv_node;
};

/**
 * @brief Creates a linked (sender, receiver) pair with the given buffer capacity.
 *
 * @tparam T       The value type to transport.
 * @param capacity Maximum number of values buffered before senders suspend.
 * @return A pair `{MpscSender<T>, MpscReceiver<T>}`.
 */
template<typename T>
[[nodiscard]] std::pair<MpscSender<T>, MpscReceiver<T>> mpsc_channel(size_t capacity) {
    auto shared = detail::make_rc<detail::MpscShared<T>>(capacity);
    return { MpscSender<T>(shared), MpscReceiver<T>(shared) };
}

} // namespace coro
