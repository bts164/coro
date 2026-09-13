#pragma once

#ifdef CORO_PICO

namespace coro {

// Implemented by every ISR-safe primitive's waiter (IsrWaitFuture,
// IsrChannelWaitFuture<T>, IsrSemaphoreWaitFuture) and registered with the
// executor's ISR poll table. See doc/design/isr_safety.md, "Multiple
// waiters" for the full rationale.
//
// is_ready() is a non-mutating peek, called from the executor thread once
// per event loop iteration for every registered entry. It must never
// consume, decrement, or clear anything -- it only decides whether this
// entry's real waker is worth firing this tick. Deliberately not named
// `poll`: unlike Future::poll(Context&), which advances state and must not
// be called again after Ready, is_ready() is designed to be called
// repeatedly and changes nothing. The actual claim happens exactly once,
// inside the real Future::poll(), invoked only through the owning
// coroutine's own top-down traversal (Coro<T>::poll()'s spurious-wake
// guard) -- never from here.
//
// Lives in its own header (rather than sync/isr_event.h) so that
// runtime/runtime.h and runtime/current_thread_executor.h — which both need
// the complete type for register_isr_poll()/add_isr_poll()'s signatures —
// don't have to include isr_event.h, which depends on runtime.h in turn.
// The executor never needs to know anything about a specific Isr* primitive's
// internal state (a flag, a count, whatever a future one adds) beyond this
// one interface -- each waiter future reaches its own owning primitive's
// state directly (via friendship; see e.g. IsrWaitFuture/IsrEvent in
// sync/isr_event.h), so no shared "ref" type is needed here either.
class IsrPollEntry {
public:
    virtual ~IsrPollEntry() = default;

    virtual bool is_ready() const = 0;
};

} // namespace coro

#endif // CORO_PICO
