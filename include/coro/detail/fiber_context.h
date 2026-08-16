#pragma once

// FiberContext / switch_context() -- see doc/design/fiber.md's "Design"
// section. Two backends, selected by CORO_PICO:
//  - POSIX/desktop: backed by ucontext.h (getcontext/makecontext/swapcontext).
//  - ARMv6-M/CORO_PICO: hand-written assembly, src/detail/fiber_context_pico.S
//    (register swap) + src/detail/fiber_context_pico.cpp (stack allocation).

#include <cstddef>

#ifdef CORO_PICO
#include <cstdint>
#else
#include <ucontext.h>
#endif

namespace coro::detail {

#ifdef CORO_PICO

/**
 * @brief One stackful execution context on the ARMv6-M (CORO_PICO) backend:
 * a saved stack pointer plus the `[stack_low, stack_high]` range
 * switch_context() checks a target's `sp` against before resuming it (see
 * doc/design/fiber.md's "Stack overflow detection"). `stack_low` is the
 * address of the canary word at the very bottom of the allocated buffer
 * (the sacrificial cushion below the real working range), not the bottom of
 * the working range itself -- a modest overrun past the working range still
 * passes this range check but is caught separately by the canary check.
 *
 * fiber_context_pico.S accesses `sp`/`stack_low`/`stack_high` by fixed byte
 * offset (0/4/8) rather than through this header (it's hand-assembled, not
 * compiled) -- the static_asserts below make sure this struct's layout can't
 * silently drift out of sync with that assembly.
 *
 * A default-constructed FiberContext (e.g. the caller side of a switch,
 * which never runs on its own dedicated stack -- see FiberFuture) has
 * `buffer == nullptr` and alloc_fiber_stack() is never called on it.
 *
 * Move-only: moving transfers stack ownership and nulls out the source's
 * `buffer` so only one side ever frees it. The destructor frees whatever
 * stack it owns (RAII) -- allocation stays a separate call (alloc_fiber_stack()
 * is deliberately lazy, deferred to FiberFuture's first poll() rather than
 * tied to construction, and can throw), but ownership release doesn't need
 * to wait for that: whichever FiberContext holds a non-null `buffer` when it
 * goes out of scope frees it, the same lazy-acquire/RAII-release shape as
 * std::unique_ptr.
 *
 * TODO: alloc_fiber_stack()/free_fiber_stack() are still free functions
 * rather than members mostly as a matter of style; consider folding them
 * into FiberContext itself (and making the fields private) at some point.
 */
struct FiberContext {
    uint32_t   sp         = 0;
    uint32_t   stack_low  = 0; // address of the canary word (bottom of the allocated buffer)
    uint32_t   stack_high = 0; // one past the top of the stack (the stack_top init_fiber_entry() takes)
    std::byte* buffer     = nullptr; // owning allocation; null if no stack owned

    FiberContext() = default;
    FiberContext(const FiberContext&)            = delete;
    FiberContext& operator=(const FiberContext&) = delete;

    FiberContext(FiberContext&& other) noexcept
        : sp(other.sp), stack_low(other.stack_low), stack_high(other.stack_high), buffer(other.buffer) {
        other.buffer = nullptr;
    }

    FiberContext& operator=(FiberContext&& other) noexcept {
        if (this == &other) return *this;
        sp         = other.sp;
        stack_low  = other.stack_low;
        stack_high = other.stack_high;
        buffer     = other.buffer;
        other.buffer = nullptr;
        return *this;
    }

    // Frees whatever stack this context owns, if any -- see free_fiber_stack()
    // below. Safe on a context that never had a stack allocated.
    ~FiberContext();
};

static_assert(offsetof(FiberContext, sp) == 0);
static_assert(offsetof(FiberContext, stack_low) == 4);
static_assert(offsetof(FiberContext, stack_high) == 8);

/// @brief Allocates a stack buffer for `fc` sized `stack_size` plus a small
/// sacrificial cushion below it, paints the whole buffer with a fill pattern
/// (for offline high-water-mark profiling -- see doc/design/fiber.md's
/// "Stack allocation and sizing"), and writes a canary word at the very
/// bottom of the cushion for switch_context() to check on every resume.
/// Throws std::bad_alloc on failure.
void alloc_fiber_stack(FiberContext& fc, size_t stack_size);

/// @brief Frees the stack buffer owned by `fc`, if any. Safe to call on a
/// FiberContext that never had a stack allocated (e.g. a caller-side context).
void free_fiber_stack(FiberContext& fc) noexcept;

/// @brief Raw ARM primitive (src/detail/fiber_context_pico.S) — builds the fake
/// resume frame at the top of `fc`'s stack (`stack_top` down), pointed at `entry`.
/// Named "_raw" and takes `stack_top` explicitly (rather than reading `fc->stack_high`
/// itself) because hand-written assembly can't dereference the struct through the
/// header; the inline `init_fiber_entry()` below wraps it so call sites (fiber.h)
/// stay identical across both backends.
extern "C" void coro_fiber_init_entry_raw(FiberContext* fc, uint32_t stack_top, void (*entry)());

/// @brief Finishes preparing `fc` (which must already have a stack via
/// alloc_fiber_stack()) to start executing `entry` the first time it is switched
/// into. `entry` takes no arguments — see FiberFuture::trampoline()'s use of the
/// t_fiber_start_arg thread-local to recover per-instance state.
inline void init_fiber_entry(FiberContext& fc, void (*entry)()) {
    coro_fiber_init_entry_raw(&fc, fc.stack_high, entry);
}

/// @brief Saves the caller's context into `from` and resumes `to` where it last left
/// off (or starts it, if this is its first switch since init_fiber_entry()). Checks
/// `to->sp` against `[to->stack_low, to->stack_high]` and `to`'s canary word before
/// resuming it, trapping via coro_fiber_stack_overflow() (see fiber_context_pico.S)
/// if either check fails -- see doc/design/fiber.md's "Stack overflow detection".
extern "C" void switch_context(FiberContext* from, FiberContext* to);

/// @brief No-op on this backend -- the ASan handshake this exists for is a POSIX/
/// ucontext concern only. Safe to call unconditionally.
inline void notify_fiber_switch_finished() {}

/// @brief Raw ARM primitive (src/detail/fiber_context_pico.S) -- performs the
/// CONTROL/PSP switch, relocating MSP to `isr_stack_top` (which must not
/// overlap the caller's own stack, now owned by PSP) so the two never alias.
/// See that file's big comment at the definition for why aliasing MSP/PSP
/// froze real hardware; coro_pico_enable_psp_stack() below is the wrapper
/// that owns the dedicated buffer and is what call sites should actually use.
extern "C" void coro_pico_enable_psp_stack_raw(uint32_t isr_stack_top);

/// @brief One-time CONTROL/PSP switch -- see doc/design/fiber.md's "Stack model
/// (Pico backend)". Must be called exactly once, from the executor's own thread,
/// before the first switch_context() call, and never afterward; the only call
/// site is CurrentThreadExecutor::wait_for_completion(). Not idempotent by
/// contract -- see fiber_context_pico.S for why calling it twice is harmless in
/// practice but not something to rely on.
///
/// Owns a static, dedicated buffer for the post-switch MSP (ISR-only) stack --
/// inline, not extern "C", specifically so this buffer lives here rather than
/// needing a separate translation unit; "inline function, static local" gives
/// every call site the same one instance, same as a function-local singleton.
/// kIsrStackWords was originally 512 words (2KB), the size that survived the
/// on-hardware bisection in fiber_context_pico.S's big comment -- but that
/// bisection predates any program with multiple *chained* shared IRQ handlers
/// that can genuinely nest on this one buffer (e.g. a DMA-completion IRQ
/// firing while a GPIO IRQ's handler -- itself one link in a multi-handler
/// chain on IO_IRQ_BANK0 -- is still running). Bumped to 2048 words (8KB)
/// while chasing an intermittent whole-system freeze correlated with input
/// events; that freeze turned out to be an unrelated reentrancy bug in a
/// third-party library (a fiber calling into that library while a
/// render/flush cycle was in progress on a different fiber), so this bump
/// did not fix it and was never actually load-bearing. Kept at 8KB anyway
/// as harmless extra headroom for the genuine nested-IRQ scenario above.
/// This is exception-handler stack only -- no coroutine/fiber body ever
/// runs on it -- so it does not need to scale with fiber stack sizing.
inline void coro_pico_enable_psp_stack() {
    constexpr std::size_t kIsrStackWords = 2048; // 8KB
    alignas(8) static uint32_t isr_stack[kIsrStackWords];
    // Two-step cast (not a direct reinterpret_cast<uint32_t>) because this
    // header, like fiber_context_pico.cpp, is also compiled on the host as
    // part of coro_pico_core for the CurrentThreadExecutor tests -- pointers
    // are 64-bit there, and a direct pointer-to-uint32_t reinterpret_cast is
    // an error (loses precision), not just a warning. The truncation is only
    // ever exercised on real ARMv6-M, where pointers are natively 32-bit.
    coro_pico_enable_psp_stack_raw(
        static_cast<uint32_t>(reinterpret_cast<uintptr_t>(isr_stack + kIsrStackWords)));
}

#else

/**
 * @brief One stackful execution context: a dedicated stack plus enough saved
 * state (here, a ucontext_t) to resume execution in it later.
 *
 * `stack_base`/`stack_size` describe the *usable* stack region — the guard
 * page `alloc_fiber_stack()` places below it is not included. A default-
 * constructed `FiberContext` with no stack (e.g. the caller side of a
 * switch, which never runs on its own dedicated stack — see `FiberFuture`)
 * has `mmap_base == nullptr` and `alloc_fiber_stack()` is never called on it.
 *
 * Move-only: moving transfers stack ownership and nulls out the source's
 * `mmap_base` so only one side ever unmaps it. The destructor frees whatever
 * stack it owns (RAII) -- allocation stays a separate call (alloc_fiber_stack()
 * is deliberately lazy, deferred to FiberFuture's first poll() rather than
 * tied to construction, and can throw), but ownership release doesn't need
 * to wait for that: whichever FiberContext holds a non-null `mmap_base` when
 * it goes out of scope frees it, the same lazy-acquire/RAII-release shape as
 * std::unique_ptr.
 *
 * TODO: alloc_fiber_stack()/free_fiber_stack() are still free functions
 * rather than members mostly as a matter of style; consider folding them
 * into FiberContext itself (and making the fields private) at some point.
 */
struct FiberContext {
    ucontext_t uc{};
    void*      mmap_base  = nullptr; // guard page + usable stack, from mmap(); null if no stack owned
    size_t     mmap_size  = 0;       // total mmap'd size, including the guard page
    std::byte* stack_base = nullptr; // usable region start, above the guard page
    size_t     stack_size = 0;       // usable region size

    FiberContext() = default;
    FiberContext(const FiberContext&)            = delete;
    FiberContext& operator=(const FiberContext&) = delete;

    FiberContext(FiberContext&& other) noexcept
        : uc(other.uc), mmap_base(other.mmap_base), mmap_size(other.mmap_size),
          stack_base(other.stack_base), stack_size(other.stack_size) {
        other.mmap_base = nullptr;
    }

    FiberContext& operator=(FiberContext&& other) noexcept {
        if (this == &other) return *this;
        uc         = other.uc;
        mmap_base  = other.mmap_base;
        mmap_size  = other.mmap_size;
        stack_base = other.stack_base;
        stack_size = other.stack_size;
        other.mmap_base = nullptr;
        return *this;
    }

    // Frees whatever stack this context owns, if any -- see free_fiber_stack()
    // below. Safe on a context that never had a stack allocated.
    ~FiberContext();
};

/// @brief Allocates `stack_size` (rounded up to a page) of stack for `fc`, with an
/// `mprotect(PROT_NONE)` guard page immediately below it — an overrun faults
/// (SIGSEGV) instead of silently corrupting adjacent memory. Throws std::bad_alloc
/// or std::runtime_error on failure.
void alloc_fiber_stack(FiberContext& fc, size_t stack_size);

/// @brief Unmaps the stack (and guard page) owned by `fc`, if any. Safe to call on a
/// FiberContext that never had a stack allocated (e.g. a caller-side context).
void free_fiber_stack(FiberContext& fc) noexcept;

/// @brief Finishes preparing `fc` (which must already have a stack via
/// alloc_fiber_stack()) to start executing `entry` the first time it is switched
/// into. `entry` takes no arguments — see FiberFuture::trampoline()'s use of the
/// t_fiber_start_arg thread-local to recover per-instance state.
void init_fiber_entry(FiberContext& fc, void (*entry)());

/// @brief Saves the caller's context into `from` and resumes `to` where it last left
/// off (or starts it, if this is its first switch since init_fiber_entry()).
void switch_context(FiberContext* from, FiberContext* to);

/// @brief Must be called as the very first statement of any function used as a
/// fiber's entry point (i.e. FiberFuture<T>::trampoline()). Completes the ASan
/// fiber-switch handshake for a stack entered for the first time via
/// makecontext() -- switch_context()'s own post-swapcontext() handshake only
/// covers a context being *resumed*, not one starting fresh. No-op (and safe
/// to call unconditionally) when not built with AddressSanitizer.
void notify_fiber_switch_finished();

#endif

} // namespace coro::detail
