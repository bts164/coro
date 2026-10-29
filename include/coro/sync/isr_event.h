#pragma once

#include <coro/coro.h>
#include <coro/detail/poll_result.h>
#include <coro/detail/context.h>
#include <coro/detail/rc.h>
#include <coro/detail/sys/notify.h>
#include <coro/detail/sys/socket.h>
#include <coro/runtime/io_driver.h>
#include <coro/runtime/runtime.h>
#include <hardware/sync.h>
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <exception>
#include <system_error>
#include <type_traits>
#include <utility>

// ISR-to-coroutine communication primitives for MCU platforms.
//
// These are the ONLY coro types safe to call from an interrupt service routine.
// All other coro APIs (event::set, oneshot::send, mpsc::send, wake(), etc.) are
// undefined behavior from ISR context. See doc/design/isr_safety.md for a full
// audit, including why every flag/payload access below goes through a
// dedicated hardware spin lock instead of a bare `volatile` or `std::atomic`
// ("Cross-core ISR delivery").
//
// Each primitive is an I/O object on the runtime's IoDriver, like a socket: its
// signal function updates the primitive's state and then signals a notify fd (the
// driver's eventfd analog, detail/sys/notify.h), and its waiters wait on that fd's
// registration. See doc/design/isr_safety.md, "Waiting on the driver".
//
// Only available under CORO_PICO — on desktop platforms there are no hardware
// ISRs; use the normal channels and events instead.

namespace coro {

namespace detail {

/// A notify fd's registration with the driver, shared by every waiter of one ISR
/// primitive while it has any. See doc/design/isr_safety.md, "The wait".
struct NotifyRegistration {
    NotifyRegistration(IoDriver& driver, sys::RawFd fd)
        : reg(driver, fd, sys::Interest::read()) {}

    IoRegistration reg;
};

/**
 * @brief The notify fd an ISR primitive signals, and its registration while the
 * primitive has waiters. Embedded in every ISR primitive.
 *
 * The fd is opened by the first wait and closed with the primitive; the primitive
 * must already outlive every ISR that signals it. The registration lives only while
 * a waiter holds it: the first waiter registers the fd with the current runtime's
 * driver and the last one to leave deregisters it, so no registration outlives the
 * tasks, even for a global primitive.
 */
class IsrNotifier {
public:
    IsrNotifier() = default;
    IsrNotifier(const IsrNotifier&)            = delete;
    IsrNotifier& operator=(const IsrNotifier&) = delete;

    ~IsrNotifier() {
        // Every waiter is gone (they hold the primitive by reference), so the fd is
        // no longer registered.
        if (m_fd != sys::kInvalidFd) sys::close_socket(m_fd);
    }

    /// The fd to signal, or kInvalidFd before the first wait. The caller holds the
    /// primitive's spin lock, which orders it against acquire() publishing it.
    sys::RawFd fd_locked() const noexcept { return m_fd; }

    /// The registration for a wait: the existing one if a waiter holds it, otherwise
    /// a new one on the current runtime's driver. Executor thread only.
    /// @throws std::runtime_error outside a runtime, std::system_error or
    ///         std::bad_alloc if the fd cannot be opened or registered.
    Rc<NotifyRegistration> acquire(spin_lock_t* lock) {
        if (auto existing = m_registration.lock()) return existing;
        if (m_fd == sys::kInvalidFd) {
            const sys::RawFd fd = sys::notify_open();
            // Published under the primitive's lock, which signal functions read it
            // under. Before it is published a signal only updates the state, which
            // the waiter checks after registering (a new registration starts ready).
            uint32_t save = spin_lock_blocking(lock);
            m_fd = fd;
            spin_unlock(lock, save);
        }
        auto created = make_rc<NotifyRegistration>(current_runtime().io_driver(), m_fd);
        m_registration = Weak<NotifyRegistration>(created);
        return created;
    }

private:
    sys::RawFd               m_fd = sys::kInvalidFd;  // written under the primitive's lock
    Weak<NotifyRegistration> m_registration;          // executor thread only
};

/// Signals a primitive's notify fd, if it has one. The last step of every signal
/// function, after the primitive's spin lock is released: notify_signal() takes a
/// striped spin lock of its own (in sem_release()), and two striped locks may be the
/// same hardware lock, which is not recursive. See doc/design/isr_safety.md, "The
/// signal".
inline void isr_signal(sys::RawFd fd) noexcept {
    if (fd != sys::kInvalidFd) sys::notify_signal(fd);
}

/**
 * @brief The waiting half every ISR wait future shares: a share of the primitive's
 * registration and the future's own IoWaiter.
 *
 * WARNING: the future is pinned from its first poll (doc/design/future_and_stream.md):
 * its IoWaiter is linked into the driver's list by address. The move operations only
 * assert that it has not started; they never re-link.
 */
class IsrWaitCore {
public:
    IsrWaitCore() = default;
    IsrWaitCore([[maybe_unused]] IsrWaitCore&& other) noexcept {
        assert(!other.m_registration && "ISR wait future moved after its first poll()");
    }
    IsrWaitCore& operator=([[maybe_unused]] IsrWaitCore&& other) noexcept {
        assert(!m_registration && "ISR wait future assigned to after its first poll()");
        assert(!other.m_registration && "ISR wait future moved after its first poll()");
        return *this;
    }
    IsrWaitCore(const IsrWaitCore&)            = delete;
    IsrWaitCore& operator=(const IsrWaitCore&) = delete;

    ~IsrWaitCore() {
        if (m_registration) m_registration->reg.cancel_wait(m_waiter);
    }

    /**
     * @brief One poll of a wait. `claim()` takes the primitive's spin lock and
     * returns true if this waiter's wait is over (it compared the epoch, or claimed a
     * value or a count).
     *
     * `claim()` runs first, so a signal that arrived before the wait started is seen
     * without registering. Otherwise the waiter waits on the notify fd through the
     * driver's waiter list, and every wake runs `claim()` again.
     */
    template<typename Claim>
    PollResult<void> poll(detail::Context& ctx, IsrNotifier& notifier, spin_lock_t* lock,
                          Claim&& claim, const char* what) {
        if (!m_registration) {
            if (claim()) return PollReady;
            try {
                m_registration = notifier.acquire(lock);
            } catch (...) {
                return PollError(std::current_exception());
            }
        }
        // Race (handled): a signal between claim() above and the waiter being linked
        // cannot be dispatched in between (dispatch runs in the driver's turn, not in
        // a task poll), so its event finds the waiter linked. One that arrived before
        // the registration existed left a new registration ready, so poll_io() runs
        // claim() again before waiting.
        auto result = m_registration->reg.poll_io(IoDirection::Read, ctx, m_waiter,
            [&]() -> sys::IoResult {
                if (claim()) return std::size_t{0};
                return std::unexpected(EAGAIN);
            });
        if (!result) return PollPending;
        if (!*result)
            return PollError(std::make_exception_ptr(
                std::system_error(result->error(), std::generic_category(), what)));
        return PollReady;
    }

private:
    // Declared before m_waiter, so m_waiter (unlinked by the destructor above) is
    // destroyed first.
    Rc<NotifyRegistration> m_registration;
    IoWaiter               m_waiter;
};

} // namespace detail

// ---------------------------------------------------------------------------
// IsrEvent — level-triggered signal from ISR, no payload
// ---------------------------------------------------------------------------

class IsrWaitFuture; // defined below; friended by IsrEvent to reach its state directly

/**
 * @brief Reusable broadcast signal from ISR to coroutine.
 *
 * The ISR calls signal_from_isr() and any number of coroutines may
 * concurrently call wait(). signal_from_isr() wakes every wait() that was
 * already made at the moment it's called -- i.e. every wait() call that
 * returned its future before this signal_from_isr(), regardless of which
 * of them the executor happens to resume first. A wait() made after
 * signal_from_isr() has already run does NOT observe that signal; it parks
 * until the next one. Meant to be wait()-ed on repeatedly across the
 * instance's lifetime (see doc/design/isr_safety.md's "Multiple waiters").
 *
 * Implemented as a 64-bit generation counter (m_epoch), not a bool flag. Each
 * wait() call captures the current epoch as it is called (inside
 * IsrWaitFuture's constructor) and resolves the moment the epoch changes --
 * i.e. the next signal_from_isr() strictly after that wait() began. This is
 * deliberately NOT "wait() observes any signal still pending from before it
 * started" -- a bool-flag design that sounds equivalent turns out not to be:
 * with nothing to mark a signal as "already observed by every current
 * waiter," a later wait() call would immediately resolve on a stale signal
 * left over from an unrelated, long-past edge, with no way to tell the two
 * apart. The epoch model doesn't have that failure mode: every wait() is
 * unambiguously scoped to "the next signal after I started waiting," for
 * every waiter, without any explicit reset step.
 *
 * Must outlive every waiter and every ISR that signals it.
 *
 * Limitations:
 * - A second signal_from_isr() before every waiter has observed the first is
 *   silently folded into it (not queued) -- see doc/design/isr_safety.md's
 *   "Limitations".
 */
class IsrEvent {
public:
    IsrEvent() : m_lock(spin_lock_instance(next_striped_spin_lock_num())) {}

    // ISR-safe. Takes the dedicated hardware spin lock — disables IRQs on
    // this core and spins on the SIO register for cross-core exclusion (see
    // doc/design/isr_safety.md, "Cross-core ISR delivery") — bumps the
    // epoch, releases, then signals the notify fd (see "The signal"). 64 bits
    // rather than 32: comparisons only ever test "has it changed", never
    // magnitude or ordering, so wrapping is harmless regardless of width, but a
    // 32-bit counter can genuinely wrap within a single long-lived instance's
    // real uptime -- e.g. a 1us-period signal wraps UINT32_MAX in ~71 minutes,
    // well inside a plausible reusable IsrEvent's lifetime. UINT64_MAX at the
    // same rate is ~584,000 years -- not a practical limitation. The extra width
    // costs nothing here: this field is only ever touched inside the same
    // spin-lock critical section as everything else in this class.
    void signal_from_isr() noexcept {
        uint32_t save = spin_lock_blocking(m_lock);
        ++m_epoch;
        const detail::sys::RawFd notify = m_notifier.fd_locked();
        spin_unlock(m_lock, save);
        detail::isr_signal(notify);
    }

    // Returns a future that resolves on the first signal_from_isr() after
    // this call. The baseline epoch is captured here, as wait() is called,
    // not when the future is first polled.
    //
    // Returns the hand-written IsrWaitFuture itself and not a Coro wrapping
    // it, so a wait has no coroutine frame. Defined below IsrWaitFuture, which
    // it constructs.
    [[nodiscard]] IsrWaitFuture wait();

    // Non-blocking snapshot of the current epoch, for callers that arm
    // hardware (e.g. gpio_set_irq_enabled()) in one place and only call
    // wait() some time later, e.g. from a separate function. An edge that
    // lands in between bumps the epoch before wait() reads its baseline, so
    // wait() would fold it into that baseline and never resolve for it.
    // Capturing the baseline here, before arming, and passing it to
    // wait_from() below closes that gap. See doc/design/isr_safety.md and
    // AsyncDmaTransfer::start()/wait() for the motivating case.
    [[nodiscard]] uint64_t epoch() const noexcept {
        uint32_t save = spin_lock_blocking(m_lock);
        uint64_t e = m_epoch;
        spin_unlock(m_lock, save);
        return e;
    }

    // Like wait(), but resolves on the first signal_from_isr() strictly after
    // `baseline` (as returned by epoch()) rather than after this call itself.
    // Use this together with epoch() to snapshot the baseline before arming
    // hardware that might signal before wait() is reached.
    [[nodiscard]] IsrWaitFuture wait_from(uint64_t baseline);

private:
    friend class IsrWaitFuture;

    spin_lock_t*        m_lock;
    uint64_t            m_epoch = 0;
    detail::IsrNotifier m_notifier;
};

// Internal — used by IsrEvent. IsrEvent and IsrWaitFuture sit at the same
// level (one owns the ISR-shared state, the other is its executor-facing
// waiter), so IsrWaitFuture reaches into IsrEvent's private m_epoch/m_lock
// directly via friendship rather than through an indirection struct.
//
// Its check never mutates IsrEvent's state -- it only compares the current epoch
// to the baseline captured at construction -- so every concurrent waiter resolves
// on the same signal (broadcast). It waits on the event's notify fd through the
// driver's waiter list; see doc/design/isr_safety.md, "Multiple waiters".
class IsrWaitFuture {
public:
    using OutputType = void;

    // Captures the current epoch as this wait's baseline -- resolves on the
    // first signal_from_isr() strictly after this point, never on one that
    // already happened before construction. See IsrEvent's class comment.
    explicit IsrWaitFuture(IsrEvent& event) :
        m_event(&event)
    {
        uint32_t save = spin_lock_blocking(m_event->m_lock);
        m_baseline = m_event->m_epoch;
        spin_unlock(m_event->m_lock, save);
    }

    // Resolves on the first signal_from_isr() strictly after `baseline` (an
    // epoch value the caller captured earlier via IsrEvent::epoch()), rather
    // than capturing a fresh baseline here. See IsrEvent::wait_from().
    IsrWaitFuture(IsrEvent& event, uint64_t baseline) :
        m_event(&event), m_baseline(baseline)
    {}

    // Moves only before the first poll(): a future is pinned once polled
    // (doc/design/future_and_stream.md, "Movable until the first poll, pinned
    // after"), and only poll() registers, so there is never a registration to
    // carry over. IsrWaitCore's move operations assert that contract instead of
    // re-registering at the new address, which no caller can ever need.
    IsrWaitFuture(IsrWaitFuture&&) noexcept            = default;
    IsrWaitFuture& operator=(IsrWaitFuture&&) noexcept = default;
    IsrWaitFuture(const IsrWaitFuture&)            = delete;
    IsrWaitFuture& operator=(const IsrWaitFuture&) = delete;

    PollResult<void> poll(detail::Context& ctx) {
        return m_core.poll(ctx, m_event->m_notifier, m_event->m_lock, [this] {
            uint32_t save = spin_lock_blocking(m_event->m_lock);
            const bool ready = m_event->m_epoch != m_baseline;
            spin_unlock(m_event->m_lock, save);
            return ready;
        }, "IsrEvent::wait");
    }

private:
    IsrEvent*           m_event;
    uint64_t            m_baseline = 0;
    detail::IsrWaitCore m_core;
};

static_assert(Future<IsrWaitFuture>);

inline IsrWaitFuture IsrEvent::wait() {
    // IsrWaitFuture's constructor captures the current epoch as this call's
    // baseline -- see IsrEvent's class comment for why that's what makes
    // repeated wait()s and concurrent broadcast waiters both correct with
    // no explicit reset step.
    return IsrWaitFuture{*this};
}

inline IsrWaitFuture IsrEvent::wait_from(uint64_t baseline) {
    return IsrWaitFuture{*this, baseline};
}

// ---------------------------------------------------------------------------
// IsrChannel<T> — ISR-to-coroutine channel with a trivially-copyable payload
// ---------------------------------------------------------------------------

template<typename T>
    requires std::is_trivially_copyable_v<T>
class IsrChannelWaitFuture; // defined below; friended by IsrChannel<T>

/**
 * @brief Reusable single-slot ISR-to-coroutine channel, one value claimed per send.
 *
 * T must be trivially copyable — no allocation, no constructor, no exception
 * in the ISR path. Each send_from_isr() claim is delivered to exactly one
 * receive() — under concurrent receive()s, whichever one's poll() runs first
 * claims the value (clears the flag and copies it out) inside a single locked
 * critical section; the rest simply observe the flag already cleared and wait
 * again. See doc/design/isr_safety.md, "IsrChannel<T>" and "Multiple waiters".
 *
 * Must outlive every waiter and every ISR that sends on it.
 */
template<typename T>
    requires std::is_trivially_copyable_v<T>
class IsrChannel {
public:
    IsrChannel() : m_lock(spin_lock_instance(next_striped_spin_lock_num())) {}

    // ISR-safe. m_value and m_flag are written inside the same spin-lock
    // critical section, so the lock's own acquire/release ordering — the same
    // ordering pico-sdk's queue_t relies on — guarantees the receiver never
    // observes a partially-written value. The lock also provides the
    // cross-core exclusion a manual barrier never did (see
    // doc/design/isr_safety.md, "Cross-core ISR delivery").
    void send_from_isr(T value) noexcept {
        uint32_t save = spin_lock_blocking(m_lock);
        m_value = value;
        m_flag  = true;
        const detail::sys::RawFd notify = m_notifier.fd_locked();
        spin_unlock(m_lock, save);
        detail::isr_signal(notify);
    }

    // Returns a future for the next value. A hand-written future and not a
    // Coro, so a receive has no coroutine frame.
    // Defined below IsrChannelWaitFuture<T>, which it constructs.
    [[nodiscard]] IsrChannelWaitFuture<T> receive();

private:
    friend class IsrChannelWaitFuture<T>;

    spin_lock_t*        m_lock;
    bool                m_flag = false;
    T                   m_value{};
    detail::IsrNotifier m_notifier;
};

// Internal — used by IsrChannel<T>. Same shape as IsrWaitFuture, but its check
// claims (clears the flag) and copies out m_value atomically in one locked
// critical section, so a spurious re-poll or a second concurrent waiter can
// never observe a torn or double-delivered value -- see
// doc/design/isr_safety.md, "IsrChannel<T>". Reaches into IsrChannel<T>'s
// private state directly via friendship -- see IsrWaitFuture's comment for
// why no indirection struct is needed.
template<typename T>
    requires std::is_trivially_copyable_v<T>
class IsrChannelWaitFuture {
public:
    using OutputType = T;

    explicit IsrChannelWaitFuture(IsrChannel<T>& channel) : m_channel(&channel) {}

    // Moves only before the first poll(); see IsrWaitFuture's move constructor.
    IsrChannelWaitFuture(IsrChannelWaitFuture&&) noexcept            = default;
    IsrChannelWaitFuture& operator=(IsrChannelWaitFuture&&) noexcept = default;
    IsrChannelWaitFuture(const IsrChannelWaitFuture&)            = delete;
    IsrChannelWaitFuture& operator=(const IsrChannelWaitFuture&) = delete;

    PollResult<T> poll(detail::Context& ctx) {
        T value{};
        auto result = m_core.poll(ctx, m_channel->m_notifier, m_channel->m_lock, [&] {
            uint32_t save = spin_lock_blocking(m_channel->m_lock);
            const bool set = std::exchange(m_channel->m_flag, false);
            if (set) value = m_channel->m_value;
            spin_unlock(m_channel->m_lock, save);
            return set;
        }, "IsrChannel::receive");
        if (result.isPending()) return PollPending;
        if (result.isError())   return PollError(result.error());
        return value;
    }

private:
    IsrChannel<T>*      m_channel;
    detail::IsrWaitCore m_core;
};

template<typename T>
    requires std::is_trivially_copyable_v<T>
IsrChannelWaitFuture<T> IsrChannel<T>::receive() {
    // The claim (check flag, copy value, clear flag) all happens inside
    // IsrChannelWaitFuture<T>::poll() -- one locked critical section. A poll()
    // that finds the flag clear waits again, so no explicit retry loop is
    // needed here even under concurrent receivers (see
    // doc/design/isr_safety.md, "Multiple waiters").
    return IsrChannelWaitFuture<T>{*this};
}

// ---------------------------------------------------------------------------
// IsrSemaphore — ISR-to-coroutine saturating counting semaphore
// ---------------------------------------------------------------------------

class IsrSemaphoreWaitFuture; // defined below; friended by IsrSemaphore

/**
 * @brief Reusable ISR-to-coroutine saturating counting semaphore.
 *
 * The ISR calls release_from_isr() to increment the count (saturating at
 * UINT32_MAX — a release that would overflow is dropped rather than wrapping).
 * Each acquire() claims one count by decrementing it; under concurrent
 * acquire()s, whichever one's poll() runs first claims, the rest observe the
 * decremented count and wait again — same claim discipline as IsrChannel<T>.
 * Used by GpioPin::edges() to count edges without dropping any between polls.
 *
 * Must outlive every waiter and every ISR that releases it.
 */
class IsrSemaphore {
public:
    IsrSemaphore() : m_lock(spin_lock_instance(next_striped_spin_lock_num())) {}

    // ISR-safe. Saturates at UINT32_MAX rather than wrapping to 0, since a
    // silent wrap would look identical to "no events occurred" to acquire().
    void release_from_isr() noexcept {
        uint32_t save = spin_lock_blocking(m_lock);
        if (m_count != UINT32_MAX)
            ++m_count;
        const detail::sys::RawFd notify = m_notifier.fd_locked();
        spin_unlock(m_lock, save);
        detail::isr_signal(notify);
    }

    // Returns a future that claims one count. A hand-written future and not
    // a Coro, so an acquire has no coroutine frame.
    // Defined below IsrSemaphoreWaitFuture, which it constructs.
    [[nodiscard]] IsrSemaphoreWaitFuture acquire();

private:
    friend class IsrSemaphoreWaitFuture;

    spin_lock_t*        m_lock;
    uint32_t            m_count = 0;
    detail::IsrNotifier m_notifier;
};

// Internal — used by IsrSemaphore. Same structural shape as IsrWaitFuture, but
// claims by decrementing m_count instead of comparing an epoch. Reaches into
// IsrSemaphore's private state directly via friendship -- see IsrWaitFuture's
// comment for why no indirection struct is needed.
class IsrSemaphoreWaitFuture {
public:
    using OutputType = void;

    explicit IsrSemaphoreWaitFuture(IsrSemaphore& sem) : m_sem(&sem) {}

    // Moves only before the first poll(); see IsrWaitFuture's move constructor.
    IsrSemaphoreWaitFuture(IsrSemaphoreWaitFuture&&) noexcept            = default;
    IsrSemaphoreWaitFuture& operator=(IsrSemaphoreWaitFuture&&) noexcept = default;
    IsrSemaphoreWaitFuture(const IsrSemaphoreWaitFuture&)            = delete;
    IsrSemaphoreWaitFuture& operator=(const IsrSemaphoreWaitFuture&) = delete;

    PollResult<void> poll(detail::Context& ctx) {
        return m_core.poll(ctx, m_sem->m_notifier, m_sem->m_lock, [this] {
            uint32_t save = spin_lock_blocking(m_sem->m_lock);
            const bool set = m_sem->m_count != 0;
            if (set) --(m_sem->m_count);
            spin_unlock(m_sem->m_lock, save);
            return set;
        }, "IsrSemaphore::acquire");
    }

private:
    IsrSemaphore*       m_sem;
    detail::IsrWaitCore m_core;
};

static_assert(Future<IsrSemaphoreWaitFuture>);

inline IsrSemaphoreWaitFuture IsrSemaphore::acquire() {
    // The claim-and-decrement happens inside IsrSemaphoreWaitFuture::poll() --
    // see doc/design/isr_safety.md, "Multiple waiters" for why no explicit
    // retry loop is needed here even under concurrent acquire()s.
    return IsrSemaphoreWaitFuture{*this};
}

} // namespace coro
