#pragma once
#include <cstdint>

// Stub for <pico/critical_section.h> used in Linux unit-test builds.
// On real RP2040 hardware, critical_section_enter_blocking() disables IRQs and
// acquires a hardware spin lock, making it safe for both ISR preemption and
// concurrent access from core 1. Here the executor is single-threaded with no
// second core or ISRs, so all operations are no-ops.

struct critical_section_t {
    uint32_t save;
};

inline void critical_section_init(critical_section_t*)               {}
inline void critical_section_enter_blocking(critical_section_t*)     {}
inline void critical_section_exit(critical_section_t*)               {}
inline void critical_section_deinit(critical_section_t*)             {}
