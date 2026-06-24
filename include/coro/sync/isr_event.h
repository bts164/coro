#pragma once

#include <coro/coro.h>
#include <coro/detail/poll_result.h>
#include <coro/detail/context.h>
#include <coro/detail/isr_flag.h>
#include <coro/runtime/runtime.h>
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

// Internal — used by IsrEvent and IsrChannel.
// Registers (ref, waker) with the executor on first poll() and returns
// PollPending. The executor checks the flag once per event loop iteration —
// through the paired spin lock, exactly like the ISR write — and when it
// observes the flag set it fires the waker. The future returns PollReady on
// the next poll().
class IsrWaitFuture {
public:
    using OutputType = void;

    explicit IsrWaitFuture(IsrFlagRef ref) : m_ref(ref) {}

    ~IsrWaitFuture() {
        // If the awaiting coroutine is cancelled while we are registered with the
        // executor's ISR poll list, remove the entry. Without this the executor
        // would dereference a flag pointer into a potentially-destroyed IsrEvent,
        // and the waker would hold an unnecessary strong reference to the task.
        if (m_registered)
            current_runtime().remove_isr_poll(m_ref);
    }

    // Move transfers ownership of the registration; source must not deregister.
    IsrWaitFuture(IsrWaitFuture&& other) noexcept
        : m_ref(other.m_ref), m_registered(other.m_registered) {
        other.m_registered = false;
    }
    IsrWaitFuture& operator=(IsrWaitFuture&& other) noexcept {
        if (this != &other) {
            if (m_registered) current_runtime().remove_isr_poll(m_ref);
            m_ref        = other.m_ref;
            m_registered = other.m_registered;
            other.m_registered = false;
        }
        return *this;
    }
    IsrWaitFuture(const IsrWaitFuture&)            = delete;
    IsrWaitFuture& operator=(const IsrWaitFuture&) = delete;

    PollResult<void> poll(detail::Context& ctx) {
        uint32_t save = spin_lock_blocking(m_ref.lock);
        bool set = *m_ref.flag;
        spin_unlock(m_ref.lock, save);
        if (set) {
#ifdef CORO_DMA_DEBUG
            std::printf("[IsrEvent] flag %p is SET — returning PollReady\n", (void*)m_ref.flag);
#endif
            return PollReady;
        }
        if (!m_registered) {
            current_runtime().register_isr_poll(m_ref, ctx.getWaker());
            m_registered = true;
#ifdef CORO_DMA_DEBUG
            std::printf("[IsrEvent] flag %p registered with executor\n", (void*)m_ref.flag);
#endif
        }
        return PollPending;
    }

private:
    IsrFlagRef m_ref;
    bool       m_registered = false;
};

static_assert(Future<IsrWaitFuture>);

// ---------------------------------------------------------------------------
// IsrEvent — level-triggered signal from ISR, no payload
// ---------------------------------------------------------------------------

/**
 * @brief One-shot event signal from ISR to coroutine.
 *
 * The ISR calls signal_from_isr() and the coroutine calls wait(), which parks
 * until the executor observes the flag and wakes the task.
 *
 * Limitations:
 * - Only one coroutine should call wait() at a time.
 * - A second signal_from_isr() before wait() returns is silently lost.
 * - signal_from_isr() called before wait() is entered is NOT lost — the
 *   next wait() observes it immediately via IsrWaitFuture::poll(). If that
 *   signal is actually stale (e.g. a level-triggered ISR firing on
 *   unrelated noise before the caller starts caring about it), call
 *   clear() first to discard it.
 */
class IsrEvent {
public:
    IsrEvent() : m_lock(spin_lock_instance(next_striped_spin_lock_num())) {}

    // ISR-safe. Takes the dedicated hardware spin lock — disables IRQs on
    // this core and spins on the SIO register for cross-core exclusion (see
    // doc/design/isr_safety.md, "Cross-core ISR delivery") — sets the flag,
    // releases. No payload to order, but the lock itself is what makes this
    // safe against an ISR firing on the *other* core, which a bare volatile
    // write is not.
    void signal_from_isr() noexcept {
        uint32_t save = spin_lock_blocking(m_lock);
        m_flag = true;
        spin_unlock(m_lock, save);
    }

    // Not ISR-safe — call only from the executor thread. Discards a pending
    // signal without waiting for it, so a later wait() observes only a
    // fresh signal rather than one left over from an unrelated earlier edge.
    void clear() noexcept {
        uint32_t save = spin_lock_blocking(m_lock);
        m_flag = false;
        spin_unlock(m_lock, save);
    }

    [[nodiscard]] Coro<void> wait() {
        // Do NOT clear m_flag here. If the IRQ fires between the caller setting
        // up the dispatch table and reaching this co_await, the flag is already
        // true and IsrWaitFuture::poll() will return PollReady immediately.
        // Clearing on entry would swallow that signal and park forever.
        // The clear at the end handles stale signals from previous transfers.
        co_await IsrWaitFuture{IsrFlagRef{&m_flag, m_lock}};
        uint32_t save = spin_lock_blocking(m_lock);
        m_flag = false;
        spin_unlock(m_lock, save);
    }

private:
    spin_lock_t*  m_lock;
    volatile bool m_flag = false;
};

// ---------------------------------------------------------------------------
// IsrChannel<T> — ISR-to-coroutine channel with a trivially-copyable payload
// ---------------------------------------------------------------------------

/**
 * @brief One-shot ISR-to-coroutine channel carrying a value of type T.
 *
 * T must be trivially copyable — no allocation, no constructor, no exception
 * in the ISR path.
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

    [[nodiscard]] Coro<T> receive() {
        co_await IsrWaitFuture{IsrFlagRef{&m_flag, m_lock}};
        uint32_t save = spin_lock_blocking(m_lock);
        T value = m_value;
        m_flag  = false;
        spin_unlock(m_lock, save);
        co_return value;
    }

private:
    spin_lock_t*  m_lock;
    volatile bool m_flag = false;
    T             m_value{};
};

} // namespace coro
