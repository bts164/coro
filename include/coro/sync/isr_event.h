#pragma once

#ifdef CORO_PICO

#include <coro/coro.h>
#include <coro/detail/poll_result.h>
#include <coro/detail/context.h>
#include <coro/runtime/runtime.h>
#include <hardware/sync.h>  // __DMB(): inline dmb on ARM, compiler-barrier stub on desktop
#include <type_traits>

// ISR-to-coroutine communication primitives for MCU platforms.
//
// These are the ONLY coro types safe to call from an interrupt service routine.
// All other coro APIs (event::set, oneshot::send, mpsc::send, wake(), etc.) are
// undefined behavior from ISR context. See doc/isr_safety.md for a full audit.
//
// Only available under CORO_PICO — on desktop platforms there are no hardware
// ISRs; use the normal channels and events instead.

namespace coro {

// Internal — used by IsrEvent and IsrChannel.
// Registers (flag, waker) with the executor on first poll() and returns
// PollPending. The executor checks the flag once per event loop iteration;
// when it observes the flag set it fires the waker. The future returns
// PollReady on the next poll().
class IsrWaitFuture {
public:
    using OutputType = void;

    explicit IsrWaitFuture(const volatile bool* flag) : m_flag(flag) {}

    PollResult<void> poll(detail::Context& ctx) {
        if (*m_flag)
            return PollReady;
        if (!m_registered) {
            current_runtime().register_isr_poll(m_flag, ctx.getWaker());
            m_registered = true;
        }
        return PollPending;
    }

private:
    const volatile bool* m_flag;
    bool                 m_registered = false;
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
 * - Do not call signal_from_isr() before wait() is entered — wait() clears
 *   any stale signal on entry.
 */
class IsrEvent {
public:
    // ISR-safe. Single volatile write → one STRB → naturally atomic on ARM.
    // No memory barrier needed: there is no payload to order.
    void signal_from_isr() noexcept { m_flag = true; }

    [[nodiscard]] Coro<void> wait() {
        m_flag = false;
        co_await IsrWaitFuture{&m_flag};
        m_flag = false;
    }

private:
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
    // ISR-safe.
    //
    // __DMB() provides release semantics for m_value: the "memory" clobber
    // acts as a compiler barrier preventing the compiler from reordering the
    // m_value store past the m_flag store, and the dmb instruction drains the
    // write buffer so m_value is globally visible before m_flag is written.
    // This pairs with the __DMB() acquire in receive().
    void send_from_isr(T value) noexcept {
        m_value = value;
        __DMB();
        m_flag = true;
    }

    // __DMB() provides acquire semantics for m_value: the "memory" clobber
    // prevents the compiler from hoisting the m_value load above the point
    // where m_flag was observed true, and the dmb instruction ensures any
    // hardware write-buffer effects from the sender are visible before the
    // load executes. This pairs with the __DMB() release in send_from_isr().
    [[nodiscard]] Coro<T> receive() {
        co_await IsrWaitFuture{&m_flag};
        __DMB();
        T value = m_value;
        m_flag = false;
        co_return value;
    }

private:
    volatile bool m_flag = false;
    T             m_value{};
};

} // namespace coro

#endif // CORO_PICO
