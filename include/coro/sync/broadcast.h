#pragma once

#include <coro/detail/context.h>
#include <coro/detail/intrusive_list.h>
#include <coro/detail/mutex.h>
#include <coro/detail/poll_result.h>
#include <coro/detail/rc.h>
#include <coro/detail/waker.h>
#include <coro/sync/channel_error.h>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <expected>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace coro {

namespace detail {

/**
 * @brief Intrusive node for a suspended `BroadcastReceiver`. Lives inside `BroadcastRecvFuture<T>`.
 *
 * Linked into `BroadcastShared::receiver_waiters` while the future is suspended.
 * Removed by the future's destructor if still linked at cancellation time.
 */
struct BroadcastReceiverNode : coro::detail::IntrusiveListNode {
    detail::Rc<coro::detail::Waker> waker;
};

/**
 * @brief Intrusive node for a suspended `receivers_active()`. Lives inside
 * `BroadcastReceiversActiveFuture<T>`.
 */
struct BroadcastSenderNode : coro::detail::IntrusiveListNode {
    detail::Rc<coro::detail::Waker> waker;
};

/**
 * @brief One value in a broadcast channel's ring, and how many receivers have yet to read it.
 *
 * `remaining` is the number of receivers active at `send()` that haven't read the value
 * or been dropped or deactivated since. The receiver that takes it to 0 empties the slot.
 */
template<typename T>
struct RingSlot {
    std::optional<T> value;
    size_t           remaining = 0;
};

/**
 * @brief Shared state for a broadcast channel.
 *
 * `ring[seq % capacity]` holds the value sent with sequence number `seq`, once that send
 * has happened, until every receiver counted in it has read it or a later wraparound
 * overwrites it. `next_seq` is the sequence number that will be assigned to the *next*
 * `send()`. A receiver's cursor is a sequence number: values at or above the cursor are
 * unread by that receiver, and each of them counts it in `remaining`.
 *
 * The oldest sequence number still held in the ring is `next_seq - capacity` (clamped to 0
 * before the ring has wrapped once). A receiver whose cursor falls below that has lagged —
 * the value(s) it hadn't read yet were overwritten — and is reported `Lagged` rather than
 * being handed stale or wrong data.
 */
template<typename T>
struct BroadcastShared {
    detail::Mutex                  mutex;
    std::vector<RingSlot<T>>       ring;       ///< Fixed-capacity, allocated once at construction.
    size_t                         capacity;
    uint64_t                       next_seq = 0; ///< Sequence number assigned to the next send().
    size_t                         sender_count   = 0;
    size_t                         receiver_count = 0; ///< Active receivers only.
    coro::detail::IntrusiveList<>  receiver_waiters; ///< Suspended BroadcastRecvFuture nodes.
    coro::detail::IntrusiveList<>  sender_waiters;   ///< Suspended receivers_active() nodes.

    explicit BroadcastShared(size_t cap) : ring(cap), capacity(cap) {}

    /// @brief The oldest sequence number still present in the ring. Caller holds `mutex`.
    uint64_t oldest_seq() const noexcept {
        return next_seq > capacity ? next_seq - capacity : 0;
    }

    /**
     * @brief Counts a new receiver and returns its starting cursor.
     *
     * Wakes every `receivers_active()` waiter when the count goes from 0 to 1.
     */
    uint64_t add_receiver() {
        std::vector<detail::Rc<coro::detail::Waker>> wakers;
        uint64_t start;
        {
            std::lock_guard lock(mutex);
            if (receiver_count++ == 0) {
                while (auto* raw = sender_waiters.pop_front()) {
                    auto* node = static_cast<BroadcastSenderNode*>(raw);
                    if (node->waker) wakers.push_back(std::move(node->waker));
                }
            }
            start = next_seq;
        }
        for (auto& w : wakers) w->wake();
        return start;
    }

    /**
     * @brief Uncounts a receiver whose cursor is @p cursor, releasing every value it
     * hadn't read: at most `capacity` slots.
     *
     * A value released here is destroyed while the mutex is held.
     */
    void remove_receiver(uint64_t cursor) {
        std::lock_guard lock(mutex);
        for (uint64_t seq = std::max(cursor, oldest_seq()); seq < next_seq; ++seq) {
            auto& slot = ring[seq % capacity];
            assert(slot.remaining > 0 && "broadcast slot released by a receiver it didn't count");
            if (--slot.remaining == 0) slot.value.reset();
        }
        --receiver_count;
    }

    /**
     * @brief Reads the value at @p seq for one receiver. Caller holds `mutex`.
     *
     * Copies it, unless this is the last receiver to read it, which moves it out.
     */
    T take(uint64_t seq) {
        auto& slot = ring[seq % capacity];
        assert(slot.value.has_value() && slot.remaining > 0);
        if (--slot.remaining > 0) return *slot.value;
        T val = std::move(*slot.value);
        slot.value.reset();
        return val;
    }
};

} // namespace detail

template<typename T> class BroadcastSender;
template<typename T> class BroadcastReceiver;
template<typename T> class InactiveBroadcastReceiver;

/**
 * @brief Future returned by `BroadcastReceiver<T>::recv()`.
 *
 * Satisfies `Future<std::expected<T, BroadcastRecvError>>`. Resolves with the next unread
 * value, `Lagged` if the receiver's cursor fell behind the oldest buffered value, or
 * `Closed` if every sender has been dropped and nothing buffered remains unread.
 *
 * **Cancellation:** if dropped while suspended, the destructor acquires the channel mutex
 * and unlinks the intrusive node before the coroutine frame is freed.
 *
 * **Lifetime:** holds a reference to the originating `BroadcastReceiver<T>` (to update its
 * cursor on completion). The receiver must not be destroyed or moved while this future is
 * live.
 *
 * @tparam T The value type to receive.
 */
template<typename T>
class BroadcastRecvFuture {
public:
    using OutputType = std::expected<T, BroadcastRecvError>;

    /// @brief Called by `BroadcastReceiver::recv()`.
    BroadcastRecvFuture(BroadcastReceiver<T>& rx, detail::Rc<detail::BroadcastShared<T>> shared)
        : m_rx(rx), m_shared(std::move(shared)) {}

    BroadcastRecvFuture(const BroadcastRecvFuture&)            = delete;
    BroadcastRecvFuture& operator=(const BroadcastRecvFuture&) = delete;
    BroadcastRecvFuture& operator=(BroadcastRecvFuture&&)      = delete;

    // Reference members cannot be rebound, so the move constructor must be
    // explicit. Only valid before poll() — the node must not be linked.
    BroadcastRecvFuture(BroadcastRecvFuture&& other) noexcept
        : m_rx(other.m_rx)
        , m_shared(std::move(other.m_shared))
        , m_node(std::move(other.m_node))
    {
        assert(!m_node.prev && !m_node.next &&
               "BroadcastRecvFuture moved while linked — undefined behaviour");
    }

    ~BroadcastRecvFuture() {
        if (!m_shared) return;
        {
            std::lock_guard lock(m_shared->mutex);
            m_shared->receiver_waiters.remove(&m_node);
        }
    }

    /**
     * @brief Advances the receive operation.
     *
     * 1. Cursor behind the oldest buffered value → `Lagged`, cursor snaps to oldest.
     * 2. Cursor has an unread value waiting → `Ready(value)`, cursor advances by one.
     * 3. No senders remain and nothing unread → `Closed`.
     * 4. Otherwise → register waker, suspend.
     *
     * Defined out-of-line below, after `BroadcastReceiver<T>` is a complete type.
     */
    PollResult<OutputType> poll(coro::detail::Context& ctx);

private:
    BroadcastReceiver<T>&                       m_rx;
    detail::Rc<detail::BroadcastShared<T>> m_shared;
    detail::BroadcastReceiverNode                m_node;
};

/**
 * @brief Future returned by `BroadcastSender<T>::receivers_active()`.
 *
 * Satisfies `Future<void>`. Resolves when the channel has at least one active receiver, at
 * once if it already has. Never fails.
 *
 * **Cancellation:** if dropped while suspended, the destructor acquires the channel mutex
 * and unlinks the intrusive node before the coroutine frame is freed.
 *
 * **Move safety:** moving is only valid before `poll()` has been called.
 *
 * @tparam T The value type of the channel.
 */
template<typename T>
class BroadcastReceiversActiveFuture {
public:
    using OutputType = void;

    explicit BroadcastReceiversActiveFuture(detail::Rc<detail::BroadcastShared<T>> shared)
        : m_shared(std::move(shared)) {}

    BroadcastReceiversActiveFuture(const BroadcastReceiversActiveFuture&)            = delete;
    BroadcastReceiversActiveFuture& operator=(const BroadcastReceiversActiveFuture&) = delete;
    BroadcastReceiversActiveFuture& operator=(BroadcastReceiversActiveFuture&&)      = delete;

    // Only valid before poll() — the node must not be linked when moved.
    BroadcastReceiversActiveFuture(BroadcastReceiversActiveFuture&& other) noexcept
        : m_shared(std::move(other.m_shared))
        , m_node(std::move(other.m_node))
    {
        assert(!m_node.prev && !m_node.next &&
               "BroadcastReceiversActiveFuture moved while linked — undefined behaviour");
    }

    ~BroadcastReceiversActiveFuture() {
        if (!m_shared) return;
        std::lock_guard lock(m_shared->mutex);
        m_shared->sender_waiters.remove(&m_node);
    }

    PollResult<void> poll(coro::detail::Context& ctx) {
        std::lock_guard lock(m_shared->mutex);
        m_shared->sender_waiters.remove(&m_node);
        if (m_shared->receiver_count > 0) return PollReady;
        m_node.waker = ctx.getWaker();
        m_shared->sender_waiters.push_back(&m_node);
        return PollPending;
    }

private:
    detail::Rc<detail::BroadcastShared<T>> m_shared;
    detail::BroadcastSenderNode            m_node;
};

/**
 * @brief Producer handle for a broadcast channel. Cloneable; each clone is independent.
 *
 * `send()` is synchronous and never suspends. The channel closes from the sender side only
 * when the last sender clone is dropped — receivers being dropped (even all of them) has no
 * bearing on this; a sender with zero current receivers remains fully valid and
 * `subscribe()`-able.
 *
 * **Thread safety:** each instance must be used by at most one thread at a time. For
 * multi-threaded producers, give each thread its own clone.
 *
 * @tparam T The value type to send. Must be copy-constructible — every receiver but the
 *           last to read a value reads its own copy out of the ring buffer.
 */
template<typename T>
class BroadcastSender {
public:
    explicit BroadcastSender(detail::Rc<detail::BroadcastShared<T>> shared)
        : m_shared(std::move(shared))
    {
        std::lock_guard lock(m_shared->mutex);
        ++m_shared->sender_count;
    }

    BroadcastSender(const BroadcastSender&)            = delete;
    BroadcastSender& operator=(const BroadcastSender&) = delete;
    BroadcastSender(BroadcastSender&&)                 = default;

    // Hand-written rather than defaulted: a defaulted move-assignment would
    // just overwrite m_shared, dropping the old Rc without running the
    // sender_count-decrement-and-wake protocol below. std::swap is NOT
    // sufficient — if the right-hand side is a named object moved via
    // std::move rather than a genuine temporary, swapping just stashes the
    // old state inside that named object, deferring the close until it
    // happens to go out of scope. Explicitly close the old channel via the
    // same helper the destructor uses, then take over the new state.
    BroadcastSender& operator=(BroadcastSender&& other) noexcept {
        if (this != &other) {
            close();
            m_shared = std::move(other.m_shared);
        }
        return *this;
    }

    ~BroadcastSender() { close(); }

    /**
     * @brief Sends @p value to every current receiver synchronously.
     *
     * The value is held until every current receiver has read it, or has been dropped or
     * deactivated. If the ring buffer is full, the oldest slot is silently overwritten —
     * receivers that hadn't yet read it observe this later as `Lagged` on their next
     * `recv()`/`try_recv()`. `send()` itself never blocks or fails for this reason.
     *
     * An overwritten value is destroyed while the channel's mutex is held, so its destructor
     * must not use this channel.
     *
     * @return The number of receivers notified on success, or `std::unexpected(value)` if
     *         there are currently zero receivers (nobody to deliver it to).
     *
     * NOT ISR-SAFE: calls waker->wake() which touches shared_ptr ref-counts and the executor
     * queue. Use IsrChannel<T>::send_from_isr() from ISR context instead.
     */
    std::expected<size_t, T> send(T value) {
        std::vector<detail::Rc<coro::detail::Waker>> wakers;
        size_t notified;
        {
            std::lock_guard lock(m_shared->mutex);
            if (m_shared->receiver_count == 0)
                return std::unexpected(std::move(value));

            auto& slot     = m_shared->ring[m_shared->next_seq % m_shared->capacity];
            slot.value     = std::move(value);
            slot.remaining = m_shared->receiver_count;
            ++m_shared->next_seq;
            notified = m_shared->receiver_count;

            while (auto* raw = m_shared->receiver_waiters.pop_front()) {
                auto* node = static_cast<detail::BroadcastReceiverNode*>(raw);
                if (node->waker) wakers.push_back(std::move(node->waker));
            }
        }
        for (auto& w : wakers) w->wake();
        return notified;
    }

    /**
     * @brief Creates an independent receiver that sees only values sent after this call —
     * no replay of channel history.
     */
    [[nodiscard]] BroadcastReceiver<T> subscribe() const {
        return BroadcastReceiver<T>(m_shared, m_shared->add_receiver());
    }

    /**
     * @brief Creates an inactive receiver: a handle that can become a receiver, but receives
     * nothing and isn't counted until it does.
     */
    [[nodiscard]] InactiveBroadcastReceiver<T> inactive() const {
        return InactiveBroadcastReceiver<T>(m_shared);
    }

    /**
     * @brief Returns a future that completes when the channel has at least one active
     * receiver, at once if it already has.
     *
     * For a task that reads one output only to make another: it waits for its own readers
     * rather than polling.
     */
    [[nodiscard]] BroadcastReceiversActiveFuture<T> receivers_active() const {
        return BroadcastReceiversActiveFuture<T>(m_shared);
    }

    /**
     * @brief Creates an independent sender clone that shares the same channel.
     *
     * Each clone increments the sender reference count. The channel closes from the sender
     * side only when the last clone is destroyed.
     */
    [[nodiscard]] BroadcastSender<T> clone() const {
        return BroadcastSender<T>(m_shared);
    }

    /**
     * @brief Returns `true` if there are currently zero receivers.
     *
     * A `true` result guarantees that any immediately subsequent `send()` will fail. This
     * is a normal, recoverable state — not a sign the channel is closed.
     */
    [[nodiscard]] bool all_receivers_dropped() const {
        std::lock_guard lock(m_shared->mutex);
        return m_shared->receiver_count == 0;
    }

private:
    // Decrements sender_count and wakes all receivers if this was the last
    // sender. Shared between the destructor and move-assignment so both
    // close the channel through the same protocol.
    void close() {
        if (!m_shared) return;
        std::vector<detail::Rc<coro::detail::Waker>> wakers;
        {
            std::lock_guard lock(m_shared->mutex);
            if (--m_shared->sender_count == 0) {
                while (auto* raw = m_shared->receiver_waiters.pop_front()) {
                    auto* node = static_cast<detail::BroadcastReceiverNode*>(raw);
                    if (node->waker) wakers.push_back(std::move(node->waker));
                }
            }
        }
        for (auto& w : wakers) w->wake();
        m_shared = nullptr;
    }

    detail::Rc<detail::BroadcastShared<T>> m_shared;
};

/**
 * @brief Consumer handle for a broadcast channel. Not cloneable — use `resubscribe()` to
 * obtain an additional independent receiver.
 *
 * Each receiver tracks its own cursor (a sequence number), so every receiver observes every
 * value sent while it is subscribed, independent of how fast other receivers consume.
 *
 * **Thread safety:** each instance must be used by at most one thread at a time.
 *
 * @tparam T The value type to receive.
 */
template<typename T>
class BroadcastReceiver {
public:
    BroadcastReceiver(const BroadcastReceiver&)            = delete;
    BroadcastReceiver& operator=(const BroadcastReceiver&) = delete;
    BroadcastReceiver(BroadcastReceiver&&)                 = default;

    // Hand-written rather than defaulted: a defaulted move-assignment would
    // just overwrite m_shared, dropping the old Rc without running the
    // receiver_count-decrement protocol below. std::swap is NOT sufficient —
    // if the right-hand side is a named object moved via std::move rather
    // than a genuine temporary, swapping just stashes the old state inside
    // that named object, deferring the decrement until it happens to go out
    // of scope. Explicitly close the old channel via the same helper the
    // destructor uses, then take over the new state.
    BroadcastReceiver& operator=(BroadcastReceiver&& other) noexcept {
        if (this != &other) {
            close();
            m_shared = std::move(other.m_shared);
            m_cursor = other.m_cursor;
        }
        return *this;
    }

    ~BroadcastReceiver() { close(); }

    /**
     * @brief Returns `true` if every sender has been dropped.
     *
     * When `true`, no further values will ever be published; `recv()` will resolve with
     * `Closed` once any already-buffered, not-yet-read values have been consumed.
     */
    [[nodiscard]] bool all_senders_dropped() const {
        std::lock_guard lock(m_shared->mutex);
        return m_shared->sender_count == 0;
    }

    /**
     * @brief Returns a future that resolves to the next unread value.
     *
     * **Lifetime:** the returned `BroadcastRecvFuture` holds a reference to `*this`. Do not
     * destroy or move this receiver while the future is live.
     */
    [[nodiscard]] BroadcastRecvFuture<T> recv() {
        return BroadcastRecvFuture<T>(*this, m_shared);
    }

    /**
     * @brief Attempts to receive a value without suspending.
     *
     * Returns:
     * - `T` on success.
     * - `BroadcastRecvError::Lagged` — cursor fell behind the oldest buffered value; cursor
     *   snaps to the oldest remaining value.
     * - `BroadcastRecvError::Closed` — every sender dropped and nothing buffered remains unread.
     * - `BroadcastRecvError::Empty` — channel open but nothing new since the last read.
     */
    [[nodiscard]] std::expected<T, BroadcastRecvError> try_recv() {
        std::lock_guard lock(m_shared->mutex);

        uint64_t oldest = m_shared->oldest_seq();
        if (m_cursor < oldest) {
            uint64_t skipped = oldest - m_cursor;
            m_cursor = oldest;
            return std::unexpected(BroadcastRecvError{BroadcastRecvError::Kind::Lagged, skipped});
        }

        if (m_cursor < m_shared->next_seq)
            return m_shared->take(m_cursor++);

        if (m_shared->sender_count == 0)
            return std::unexpected(BroadcastRecvError{BroadcastRecvError::Kind::Closed});

        return std::unexpected(BroadcastRecvError{BroadcastRecvError::Kind::Empty});
    }

    /**
     * @brief Creates an independent receiver that sees only values sent after this call.
     *
     * Always starts fresh at the channel's current sequence number — not at this
     * receiver's current cursor position.
     */
    [[nodiscard]] BroadcastReceiver<T> resubscribe() const {
        return BroadcastReceiver<T>(m_shared, m_shared->add_receiver());
    }

    /**
     * @brief Makes this receiver inactive, releasing every value it hadn't read.
     *
     * Any `recv()` future of this receiver must already have been dropped.
     */
    [[nodiscard]] InactiveBroadcastReceiver<T> deactivate() && {
        m_shared->remove_receiver(m_cursor);
        return InactiveBroadcastReceiver<T>(std::move(m_shared));
    }

    /// @brief Current cursor (sequence number of the next value this receiver hasn't read).
    uint64_t cursor() const noexcept { return m_cursor; }

private:
    friend class BroadcastSender<T>;
    friend class BroadcastRecvFuture<T>;
    friend class InactiveBroadcastReceiver<T>;

    // receiver_count is incremented by the caller, through BroadcastShared::add_receiver(),
    // which also gives the starting cursor under the same lock.
    BroadcastReceiver(detail::Rc<detail::BroadcastShared<T>> shared, uint64_t start_seq)
        : m_shared(std::move(shared)), m_cursor(start_seq) {}

    /// @brief Updates the cursor. Called by `BroadcastRecvFuture` on completion.
    void set_cursor(uint64_t seq) noexcept { m_cursor = seq; }

    // Decrements receiver_count and releases every value this receiver hadn't read.
    // Shared between the destructor and move-assignment so both close the channel
    // through the same protocol.
    void close() {
        if (!m_shared) return;
        // remove_receiver() unlocks before returning: resetting m_shared below may
        // drop the last reference to BroadcastShared, destroying its mutex.
        m_shared->remove_receiver(m_cursor);
        m_shared = nullptr;
    }

    detail::Rc<detail::BroadcastShared<T>> m_shared;
    uint64_t                                     m_cursor;
};

// --- BroadcastRecvFuture::poll definition (needs BroadcastReceiver to be complete) ---

template<typename T>
PollResult<typename BroadcastRecvFuture<T>::OutputType>
BroadcastRecvFuture<T>::poll(coro::detail::Context& ctx) {
    std::unique_lock lock(m_shared->mutex);

    uint64_t cursor = m_rx.cursor();
    uint64_t oldest = m_shared->oldest_seq();

    if (cursor < oldest) {
        uint64_t skipped = oldest - cursor;
        m_rx.set_cursor(oldest);
        m_shared->receiver_waiters.remove(&m_node);
        return OutputType(std::unexpected(
            BroadcastRecvError{BroadcastRecvError::Kind::Lagged, skipped}));
    }

    if (cursor < m_shared->next_seq) {
        m_rx.set_cursor(cursor + 1);
        m_shared->receiver_waiters.remove(&m_node);
        return OutputType(m_shared->take(cursor));
    }

    if (m_shared->sender_count == 0) {
        m_shared->receiver_waiters.remove(&m_node);
        return OutputType(std::unexpected(
            BroadcastRecvError{BroadcastRecvError::Kind::Closed}));
    }

    m_node.waker = ctx.getWaker();
    m_shared->receiver_waiters.remove(&m_node);
    m_shared->receiver_waiters.push_back(&m_node);
    return PollPending;
}

/**
 * @brief A handle that can become a broadcast receiver. Copyable.
 *
 * Receives nothing and isn't counted, so `all_receivers_dropped()` and `send()`'s failure
 * mean "nobody is reading now", whatever handles exist. It can't send, which is why it exists
 * apart from `subscribe()` on a sender clone. It doesn't keep the channel open: a receiver
 * activated after the last sender drops gets `Closed` from its first `recv()`.
 *
 * @tparam T The value type of the channel.
 */
template<typename T>
class InactiveBroadcastReceiver {
public:
    /// @brief Returns a new receiver, which sees only values sent after this call.
    [[nodiscard]] BroadcastReceiver<T> activate() const {
        return BroadcastReceiver<T>(m_shared, m_shared->add_receiver());
    }

private:
    friend class BroadcastSender<T>;
    friend class BroadcastReceiver<T>;

    explicit InactiveBroadcastReceiver(detail::Rc<detail::BroadcastShared<T>> shared)
        : m_shared(std::move(shared)) {}

    detail::Rc<detail::BroadcastShared<T>> m_shared;
};

/**
 * @brief Creates a linked (sender, receiver) pair with the given ring buffer capacity.
 *
 * @tparam T       The value type to transport. Must be copy-constructible.
 * @param capacity Maximum number of not-yet-evicted values held in the ring buffer.
 * @throws std::invalid_argument if @p capacity is zero — a zero-capacity channel could
 *         never hold a value for any receiver to read.
 * @return A pair `{BroadcastSender<T>, BroadcastReceiver<T>}`.
 */
template<typename T>
[[nodiscard]] std::pair<BroadcastSender<T>, BroadcastReceiver<T>> broadcast_channel(size_t capacity) {
    if (capacity == 0)
        throw std::invalid_argument("broadcast_channel: capacity must be non-zero");
    auto shared = detail::make_rc<detail::BroadcastShared<T>>(capacity);
    BroadcastSender<T> tx(shared);
    BroadcastReceiver<T> rx = tx.subscribe();
    return { std::move(tx), std::move(rx) };
}

} // namespace coro
