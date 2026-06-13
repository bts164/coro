#pragma once
#include <cstdint>

// Stub for Pico SDK <hardware/sync.h> used in Linux unit-test builds.
// On real RP2040 hardware these manipulate the PRIMASK register and the SIO
// spin lock registers. Here the executor is single-threaded with no second
// core or ISRs, so all operations are no-ops.

using spin_lock_t = volatile uint32_t;

inline uint32_t save_and_disable_interrupts()     { return 0; }
inline void     restore_interrupts(uint32_t)       {}

// A single dummy target so spin_lock_instance() never returns null.
inline spin_lock_t* spin_lock_instance(uint) {
    static spin_lock_t s_dummy = 0;
    return &s_dummy;
}

inline uint32_t spin_lock_blocking(spin_lock_t*)          { return 0; }
inline void     spin_unlock(spin_lock_t*, uint32_t)        {}
