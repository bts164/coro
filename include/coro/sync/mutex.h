#pragma once

#include <coro/detail/context.h>
#include <coro/detail/poll_result.h>
#include <coro/detail/waker.h>
#include <deque>
#include <memory>
#include <mutex>

namespace coro {

template<typename T>
class Mutex;

// ---------------------------------------------------------------------------
// MutexGuard<T>
// ---------------------------------------------------------------------------

/**
 * @brief RAII guard returned by `co_await mutex.lock()`.
 *
 * Provides exclusive access to the value owned by `Mutex<T>`. The mutex is
 * released when the guard is destroyed. Access the protected value via
 * `operator*` and `operator->`.
 *
 * **Do not hold a `MutexGuard` across a `co_await` point.** Doing so keeps
 * the mutex locked while the task is suspended, blocking all other waiters
 * for the entire duration of the suspension.
 *
 * @tparam T The protected value type.
 */
template<typename T>
class MutexGuard {
public:
    MutexGuard(MutexGuard&&) noexcept            = default;
    MutexGuard& operator=(MutexGuard&&) noexcept = default;
    MutexGuard(const MutexGuard&)                = delete;
    MutexGuard& operator=(const MutexGuard&)     = delete;

    ~MutexGuard();

    T&       operator*()        noexcept { return *m_value; }
    const T& operator*()  const noexcept { return *m_value; }
    T*       operator->()       noexcept { return  m_value; }
    const T* operator->() const noexcept { return  m_value; }

private:
    friend class Mutex<T>;

    MutexGuard(Mutex<T>* mutex, T* value) noexcept
        : m_mutex(mutex), m_value(value) {}

    Mutex<T>* m_mutex = nullptr;
    T*        m_value = nullptr;
};

// ---------------------------------------------------------------------------
// Mutex<T>
// ---------------------------------------------------------------------------

/**
 * @brief Async mutex that suspends the *task* rather than blocking the thread.
 *
 * `std::mutex::lock()` blocks the OS thread, starving the executor of its
 * worker. `Mutex<T>` instead suspends the coroutine and yields the thread back
 * to the executor while waiting, letting other tasks make progress.
 *
 * ```cpp
 * coro::Mutex<std::vector<int>> shared;
 *
 * coro::Coro<void> append(int value) {
 *     auto guard = co_await shared.lock();
 *     guard->push_back(value);
 * }   // guard released here, next waiter is woken
 * ```
 *
 * **Fairness:** waiters are woken in FIFO order.
 *
 * **Thread safety:** `lock()` and the internal release path are safe to call
 * from any thread.
 *
 * @tparam T The protected value type.
 */
template<typename T>
class Mutex {
public:
    /**
     * @brief `Future<MutexGuard<T>>` returned by `Mutex<T>::lock()`.
     *
     * Resolves immediately if the mutex is currently unlocked; otherwise
     * suspends and registers a waker that the previous guard's destructor
     * will call when the mutex is released.
     */
    class LockFuture {
    public:
        using OutputType = MutexGuard<T>;

        explicit LockFuture(Mutex<T>* mutex) noexcept : m_mutex(mutex) {}

        LockFuture(LockFuture&&) noexcept            = default;
        LockFuture& operator=(LockFuture&&) noexcept = default;
        LockFuture(const LockFuture&)                = delete;
        LockFuture& operator=(const LockFuture&)     = delete;

        PollResult<MutexGuard<T>> poll(detail::Context& ctx) {
            std::lock_guard lock(m_mutex->m_mutex);

            if (!m_mutex->m_locked) {
                // Fast path: mutex is free — acquire immediately.
                m_mutex->m_locked = true;
                return MutexGuard<T>(m_mutex, &m_mutex->m_value);
            }

            // Slow path: mutex is held — enqueue waker and suspend.
            // RACE CONDITION NOTE: the waker is enqueued inside m_mutex so
            // MutexGuard::~MutexGuard() cannot release the lock and drain
            // the waiter queue between our m_locked check and the enqueue,
            // which would produce a lost wakeup.
            m_mutex->m_waiters.push_back(ctx.getWaker());
            return PollPending;
        }

    private:
        Mutex<T>* m_mutex;
    };

    // -----------------------------------------------------------------------
    // Construction
    // -----------------------------------------------------------------------

    Mutex()                        = default;
    explicit Mutex(T value) : m_value(std::move(value)) {}

    Mutex(const Mutex&)            = delete;
    Mutex& operator=(const Mutex&) = delete;
    // Not movable: LockFuture and MutexGuard hold raw pointers to this object.
    Mutex(Mutex&&)                 = delete;
    Mutex& operator=(Mutex&&)      = delete;

    /**
     * @brief Acquires exclusive access to the protected value.
     *
     * @return A `LockFuture` that resolves to a `MutexGuard<T>` once the
     *         mutex is held by the calling task.
     */
    [[nodiscard]] LockFuture lock() noexcept { return LockFuture(this); }

private:
    friend class MutexGuard<T>;

    /// Called by MutexGuard<T>::~MutexGuard() to release the lock.
    void release() {
        std::shared_ptr<detail::Waker> next_waker;
        {
            std::lock_guard lock(m_mutex);
            if (m_waiters.empty()) {
                m_locked = false;
                return;
            }
            // FIFO: wake the oldest waiter. The mutex stays logically locked —
            // ownership transfers directly to the next waiter without ever
            // passing through m_locked = false, which would allow a concurrent
            // lock() call to jump the queue.
            next_waker = std::move(m_waiters.front());
            m_waiters.pop_front();
        }
        // Wake outside the lock to avoid the woken task re-entering release()
        // before we have released m_mutex, which on a multi-threaded executor
        // could cause a deadlock.
        next_waker->wake();
    }

    std::mutex                              m_mutex;
    T                                       m_value{};
    bool                                    m_locked = false;
    std::deque<std::shared_ptr<detail::Waker>> m_waiters;
};

// ---------------------------------------------------------------------------
// MutexGuard<T> destructor — defined after Mutex<T> is complete
// ---------------------------------------------------------------------------

template<typename T>
MutexGuard<T>::~MutexGuard() {
    if (m_mutex) m_mutex->release();
}

} // namespace coro
