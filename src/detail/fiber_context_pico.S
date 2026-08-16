// ARMv6-M (Cortex-M0+ / RP2040, CORO_PICO) implementation of switch_context(),
// init_fiber_entry(), and coro_pico_enable_psp_stack() -- see
// doc/design/fiber.md's "Stack model (Pico backend)" section. Wired into the
// coro_pico build (cmake/platforms/pico.cmake).
//
// switch_context()'s `mov sp, r2` only ever operates correctly on a dedicated
// per-thread PSP stack once coro_pico_enable_psp_stack() has run -- see that
// function below and its one call site, current_thread_executor.cpp's
// wait_for_completion().
//
// This file accesses FiberContext's `sp`/`stack_low`/`stack_high` fields by
// fixed byte offset (0/4/8) since it's hand-assembled, not compiled against
// fiber_context.h -- those offsets are pinned by static_asserts in that
// header, so if this file's field usage and the header's struct layout ever
// disagree, the build fails loudly instead of corrupting memory silently.
//
// switch_context(FiberContext* from /* r0 */, FiberContext* to /* r1 */):
// saves the callee-saved registers (AAPCS: r4-r11) and the current stack
// pointer onto the *current* stack, stores the resulting sp into *from*,
// loads *to*'s saved sp, and restores the callee-saved registers and return
// address from there. Symmetric and unconditional -- there is no separate
// "first entry" code path here, unlike ucontext's makecontext/swapcontext
// split (see fiber_context_posix.cpp's ASan handshake for that asymmetry).
// init_fiber_entry() achieves that by pre-building a stack frame that is
// byte-for-byte what switch_context()'s restore sequence expects to find,
// so the very first switch into a fresh fiber "returns" into the entry
// function exactly like an ordinary resume would.
//
// ARMv6-M's Thumb-1 PUSH/POP can only reference r0-r7 (plus lr on push / pc
// on pop) -- r8-r11 have to be shuffled through low registers first. This is
// the same dance FreeRTOS's GCC/ARM_CM0 port does for exactly this reason.
//
// Before resuming `to`, switch_context() checks its saved `sp` against
// `to->stack_low`/`to->stack_high` (FiberContext offsets 4/8) and the canary
// word stored at `to->stack_low` -- see doc/design/fiber.md's "Stack overflow
// detection" (ChibiOS's placement: check before resuming, not after). Either
// check failing branches to coro_fiber_stack_overflow() instead of resuming
// with a stack pointer or canary that's no longer trustworthy. Skipped
// entirely when `to->stack_low == 0`: that marks an unmanaged context with no
// allocated buffer (a default-constructed FiberContext, e.g. FiberFuture's
// own m_caller_ctx -- see fiber_context.h), and there is no bound or canary to
// check it against.

.syntax unified
.cpu cortex-m0plus
.thumb

.section .text
.align 2

.equ STACK_CANARY, 0xc0ffeeee

.global switch_context
.thumb_func
switch_context:
    push {r4-r7, lr}
    mov  r2, r8
    mov  r3, r9
    push {r2, r3}
    mov  r2, r10
    mov  r3, r11
    push {r2, r3}
    mov  r2, sp
    str  r2, [r0]           @ from->sp = sp (FiberContext::sp is offset 0)

    ldr  r2, [r1]           @ r2 = to->sp (candidate)
    ldr  r3, [r1, #4]       @ r3 = to->stack_low
    cmp  r3, #0
    beq  .Lno_bounds_check  @ stack_low == 0: `to` is an unmanaged context with
                            @ no allocated buffer (e.g. FiberFuture's own
                            @ m_caller_ctx / FiberContext's default state --
                            @ see fiber_context.h) -- never range/canary-checked.
                            @ Without this, `to->stack_high == 0` too would
                            @ make the very next check read as "sp > 0", which
                            @ is true for every real address, spuriously
                            @ tripping stack-overflow on the *first* switch
                            @ back to any such context -- confirmed on real
                            @ hardware via psp_probe.cpp's fiber round-trip
                            @ phase (locked up on round-trip 0, every time).
    cmp  r2, r3
    blo  .Lstack_overflow   @ sp < stack_low -- out of range (unsigned compare)
    ldr  r3, [r1, #8]       @ r3 = to->stack_high
    cmp  r2, r3
    bhi  .Lstack_overflow   @ sp > stack_high -- out of range (unsigned compare)
    ldr  r3, [r1, #4]       @ r3 = to->stack_low, again (clobbered above)
    ldr  r0, [r3]           @ r0 = canary word at stack_low (from-> no longer needed)
    ldr  r3, =STACK_CANARY
    cmp  r0, r3
    bne  .Lstack_overflow

.Lno_bounds_check:
    mov  sp, r2
    pop  {r2, r3}
    mov  r10, r2
    mov  r11, r3
    pop  {r2, r3}
    mov  r8, r2
    mov  r9, r3
    pop  {r4-r7, pc}        @ restore r4-r7 and jump to the saved return address

.Lstack_overflow:
    bl   coro_fiber_stack_overflow
    b    .Lstack_overflow   @ coro_fiber_stack_overflow() must not return; loop if it somehow does

.pool

// Called when switch_context() detects `to`'s stack has overflowed (sp out of
// range, or its canary word was clobbered). Traps with a breakpoint and then
// loops forever -- a real CORO_PICO build can override this (it's a plain
// global symbol, not weak, so overriding it means replacing this definition,
// e.g. with a call into the Pico SDK's panic()) once that integration exists.
.global coro_fiber_stack_overflow
.thumb_func
coro_fiber_stack_overflow:
    bkpt #2
    b    coro_fiber_stack_overflow

// coro_fiber_init_entry_raw(FiberContext* fc /* r0 */, uint32_t stack_top /* r1 */,
//                           void (*entry)() /* r2 */):
// Builds the 9-word fake frame (r10,r11,r8,r9,r4,r5,r6,r7,pc -- the exact
// layout switch_context()'s restore sequence pops, in that order) at the
// top of the stack buffer, and points it at `entry`. The callee-saved
// register slots are never read by real code before being overwritten by
// the fiber's own execution, so they're left as zero rather than needing
// any particular value.
//
// `entry`'s bit 0 must be set (Thumb-bit) before it's stored as the fake
// frame's "pc" -- ARMv6-M is Thumb-only, and any value loaded directly into
// pc (as this POP {..., pc} does) is interpreted as an interworking branch
// address; bit 0 == 0 would be read as "switch to ARM mode", which this core
// doesn't have and which faults. Ordinary C function pointers on this target
// already have bit 0 set for exactly this reason, but this sets it
// unconditionally rather than assuming callers get that right.
//
// Named "_raw" and takes stack_top explicitly (rather than reading
// fc->stack_high) so its signature matches the POSIX backend's shape as
// closely as ARM's calling convention allows; fiber_context.h wraps this in
// an inline coro::detail::init_fiber_entry(FiberContext&, void(*)()) that
// pulls stack_top from fc itself, so call sites (fiber.h) stay identical
// across both backends -- see that header for why a raw 3-arg extern "C"
// symbol can't just be named init_fiber_entry directly (name collision with
// the wrapper in namespace coro::detail).
.global coro_fiber_init_entry_raw
.thumb_func
coro_fiber_init_entry_raw:
    movs r3, #36            @ 9 words
    subs r1, r1, r3          @ r1 = frame start = stack_top - 36
    movs r3, #0
    str  r3, [r1, #0]        @ r10 (unused until first real switch out)
    str  r3, [r1, #4]        @ r11
    str  r3, [r1, #8]        @ r8
    str  r3, [r1, #12]       @ r9
    str  r3, [r1, #16]       @ r4
    str  r3, [r1, #20]       @ r5
    str  r3, [r1, #24]       @ r6
    str  r3, [r1, #28]       @ r7
    movs r3, #1
    orrs r2, r2, r3          @ entry | 1 -- Thumb bit, see comment above
    str  r2, [r1, #32]       @ pc = entry
    str  r1, [r0]            @ fc->sp = frame start
    bx   lr

// coro_pico_enable_psp_stack_raw(uint32_t isr_stack_top /* r0 */): one-time
// CONTROL/PSP switch -- see doc/design/fiber.md's "Stack model (Pico
// backend)". Must be called exactly once, from the executor's own thread,
// before the first switch_context() call (via the coro_pico_enable_psp_stack()
// wrapper in fiber_context.h, whose only call site is
// current_thread_executor.cpp's wait_for_completion()) and never reverted
// afterward. Not idempotent by contract -- calling it a second time would
// re-point PSP at whatever the *current* sp happens to be at that later call
// site and relocate MSP again, which is harmless in practice (CONTROL.SPSEL
// is already 1) but is not something this function is designed to be relied
// on for; the call site is responsible for only calling it once.
//
// WARNING: an earlier version of this function only aliased PSP to MSP's
// current value and left MSP itself untouched -- confirmed on real hardware
// (not reproducible under Unicorn emulation) to freeze any CORO_PICO program
// within milliseconds of this call returning, fiber or no fiber. Per the
// ARMv6-M exception model, an interrupt taken while thread mode is on PSP
// stacks its 8-word hardware frame onto PSP, but the handler body then
// executes using MSP -- if MSP was left pointing at the *same* address PSP
// now descends from (rather than a genuinely separate region), the handler's
// own local pushes silently overwrite that live exception frame, corrupting
// its saved PC/LR and escalating to an unrecoverable core lockup on return --
// not a catchable HardFault, which is why overriding isr_hardfault never
// caught it during the on-hardware bisection that found this. This version
// takes an explicit, separate buffer top for MSP so the two stacks never
// alias.
//
// r1 = mrs msp reads the *current* stack pointer (still MSP-backed here,
// since CONTROL.SPSEL is still 0), `msr psp, r1` makes PSP alias that same
// address -- the caller's own stack, undisturbed, so there is no
// discontinuity for thread-mode code across this call. `msr msp, r0` then
// relocates MSP to the caller-supplied, dedicated ISR-only buffer (r0 =
// isr_stack_top), which must not overlap the thread stack PSP now owns.
// `movs r1, #2` / `msr control, r1` flips CONTROL.SPSEL (bit 1) to 1,
// selecting PSP for thread-mode execution from here on; `isb` is required
// after writing CONTROL so the pipeline doesn't fetch subsequent instructions
// under stale stack-pointer bank selection (see ARMv6-M Architecture
// Reference Manual, B1.4.4). `bx lr` then returns normally -- to the
// caller's own (PSP-backed) stack, exactly where it left off.
.global coro_pico_enable_psp_stack_raw
.thumb_func
coro_pico_enable_psp_stack_raw:
    mrs  r1, msp
    msr  psp, r1
    msr  msp, r0
    movs r1, #2
    msr  control, r1
    isb
    bx   lr
