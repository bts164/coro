#pragma once

#include <coro/detail/context.h>
#include <coro/detail/poll_result.h>
#include <coro/detail/waker.h>
#include <memory>
#include <mutex>

namespace coro {

/**
 * @brief A single-waiter async set/wait primitive for pure signalling.
 *
 * An `Event` starts unset. A coroutine calls `wait()` to obtain a `WaitFuture`
 * and awaits it; the future suspends until `set()` is called from any context
 * (another task, a callback, or another thread).
 *
 * ```cpp
 * coro::Event ev;
 *
 * // Producer — any context, no co_await needed:
 * ev.set();
 *
 * // Consumer — coroutine:
 * co_await ev.wait();   // suspends until set() is called
 * ```
 *
 * **Latch semantics:** if `set()` has already been called when `wait()` is
 * first polled, the future resolves immediately without suspending.
 *
 * **Reuse:** call `clear()` to reset the event so it can be waited on again.
 *
 * **Single-waiter:** at most one task may be suspended on `wait()` at a time.
 * Waiting from multiple tasks concurrently is undefined behaviour.
 *
 * **Lifetime:** if the `Event` is destroyed while a waiter is suspended, the
 * waiter is woken immediately and the `WaitFuture` resolves as if `set()` had
 * been called. The waiter cannot distinguish between a normal `set()` and
 * destruction of the `Event` — both complete the future with `void`.
 * If no task is currently suspended, destruction is a no-op.
 *
 * **Thread safety:** `set()` and `clear()` are safe to call from any thread.
 * `wait()` must be called from the owning coroutine's thread.
 */
class Event {
public:
    /**
     * @brief `Future<void>` returned by `Event::wait()`.
     *
     * Resolves immediately if the event is already set; otherwise suspends and
     * registers a waker that `set()` will call when the event fires.
     */
    class WaitFuture {
        friend class Event;
    public:
        using OutputType = void;

        explicit WaitFuture(Event* event) noexcept : m_event(event) {}

        WaitFuture(WaitFuture&& other) noexcept
            : m_event(other.m_event)
        {
            other.m_event = nullptr;
        }

        WaitFuture& operator=(WaitFuture&& other) = delete;
        WaitFuture(const WaitFuture&)            = delete;
        WaitFuture& operator=(const WaitFuture&) = delete;

        // Clears the registered waker so a later set() does not spuriously
        // wake a task that is no longer interested in this event (e.g. because
        // a select() branch was cancelled).
        ~WaitFuture() { _clearWaker(); }

        PollResult<void> poll(detail::Context& ctx) {
            if (nullptr == m_event) {
                return PollReady;
            }
            std::lock_guard lock(m_event->m_mutex);
            if (m_event->m_set) {
                return PollReady;
            }
            // RACE CONDITION NOTE: waker is set inside the mutex so set() cannot
            // sneak in between our m_set check and the waker registration, which
            // would produce a lost wakeup.
            m_event->m_wait_future = this;
            m_event->m_waker = ctx.getWaker();
            return PollPending;
        }

    private:
        Event* m_event;

        void _clearWaker() noexcept {
            if (!m_event) return;
            std::lock_guard lock(m_event->m_mutex);
            m_event->m_wait_future = nullptr;
            m_event->m_waker = nullptr;
        }
    };

    Event()                        = default;
    ~Event()
    {
        std::unique_lock lk(m_mutex);
        if (nullptr != m_wait_future) {
            std::exchange(m_wait_future, nullptr)->m_event = nullptr;
            auto waker = std::exchange(m_waker, nullptr);
            lk.unlock();
            waker->wake();
        }
    }
    Event(const Event&)            = delete;
    Event& operator=(const Event&) = delete;
    // Not movable: WaitFuture holds a raw pointer to this object.
    Event(Event&&)                 = delete;
    Event& operator=(Event&&)      = delete;

    /**
     * @brief Returns a `WaitFuture` that resolves when the event is set.
     *
     * If the event is already set, the future resolves on the first poll
     * without registering a waker.
     */
    [[nodiscard]] WaitFuture wait() noexcept { return WaitFuture(this); }

    /**
     * @brief Sets the event and wakes any suspended waiter.
     *
     * Thread-safe. Idempotent — calling `set()` on an already-set event is a
     * no-op (the waiter has already been woken).
     */
    void set() {
        std::shared_ptr<detail::Waker> waker;
        {
            std::lock_guard lock(m_mutex);
            m_set = true;
            // RACE CONDITION NOTE: waker must be extracted inside the mutex.
            // Extracting it outside would allow a concurrent wait() call to
            // register a new waker after we read m_set=true but before we
            // clear m_waker — that waker would never be woken.
            waker = std::exchange(m_waker, nullptr);
        }
        if (waker) waker->wake();
    }

    /**
     * @brief Resets the event so it can be waited on again.
     *
     * Thread-safe. Any task currently suspended on `wait()` will not be woken
     * by a subsequent `set()` until `wait()` is called again.
     */
    void clear() {
        std::lock_guard lock(m_mutex);
        m_set = false;
    }

    /**
     * @brief Returns true if the event is currently set.
     *
     * Thread-safe snapshot. The result may be stale by the time the caller acts
     * on it; prefer awaiting `wait()` for reliable notification.
     */
    bool is_set() const {
        std::lock_guard lock(m_mutex);
        return m_set;
    }

private:
    mutable std::mutex             m_mutex;
    bool                           m_set   = false;
    WaitFuture*                    m_wait_future = nullptr;
    std::shared_ptr<detail::Waker> m_waker;
};

} // namespace coro
