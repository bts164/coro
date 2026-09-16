#pragma once

#include <coro/coro.h>
#include <coro/detail/poll_result.h>
#include <coro/detail/context.h>
#include <coro/detail/isr_flag.h>
#include <coro/runtime/runtime.h>
#include <hardware/sync.h>
#include <cstdint>
#include <type_traits>
#ifdef CORO_DMA_DEBUG
#include <cstdio>
#endif

// ISR-to-coroutine communication primitives for MCU platforms.
//
// These are the ONLY coro types safe to call from an interrupt service routine.
// All other coro APIs (event::set, oneshot::send, mpsc::send, wake(), etc.) are
// undefined behavior from ISR context. See doc/design/isr_safety.md for a full
// audit, including why every flag/payload access below goes through a
// dedicated hardware spin lock instead of a bare `volatile` or `std::atomic`
// ("Cross-core ISR delivery").
//
// Only available under CORO_PICO — on desktop platforms there are no hardware
// ISRs; use the normal channels and events instead.

namespace coro {

// ---------------------------------------------------------------------------
// IsrEvent — level-triggered signal from ISR, no payload
// ---------------------------------------------------------------------------

class IsrWaitFuture; // defined below; friended by IsrEvent to reach its state directly

/**
 * @brief Reusable broadcast signal from ISR to coroutine.
 *
 * The ISR calls signal_from_isr() and any number of coroutines may
 * concurrently call wait(). signal_from_isr() wakes every wait() that was
 * already registered at the moment it's called -- i.e. every wait() whose
 * co_await was reached before this signal_from_isr(), regardless of which
 * of them the executor happens to resume first. A wait() registered after
 * signal_from_isr() has already run does NOT observe that signal; it parks
 * until the next one. Meant to be wait()-ed on repeatedly across the
 * instance's lifetime (see doc/design/isr_safety.md's "Multiple waiters").
 *
 * Implemented as a 64-bit generation counter (m_epoch), not a bool flag. Each
 * wait() call captures the current epoch when it starts (inside
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
    // epoch, releases. No payload to order, but the lock itself is what
    // makes this safe against an ISR firing on the *other* core, which a
    // bare volatile write is not. 64 bits rather than 32: comparisons only
    // ever test "has it changed", never magnitude or ordering, so wrapping
    // is harmless regardless of width, but a 32-bit counter can genuinely
    // wrap within a single long-lived instance's real uptime -- e.g. a
    // 1us-period signal wraps UINT32_MAX in ~71 minutes, well inside a
    // plausible reusable IsrEvent's lifetime. UINT64_MAX at the same rate is
    // ~584,000 years -- not a practical limitation. The extra width costs
    // nothing here: this field is only ever touched inside the same
    // spin-lock critical section as everything else in this class, never
    // read or written outside it, so there's no atomicity requirement being
    // traded away by widening it.
    void signal_from_isr() noexcept {
        uint32_t save = spin_lock_blocking(m_lock);
        ++m_epoch;
        spin_unlock(m_lock, save);
    }

    // Defined below IsrWaitFuture, which it constructs.
    [[nodiscard]] Coro<void> wait();

    // Non-blocking snapshot of the current epoch, for callers that must arm
    // hardware (e.g. gpio_set_irq_enabled()) and only start waiting some time
    // later -- capturing the baseline here, before arming, and passing it to
    // wait_from() below closes the gap that plain wait() cannot: wait()'s
    // baseline isn't captured until IsrWaitFuture's constructor runs, which
    // for a lazily-started Coro<void> is only once the caller actually
    // co_awaits it -- i.e. strictly after arming, not atomically with it. Any
    // edge landing in between would bump the epoch before that baseline is
    // read, so wait() would just fold it into its own baseline and never
    // resolve for it. See doc/design/isr_safety.md and gpio.cpp's
    // arm_and_wait() for the motivating bug.
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
    [[nodiscard]] Coro<void> wait_from(uint64_t baseline);

private:
    friend class IsrWaitFuture;

    spin_lock_t*       m_lock;
    uint64_t  m_epoch = 0;
};

// Internal — used by IsrEvent. IsrEvent and IsrWaitFuture sit at the same
// level (one owns the ISR-shared state, the other is its executor-facing
// waiter), so IsrWaitFuture reaches into IsrEvent's private m_epoch/m_lock
// directly via friendship rather than through an indirection struct.
//
// Implements IsrPollEntry (the executor's non-mutating is_ready() peek) and
// Future (poll(), called possibly many times via the owning coroutine's own
// top-down traversal -- see doc/design/isr_safety.md, "Multiple waiters").
// Unlike IsrChannelWaitFuture<T>/IsrSemaphoreWaitFuture, poll() here never
// mutates IsrEvent's state at all -- it only compares the current epoch to
// the baseline captured at construction, so it's safe to poll repeatedly
// with no claim semantics to worry about. Registers itself with the
// executor on first poll() and returns PollPending; the executor's
// check_isr_events() peeks is_ready() once per event loop iteration and,
// when it comes back true, fires the waker -- the future returns PollReady
// on the next poll() (or immediately, on the very first poll(), if the
// epoch had already changed by the time this future was constructed).
class IsrWaitFuture : public IsrPollEntry {
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

    ~IsrWaitFuture() {
        // If the awaiting coroutine is cancelled while we are registered with the
        // executor's ISR poll list, remove the entry. Without this the executor
        // would dereference a stale IsrPollEntry* into a potentially-destroyed
        // IsrEvent, and the waker would hold an unnecessary strong reference to
        // the task.
        if (m_registered)
            current_runtime().remove_isr_poll(this);
    }

    // Move transfers ownership of the registration; source must not deregister.
    // The executor's poll table is keyed by entry identity (this pointer), so
    // a moved-from/moved-to future must re-register at its new address rather
    // than simply copying m_registered -- see poll()/register_isr_poll below.
    IsrWaitFuture(IsrWaitFuture&& other) noexcept :
        m_event(other.m_event),
        m_baseline(other.m_baseline)
    {
        if (std::exchange(other.m_registered, false)) {
            current_runtime().remove_isr_poll(&other);
            current_runtime().register_isr_poll(this, other.m_waker);
            m_waker      = std::move(other.m_waker);
            m_registered = true;
        }
    }
    IsrWaitFuture& operator=(IsrWaitFuture&& other) noexcept {
        if (this != &other) {
            if (m_registered) current_runtime().remove_isr_poll(this);
            m_event = other.m_event;
            m_baseline = other.m_baseline;
            m_registered = false;
            if (std::exchange(other.m_registered, false)) {
                current_runtime().remove_isr_poll(&other);
                current_runtime().register_isr_poll(this, other.m_waker);
                m_waker      = std::move(other.m_waker);
                m_registered = true;
            }
        }
        return *this;
    }
    IsrWaitFuture(const IsrWaitFuture&)            = delete;
    IsrWaitFuture& operator=(const IsrWaitFuture&) = delete;

    // Non-mutating peek, called by the executor thread only -- see
    // IsrPollEntry's contract. Purely a comparison against m_baseline; there
    // is nothing here for poll() below to have exclusive claim over.
    bool is_ready() const override {
        uint32_t save = spin_lock_blocking(m_event->m_lock);
        bool ready = m_event->m_epoch != m_baseline;
        spin_unlock(m_event->m_lock, save);
        return ready;
    }

    PollResult<void> poll(detail::Context& ctx) {
        uint32_t save = spin_lock_blocking(m_event->m_lock);
        bool ready = m_event->m_epoch != m_baseline;
        spin_unlock(m_event->m_lock, save);
        if (ready) {
#ifdef CORO_DMA_DEBUG
            std::printf("[IsrEvent] epoch advanced past %llu — returning PollReady\n",
                        static_cast<unsigned long long>(m_baseline));
#endif
            return PollReady;
        }
        if (!m_registered) {
            m_waker = ctx.getWaker();
            current_runtime().register_isr_poll(this, m_waker);
            m_registered = true;
#ifdef CORO_DMA_DEBUG
            std::printf("[IsrEvent] baseline %llu registered with executor\n",
                        static_cast<unsigned long long>(m_baseline));
#endif
        }
        return PollPending;
    }

private:
    IsrEvent*                  m_event;
    uint64_t                   m_baseline;
    bool                       m_registered = false;
    detail::Rc<detail::Waker>  m_waker;
};

static_assert(Future<IsrWaitFuture>);

inline Coro<void> IsrEvent::wait() {
    // IsrWaitFuture's constructor captures the current epoch as this call's
    // baseline -- see IsrEvent's class comment for why that's what makes
    // repeated wait()s and concurrent broadcast waiters both correct with
    // no explicit reset step.
    co_await IsrWaitFuture{*this};
}

inline Coro<void> IsrEvent::wait_from(uint64_t baseline) {
    co_await IsrWaitFuture{*this, baseline};
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
 * critical section; the rest simply observe the flag already cleared and stay
 * pending. See doc/design/isr_safety.md, "IsrChannel<T>" and "Multiple waiters".
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
        spin_unlock(m_lock, save);
    }

    // Defined below IsrChannelWaitFuture<T>, which it constructs.
    [[nodiscard]] Coro<T> receive();

private:
    friend class IsrChannelWaitFuture<T>;

    spin_lock_t*  m_lock;
    bool m_flag = false;
    T             m_value{};
};

// Internal — used by IsrChannel<T>. Same shape as IsrWaitFuture, but poll()
// claims (clears the flag) and copies out m_value atomically in one locked
// critical section, so a spurious re-poll or a second concurrent waiter can
// never observe a torn or double-delivered value -- see
// doc/design/isr_safety.md, "IsrChannel<T>". Reaches into IsrChannel<T>'s
// private state directly via friendship -- see IsrWaitFuture's comment for
// why no indirection struct is needed.
template<typename T>
    requires std::is_trivially_copyable_v<T>
class IsrChannelWaitFuture : public IsrPollEntry {
public:
    using OutputType = T;

    explicit IsrChannelWaitFuture(IsrChannel<T>& channel) : m_channel(&channel) {}

    ~IsrChannelWaitFuture() {
        if (m_registered)
            current_runtime().remove_isr_poll(this);
    }

    IsrChannelWaitFuture(IsrChannelWaitFuture&& other) noexcept :
        m_channel(other.m_channel)
    {
        if (std::exchange(other.m_registered, false)) {
            current_runtime().remove_isr_poll(&other);
            current_runtime().register_isr_poll(this, other.m_waker);
            m_waker      = std::move(other.m_waker);
            m_registered = true;
        }
    }
    IsrChannelWaitFuture& operator=(IsrChannelWaitFuture&& other) noexcept {
        if (this != &other) {
            if (m_registered) current_runtime().remove_isr_poll(this);
            m_channel = other.m_channel;
            m_registered = false;
            if (std::exchange(other.m_registered, false)) {
                current_runtime().remove_isr_poll(&other);
                current_runtime().register_isr_poll(this, other.m_waker);
                m_waker      = std::move(other.m_waker);
                m_registered = true;
            }
        }
        return *this;
    }
    IsrChannelWaitFuture(const IsrChannelWaitFuture&)            = delete;
    IsrChannelWaitFuture& operator=(const IsrChannelWaitFuture&) = delete;

    // Non-mutating peek — never claims. See IsrWaitFuture::is_ready().
    bool is_ready() const override {
        uint32_t save = spin_lock_blocking(m_channel->m_lock);
        bool set = m_channel->m_flag;
        spin_unlock(m_channel->m_lock, save);
        return set;
    }

    PollResult<T> poll(detail::Context& ctx) {
        uint32_t save = spin_lock_blocking(m_channel->m_lock);
        bool set = std::exchange(m_channel->m_flag, false);
        T value = set ? m_channel->m_value : T{};
        spin_unlock(m_channel->m_lock, save);
        if (set)
            return value;
        if (!m_registered) {
            m_waker = ctx.getWaker();
            current_runtime().register_isr_poll(this, m_waker);
            m_registered = true;
        }
        return PollPending;
    }

private:
    IsrChannel<T>*             m_channel;
    bool                       m_registered = false;
    detail::Rc<detail::Waker>  m_waker;
};

template<typename T>
    requires std::is_trivially_copyable_v<T>
Coro<T> IsrChannel<T>::receive() {
    // The claim (check flag, copy value, clear flag) all happens inside
    // IsrChannelWaitFuture<T>::poll() -- one locked critical section, called
    // at most once per receive() (see doc/design/isr_safety.md, "Multiple
    // waiters" for why no explicit retry loop is needed here even under
    // concurrent receivers).
    co_return co_await IsrChannelWaitFuture<T>{*this};
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
 * decremented count and stay pending — same claim discipline as IsrChannel<T>.
 * Used by GpioPin::edges() to count edges without dropping any between polls.
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
        spin_unlock(m_lock, save);
    }

    // Defined below IsrSemaphoreWaitFuture, which it constructs.
    [[nodiscard]] Coro<void> acquire();

private:
    friend class IsrSemaphoreWaitFuture;

    spin_lock_t*       m_lock;
    uint32_t  m_count = 0;
};

// Internal — used by IsrSemaphore. Same structural shape as IsrWaitFuture, but
// claims by decrementing m_count (saturating at 0) instead of clearing a
// bool. Reaches into IsrSemaphore's private state directly via friendship --
// see IsrWaitFuture's comment for why no indirection struct is needed.
class IsrSemaphoreWaitFuture : public IsrPollEntry {
public:
    using OutputType = void;

    explicit IsrSemaphoreWaitFuture(IsrSemaphore& sem) : m_sem(&sem) {}

    ~IsrSemaphoreWaitFuture() {
        if (m_registered)
            current_runtime().remove_isr_poll(this);
    }

    IsrSemaphoreWaitFuture(IsrSemaphoreWaitFuture&& other) noexcept :
        m_sem(other.m_sem)
    {
        if (std::exchange(other.m_registered, false)) {
            current_runtime().remove_isr_poll(&other);
            current_runtime().register_isr_poll(this, other.m_waker);
            m_waker      = std::move(other.m_waker);
            m_registered = true;
        }
    }
    IsrSemaphoreWaitFuture& operator=(IsrSemaphoreWaitFuture&& other) noexcept {
        if (this != &other) {
            if (m_registered) current_runtime().remove_isr_poll(this);
            m_sem = other.m_sem;
            m_registered = false;
            if (std::exchange(other.m_registered, false)) {
                current_runtime().remove_isr_poll(&other);
                current_runtime().register_isr_poll(this, other.m_waker);
                m_waker      = std::move(other.m_waker);
                m_registered = true;
            }
        }
        return *this;
    }
    IsrSemaphoreWaitFuture(const IsrSemaphoreWaitFuture&)            = delete;
    IsrSemaphoreWaitFuture& operator=(const IsrSemaphoreWaitFuture&) = delete;

    // Non-mutating peek — never decrements. See IsrWaitFuture::is_ready().
    bool is_ready() const override {
        uint32_t save = spin_lock_blocking(m_sem->m_lock);
        bool set = m_sem->m_count != 0;
        spin_unlock(m_sem->m_lock, save);
        return set;
    }

    PollResult<void> poll(detail::Context& ctx) {
        uint32_t save = spin_lock_blocking(m_sem->m_lock);
        bool set = m_sem->m_count != 0;
        if (set)
            --(m_sem->m_count);
        spin_unlock(m_sem->m_lock, save);
        if (set)
            return PollReady;
        if (!m_registered) {
            m_waker = ctx.getWaker();
            current_runtime().register_isr_poll(this, m_waker);
            m_registered = true;
        }
        return PollPending;
    }

private:
    IsrSemaphore*                m_sem;
    bool                       m_registered = false;
    detail::Rc<detail::Waker>  m_waker;
};

static_assert(Future<IsrSemaphoreWaitFuture>);

inline Coro<void> IsrSemaphore::acquire() {
    // The claim-and-decrement happens inside IsrSemaphoreWaitFuture::poll() --
    // see doc/design/isr_safety.md, "Multiple waiters" for why no explicit
    // retry loop is needed here even under concurrent acquire()s.
    co_await IsrSemaphoreWaitFuture{*this};
}

} // namespace coro
