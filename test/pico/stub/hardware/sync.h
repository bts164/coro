#pragma once
#include <cstdint>
#include <mutex>

// Stub for Pico SDK <hardware/sync.h> used in Linux unit-test builds.
//
// On real RP2040 hardware, spin_lock_blocking()/spin_unlock() disable IRQs on
// the local core (cpsid i / cpsie i) and spin on a genuine SIO hardware
// spin-lock register, giving mutual exclusion against both same-core ISR
// preemption AND a second core (see pico-sdk's hardware/sync/spin_lock.h and
// doc/design/isr_safety.md, "Cross-core ISR delivery").
//
// The host build has no real IRQs and no second core, but
// test/sync/test_isr_event.cpp simulates an ISR with a genuine std::thread —
// which IS truly concurrent under the C++ memory model, unlike a real
// single-core interrupt. Backing the stub spin lock with a real std::mutex
// gives the host build the same mutual-exclusion guarantee the SIO register
// gives real hardware, so isr_event.h's locking is correct (and TSan-clean)
// on both targets with no #ifdef in isr_event.h itself.
//
// A std::mutex would NOT be safe to take from a real ISR (it can block) —
// this stub is only valid because the host's simulated "ISR" is an ordinary
// thread that's allowed to block, not a real interrupt handler.

using spin_lock_t = std::mutex;

inline uint32_t save_and_disable_interrupts()           { return 0; }
inline void     restore_interrupts(uint32_t)              {}
inline void     restore_interrupts_from_disabled(uint32_t) {}

// Striped-pool allocator stub. Real hardware has 32 independent spin-lock
// registers; the host build has no contention to model, so every caller is
// handed the same dummy lock instance below — correct (if more conservative
// than necessary) since std::mutex critical sections here are as brief as
// their hardware counterparts.
inline uint32_t next_striped_spin_lock_num() { return 0; }

inline spin_lock_t* spin_lock_instance(uint32_t) {
    static spin_lock_t s_dummy;
    return &s_dummy;
}

inline uint32_t spin_lock_blocking(spin_lock_t* lock) {
    lock->lock();
    return 0;
}

inline void spin_unlock(spin_lock_t* lock, uint32_t /*saved_irq*/) {
    lock->unlock();
}
