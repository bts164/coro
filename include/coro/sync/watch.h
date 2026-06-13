#pragma once

#include <coro/detail/context.h>
#include <coro/detail/intrusive_list.h>
#include <coro/detail/poll_result.h>
#include <coro/detail/waker.h>
#include <coro/sync/channel_error.h>

#include <cassert>
#include <expected>
#include <memory>
#include <utility>
#include <coro/detail/mutex.h>

namespace coro {

namespace detail {

/**
 * @brief Intrusive node for a suspended `WatchReceiver`. Lives inside `WatchChangedFuture<T>`.
 *
 * Linked into `WatchShared::receiver_waiters` while the future is suspended.
 * Removed by the future's destructor if still linked at cancellation time.
 */
struct WatchReceiverNode : coro::detail::IntrusiveListNode {
    std::shared_ptr<coro::detail::Waker> waker;
    uint64_t                             last_seen = 0; ///< Version when the future suspended.
};

/**
 * @brief Shared state for a watch channel.
 *
 * Two separate locks avoid contention between long-held `borrow()` read locks
 * and `changed()` waker registration:
 * - `value_mutex` — guards `value` and `version`; taken shared by `borrow()`,
 *   exclusive by `send()`.
 * - `waker_mutex` — guards `receiver_waiters`, `sender_count`, and `receiver_count`;
 *   taken briefly by `changed()` (register waker) and `send()` (drain wakers).
 *
 * **Lock ordering:** always acquire `value_mutex` before `waker_mutex` to prevent
 * deadlocks. `send()` acquires both; `changed()` acquires only `waker_mutex`.
 *
 * @tparam T The watched value type.
 */
template<typename T>
struct WatchShared {
    detail::SharedMutex            value_mutex;
    T                            value;
    uint64_t                     version = 0;    ///< Incremented on every send().

    detail::Mutex                waker_mutex;
    coro::detail::IntrusiveList<> receiver_waiters; ///< Suspended WatchChangedFuture nodes.
    size_t                       receiver_count = 0;
    size_t                       sender_count   = 0; ///< Number of live WatchSender handles.

    explicit WatchShared(T initial) : value(std::move(initial)) {}
};

} // namespace detail

template<typename T> class WatchReceiver;

/**
 * @brief RAII read-lock guard returned by `WatchReceiver<T>::borrow()`.
 *
 * Holds a shared lock on the channel's value for its lifetime. Provides
 * read-only access via `operator*` and `operator->`.
 *
 * **Not copyable** — copying would require duplicating the lock.
 * **Movable** — ownership can be transferred.
 *
 * @warning Do not hold a `WatchBorrowGuard` across a `co_await` point. Doing so
 *          keeps the read lock live while suspended, blocking all `send()` calls.
 *
 * @tparam T The watched value type.
 */
template<typename T>
class WatchBorrowGuard {
public:
    WatchBorrowGuard(std::shared_ptr<detail::WatchShared<T>> shared,
                     std::shared_lock<detail::SharedMutex> lock)
        : m_shared(std::move(shared)), m_lock(std::move(lock)) {}

    WatchBorrowGuard(const WatchBorrowGuard&)            = delete;
    WatchBorrowGuard& operator=(const WatchBorrowGuard&) = delete;
    WatchBorrowGuard(WatchBorrowGuard&&)                 = default;
    WatchBorrowGuard& operator=(WatchBorrowGuard&&)      = default;

    const T& operator*()  const noexcept { return m_shared->value; }
    const T* operator->() const noexcept { return &m_shared->value; }

private:
    std::shared_ptr<detail::WatchShared<T>> m_shared;
    std::shared_lock<detail::SharedMutex>     m_lock;
};

/**
 * @brief RAII write-lock guard returned by `WatchSender<T>::borrow_mut()`.
 *
 * Holds an exclusive lock on the channel's value for its lifetime. Provides
 * mutable access via `operator*` and `operator->`. On destruction, increments
 * the channel version and wakes all receivers — equivalent to calling `send()`
 * with the modified value. No explicit `send()` call is needed.
 *
 * **Not copyable** — copying would require duplicating the lock.
 * **Movable** — ownership can be transferred.
 *
 * @warning Do not hold a `WatchBorrowMutGuard` across a `co_await` point. Doing so
 *          keeps the exclusive write lock live while suspended, blocking all
 *          `borrow()` calls on receivers.
 *
 * @tparam T The watched value type.
 */
template<typename T>
class WatchBorrowMutGuard {
public:
    WatchBorrowMutGuard(std::shared_ptr<detail::WatchShared<T>> shared,
                        std::unique_lock<detail::SharedMutex> lock)
        : m_shared(std::move(shared)), m_lock(std::move(lock)) {}

    WatchBorrowMutGuard(const WatchBorrowMutGuard&)            = delete;
    WatchBorrowMutGuard& operator=(const WatchBorrowMutGuard&) = delete;
    WatchBorrowMutGuard(WatchBorrowMutGuard&&)                 = default;
    WatchBorrowMutGuard& operator=(WatchBorrowMutGuard&&)      = default;

    ~WatchBorrowMutGuard() {
        if (!m_shared || !m_lock.owns_lock()) return;
        m_shared->version++;
        m_lock.unlock(); // release value_mutex before waker_mutex (lock ordering)

        std::vector<std::shared_ptr<coro::detail::Waker>> wakers;
        {
            std::lock_guard wl(m_shared->waker_mutex);
            while (auto* raw = m_shared->receiver_waiters.pop_front()) {
                auto* node = static_cast<detail::WatchReceiverNode*>(raw);
                if (node->waker) wakers.push_back(std::move(node->waker));
            }
        }
        for (auto& w : wakers) w->wake();
    }

    T& operator*()  noexcept { return m_shared->value; }
    T* operator->() noexcept { return &m_shared->value; }

private:
    std::shared_ptr<detail::WatchShared<T>> m_shared;
    std::unique_lock<detail::SharedMutex>     m_lock;
};

/**
 * @brief Future returned by `WatchReceiver<T>::changed()`.
 *
 * Satisfies `Future<std::expected<void, ChannelError>>`. Resolves when the
 * channel's version advances past `last_seen`. Resolves with
 * `ChannelError::SenderDropped` if the sender is dropped before a new value
 * is sent.
 *
 * **Cancellation:** if dropped while suspended, the destructor acquires
 * `waker_mutex` and unlinks the intrusive node.
 *
 * **Lifetime:** holds a reference to the originating `WatchReceiver<T>`. The
 * receiver must not be destroyed or moved while this future is live.
 *
 * @tparam T The watched value type.
 */
template<typename T>
class WatchChangedFuture {
public:
    using OutputType = std::expected<void, ChannelError>;

    /// @brief Called by `WatchReceiver::changed()`.
    WatchChangedFuture(WatchReceiver<T>& rx,
                       std::shared_ptr<detail::WatchShared<T>> shared,
                       uint64_t last_seen)
        : m_rx(rx), m_shared(std::move(shared)), m_last_seen(last_seen) {}

    WatchChangedFuture(const WatchChangedFuture&)            = delete;
    WatchChangedFuture& operator=(const WatchChangedFuture&) = delete;
    WatchChangedFuture& operator=(WatchChangedFuture&&)      = delete;

    // Reference members cannot be rebound, so the move constructor must be
    // explicit. Only valid before poll() — the node must not be linked.
    WatchChangedFuture(WatchChangedFuture&& other) noexcept
        : m_rx(other.m_rx)
        , m_shared(std::move(other.m_shared))
        , m_last_seen(other.m_last_seen)
        , m_node(std::move(other.m_node))
    {
        assert(!m_node.is_linked() &&
               "WatchChangedFuture moved while linked — undefined behaviour");
    }

    ~WatchChangedFuture() {
        if (!m_shared) return;
        if (m_node.is_linked()) {
            std::lock_guard lock(m_shared->waker_mutex);
            if (m_node.is_linked())
                m_shared->receiver_waiters.remove(&m_node);
        }
    }

    /**
     * @brief Advances the changed-detection future.
     *
     * Resolves immediately if the version has already advanced. Otherwise
     * registers the waker and returns Pending.
     */
    PollResult<OutputType> poll(coro::detail::Context& ctx);

private:
    WatchReceiver<T>&                       m_rx;
    std::shared_ptr<detail::WatchShared<T>> m_shared;
    uint64_t                                m_last_seen;
    detail::WatchReceiverNode               m_node;
};

/**
 * @brief Sender half of a watch channel. Cloneable via `clone()`.
 *
 * `send()` synchronously overwrites the watched value. Multiple senders may
 * exist simultaneously — the channel closes only when the last sender is dropped.
 *
 * **Thread safety:** each instance must be used by at most one thread at a time.
 * Give each thread its own clone.
 *
 * @tparam T The watched value type.
 */
template<typename T>
class WatchSender {
public:
    explicit WatchSender(std::shared_ptr<detail::WatchShared<T>> shared)
        : m_shared(std::move(shared))
    {
        std::lock_guard lock(m_shared->waker_mutex);
        ++m_shared->sender_count;
    }

    WatchSender(const WatchSender&)            = delete;
    WatchSender& operator=(const WatchSender&) = delete;
    WatchSender(WatchSender&&)                 = default;
    WatchSender& operator=(WatchSender&&)      = default;

    ~WatchSender() {
        if (!m_shared) return;
        // Decrement sender count; only wake receivers when the last sender drops.
        std::vector<std::shared_ptr<coro::detail::Waker>> wakers;
        {
            std::lock_guard wl(m_shared->waker_mutex);
            if (--m_shared->sender_count != 0) return;
            while (auto* raw = m_shared->receiver_waiters.pop_front()) {
                auto* node = static_cast<detail::WatchReceiverNode*>(raw);
                if (node->waker) wakers.push_back(std::move(node->waker));
            }
        }
        for (auto& w : wakers) w->wake();
    }

    /**
     * @brief Overwrites the watched value synchronously.
     *
     * Wakes all receivers suspended in `changed()`. Returns
     * `std::unexpected(value)` if all receivers have been dropped.
     *
     * NOT ISR-SAFE: calls waker->wake() which touches shared_ptr ref-counts and
     * the executor queue. Use IsrChannel<T>::send_from_isr() from ISR context instead.
     *
     * @param value New value to publish.
     * @return `{}` on success; `std::unexpected(std::move(value))` if no receivers remain.
     */
    std::expected<void, T> send(T value) {
        std::vector<std::shared_ptr<coro::detail::Waker>> wakers;
        {
            // Exclusive lock on value, then brief lock on wakers.
            std::unique_lock vl(m_shared->value_mutex);
            if (m_shared->receiver_count == 0)
                return std::unexpected(std::move(value));
            m_shared->value   = std::move(value);
            m_shared->version++;
            vl.unlock();

            std::lock_guard wl(m_shared->waker_mutex);
            while (auto* raw = m_shared->receiver_waiters.pop_front()) {
                auto* node = static_cast<detail::WatchReceiverNode*>(raw);
                if (node->waker) wakers.push_back(std::move(node->waker));
            }
        }
        for (auto& w : wakers) w->wake();
        return {};
    }

    /**
     * @brief Creates an independent sender sharing the same channel.
     *
     * The channel remains open until all sender clones are dropped.
     * Each clone must be used by at most one thread at a time.
     */
    [[nodiscard]] WatchSender<T> clone() const {
        return WatchSender<T>(m_shared);
    }

    /**
     * @brief Returns `true` if all receivers have been dropped.
     *
     * A `true` result guarantees that any subsequent `send()` will return
     * `std::unexpected`. A `false` result is a snapshot.
     */
    [[nodiscard]] bool all_receivers_dropped() const {
        std::lock_guard lock(m_shared->waker_mutex);
        return m_shared->receiver_count == 0;
    }

    /**
     * @brief Acquires an exclusive write lock and returns a guard for in-place mutation.
     *
     * The guard holds the exclusive lock for its lifetime. On destruction it
     * automatically increments the channel version and wakes all receivers.
     * No explicit `send()` call is needed — mutation through the guard is the
     * notification.
     *
     * Use this when you need to modify a field of a complex value in place rather
     * than constructing a new value to pass to `send()`.
     *
     * @warning Do not hold a `WatchBorrowMutGuard` across a `co_await` point.
     */
    [[nodiscard]] WatchBorrowMutGuard<T> borrow_mut() {
        return WatchBorrowMutGuard<T>(m_shared,
            std::unique_lock<detail::SharedMutex>(m_shared->value_mutex));
    }

    /**
     * @brief Modifies the watched value in place, notifying receivers only if changed.
     *
     * Calls `modify(T&)` under the exclusive value lock. If `modify` returns `true`
     * the version is incremented and all suspended receivers are woken. If `modify`
     * returns `false` the value, version, and waiters are left untouched.
     *
     * @param modify Callable with signature `bool(T&)`. Return `true` to signal
     *               that the value was changed, `false` to suppress notification.
     * @return `true` if `modify` indicated a change (receivers notified);
     *         `false` if `modify` indicated no change (no-op).
     */
    template<typename F>
    bool send_if_modified(F&& modify) {
        std::vector<std::shared_ptr<coro::detail::Waker>> wakers;
        {
            std::unique_lock vl(m_shared->value_mutex);
            if (!modify(m_shared->value)) return false;
            m_shared->version++;
            vl.unlock();

            std::lock_guard wl(m_shared->waker_mutex);
            while (auto* raw = m_shared->receiver_waiters.pop_front()) {
                auto* node = static_cast<detail::WatchReceiverNode*>(raw);
                if (node->waker) wakers.push_back(std::move(node->waker));
            }
        }
        for (auto& w : wakers) w->wake();
        return true;
    }

private:
    std::shared_ptr<detail::WatchShared<T>> m_shared;
};

/**
 * @brief Receiver half of a watch channel. Cloneable; each clone tracks its own version.
 *
 * - `changed()` — suspends until a new value is sent after `last_seen`.
 * - `borrow()`  — returns a `WatchBorrowGuard<T>` for read-only access under a shared lock.
 * - `clone()`   — creates an independent receiver starting at version 0.
 *
 * **Thread safety:** each instance must be used by at most one thread at a time.
 *
 * @tparam T The watched value type.
 */
template<typename T>
class WatchReceiver {
public:
    explicit WatchReceiver(std::shared_ptr<detail::WatchShared<T>> shared)
        : m_shared(std::move(shared)), m_last_seen(0)
    {
        std::lock_guard lock(m_shared->waker_mutex);
        ++m_shared->receiver_count;
    }

    WatchReceiver(const WatchReceiver&)            = delete;
    WatchReceiver& operator=(const WatchReceiver&) = delete;
    WatchReceiver(WatchReceiver&&)                 = default;
    WatchReceiver& operator=(WatchReceiver&&)      = default;

    ~WatchReceiver() {
        if (!m_shared) return;
        std::lock_guard lock(m_shared->waker_mutex);
        --m_shared->receiver_count;
    }

    /**
     * @brief Returns `true` if the sender has been dropped.
     *
     * When `true`, no further values will be published. The last published value
     * remains readable via `borrow()`. A `false` result is a snapshot.
     */
    [[nodiscard]] bool sender_dropped() const {
        std::lock_guard lock(m_shared->waker_mutex);
        return m_shared->sender_count == 0;
    }

    /**
     * @brief Returns a future that resolves when the value changes.
     *
     * The future resolves immediately if the channel's version is already
     * ahead of `last_seen` (i.e. at least one `send()` has occurred since
     * this receiver last observed the channel).
     *
     * **Lifetime:** the returned `WatchChangedFuture` holds a reference to `*this`.
     *              Do not destroy or move this receiver while the future is live.
     *
     * @return `WatchChangedFuture<T>` resolving to `std::expected<void, ChannelError>`.
     */
    [[nodiscard]] WatchChangedFuture<T> changed() {
        return WatchChangedFuture<T>(*this, m_shared, m_last_seen);
    }

    /**
     * @brief Acquires a read lock and returns a guard providing access to the current value.
     *
     * Does **not** update `last_seen` — a subsequent `changed()` will still resolve
     * for the current value if it has not been marked seen. Use `borrow_and_update()`
     * when you want to consume the current value and wait only for future changes.
     *
     * @warning Do not hold the returned guard across a `co_await` point.
     */
    [[nodiscard]] WatchBorrowGuard<T> borrow() {
        return WatchBorrowGuard<T>(m_shared,
            std::shared_lock<detail::SharedMutex>(m_shared->value_mutex));
    }

    /**
     * @brief Acquires a read lock, marks the current version as seen, and returns a guard.
     *
     * Equivalent to calling `borrow()` and then updating the receiver's version
     * atomically under the read lock. After this call, `changed()` will only
     * resolve for values sent *after* `borrow_and_update()` returns.
     *
     * @warning Do not hold the returned guard across a `co_await` point.
     */
    [[nodiscard]] WatchBorrowGuard<T> borrow_and_update() {
        std::shared_lock<detail::SharedMutex> lock(m_shared->value_mutex);
        m_last_seen = m_shared->version;
        return WatchBorrowGuard<T>(m_shared, std::move(lock));
    }

    /**
     * @brief Creates an independent receiver starting at version 0.
     *
     * The new receiver will see the next `send()` as a change, even if it was
     * already sent before `clone()` was called. Call `borrow()` on the clone
     * to read the current value without waiting.
     */
    [[nodiscard]] WatchReceiver<T> clone() const {
        return WatchReceiver<T>(m_shared);
    }

    /// @brief Updates `last_seen` to the current channel version. Called by `WatchChangedFuture`.
    void mark_seen(uint64_t version) noexcept { m_last_seen = version; }

    uint64_t last_seen() const noexcept { return m_last_seen; }

private:
    std::shared_ptr<detail::WatchShared<T>> m_shared;
    uint64_t                                m_last_seen;

    friend class WatchChangedFuture<T>;
};

// --- WatchChangedFuture::poll definition (needs WatchReceiver to be complete) ---

template<typename T>
PollResult<typename WatchChangedFuture<T>::OutputType>
WatchChangedFuture<T>::poll(coro::detail::Context& ctx) {
    // Check if already changed (no need to lock value_mutex — version is only
    // ever incremented, so a stale read is safe for the fast path).
    {
        std::shared_lock vl(m_shared->value_mutex);
        uint64_t current = m_shared->version;
        if (current != m_last_seen) {
            if (m_node.is_linked()) {
                vl.unlock();
                std::lock_guard wl(m_shared->waker_mutex);
                if (m_node.is_linked())
                    m_shared->receiver_waiters.remove(&m_node);
            }
            m_rx.mark_seen(current);
            return OutputType{};
        }
    }

    // Check for sender dropped.
    {
        std::lock_guard wl(m_shared->waker_mutex);
        if (m_shared->sender_count == 0) {
            if (m_node.is_linked())
                m_shared->receiver_waiters.remove(&m_node);
            return OutputType(std::unexpected(ChannelError::SenderDropped));
        }
        // Register waker and suspend.
        m_node.waker     = ctx.getWaker();
        m_node.last_seen = m_last_seen;
        if (!m_node.is_linked())
            m_shared->receiver_waiters.push_back(&m_node);
    }
    return PollPending;
}

/**
 * @brief Creates a watch channel with the given initial value.
 *
 * @tparam T     The watched value type.
 * @param initial The initial value published to all receivers.
 * @return A pair `{WatchSender<T>, WatchReceiver<T>}`.
 */
template<typename T>
[[nodiscard]] std::pair<WatchSender<T>, WatchReceiver<T>> watch_channel(T initial) {
    auto shared = std::make_shared<detail::WatchShared<T>>(std::move(initial));
    return { WatchSender<T>(shared), WatchReceiver<T>(shared) };
}

} // namespace coro
