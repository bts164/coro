#include <coro/detail/fiber_context.h>

#include <cstdint>
#include <new>

// alloc_fiber_stack()/free_fiber_stack() for the CORO_PICO (ARMv6-M) backend
// -- see doc/design/fiber.md's "Stack allocation and sizing" and "Stack
// overflow detection". Plain heap allocation (no mmap/mprotect -- Cortex-M0+
// has no MMU): the buffer is [cushion, with the canary word at its bottom]
// followed by the real working stack region. switch_context()
// (fiber_context_pico.S) checks a target's sp against [stack_low, stack_high]
// (the whole buffer, cushion included) and the canary word at stack_low
// before resuming it -- a modest overrun past the working region still lands
// inside the cushion and passes the range check, but clobbers the canary.
//
// This file is ordinary, target-independent C++ (no ARM-specific code), so
// test/task/test_fiber_context_pico_alloc.cpp compiles and exercises it
// directly on the host rather than needing Unicorn. FiberContext's
// stack_low/stack_high are uint32_t (matching the ARMv6-M target this backend
// actually ships on, where pointers are 32 bits); truncating a 64-bit host
// pointer into them is expected and harmless for that host-side unit test --
// it never calls the real switch_context() assembly, only alloc/free.

namespace coro::detail {

namespace {
constexpr uint32_t kStackCanary = 0xc0ffeeee; // must match STACK_CANARY in fiber_context_pico.S
constexpr uint32_t kFillPattern = 0xa5a5a5a5; // FreeRTOS-style paint, for offline high-water-mark profiling
constexpr size_t   kCushionBytes = 32;        // sacrificial cushion below the working range
constexpr size_t   kStackAlign   = 8;         // AAPCS requires 8-byte stack alignment at public interfaces
} // namespace

void alloc_fiber_stack(FiberContext& fc, size_t stack_size) {
    size_t total = kCushionBytes + stack_size;
    total = (total + kStackAlign - 1) & ~(kStackAlign - 1);

    auto* buffer = static_cast<std::byte*>(::operator new(total, std::align_val_t(kStackAlign)));

    auto* words = reinterpret_cast<uint32_t*>(buffer);
    for (size_t i = 0; i < total / sizeof(uint32_t); ++i) {
        words[i] = kFillPattern;
    }
    words[0] = kStackCanary; // bottom of the cushion, checked by switch_context()

    fc.buffer     = buffer;
    fc.stack_low  = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(buffer));
    fc.stack_high = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(buffer + total));
}

void free_fiber_stack(FiberContext& fc) noexcept {
    if (fc.buffer == nullptr) return;
    ::operator delete(fc.buffer, std::align_val_t(kStackAlign));
    fc.buffer     = nullptr;
    fc.stack_low  = 0;
    fc.stack_high = 0;
}

FiberContext::~FiberContext() { free_fiber_stack(*this); }

} // namespace coro::detail
