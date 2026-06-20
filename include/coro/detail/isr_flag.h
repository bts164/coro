#pragma once

#ifdef CORO_PICO

#include <hardware/sync.h>

namespace coro {

// Identifies the (flag, spin lock) pair shared between an ISR/other core and
// the executor thread. The flag alone is not sufficient for cross-core safety
// — every access from either side must go through the paired hardware spin
// lock. See doc/design/isr_safety.md, "Cross-core ISR delivery".
//
// Lives in its own header (rather than sync/isr_event.h) so that
// runtime/runtime.h and runtime/current_thread_executor.h — which both need
// the complete type for register_isr_poll()/add_isr_poll()'s signatures —
// don't have to include isr_event.h, which depends on runtime.h in turn.
struct IsrFlagRef {
    volatile bool* flag;
    spin_lock_t*   lock;
};

} // namespace coro

#endif // CORO_PICO
