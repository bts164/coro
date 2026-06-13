#pragma once
#include <cstdint>

// Stub for Pico SDK <hardware/sync.h> used in Linux unit-test builds.
// On real RP2040 hardware these manipulate the PRIMASK register and the SIO
// spin lock registers. Here the executor is single-threaded with no second
// core or ISRs, so all operations are no-ops.

// __DMB() — Data Memory Barrier.
//
// On real ARM hardware (via Pico SDK + CMSIS headers) this expands to:
//   __asm__ volatile("dmb" ::: "memory")
//
// The "dmb" instruction drains the CPU write buffer so all stores before it
// are globally visible before any store after it. The "memory" clobber is the
// compiler barrier — it prevents GCC/Clang from reordering any memory access
// (volatile or not) across the instruction.
//
// On x86 (TSO), stores are never reordered past other stores at hardware level,
// so no fence instruction is needed. The compiler barrier alone (empty asm with
// "memory" clobber) is sufficient for correctness in unit tests.
#ifndef __DMB
#define __DMB() __asm__ volatile("" ::: "memory")
#endif

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
