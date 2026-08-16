@ Test-only ARM harness for test_switch_context_pico.cpp -- drives the real
@ src/detail/fiber_context_pico.S primitives (switch_context/init_fiber_entry)
@ under Unicorn. Not part of coro itself; nothing here is production code.
@
@ FiberContext is now 4 words (sp, stack_low, stack_high, buffer -- see
@ fiber_context.h); switch_context() checks a target's sp against
@ [stack_low, stack_high] and the canary word at stack_low before resuming
@ it, so every FiberContext this harness switches *into* needs those fields
@ populated and a canary written into its backing memory first -- exactly
@ what alloc_fiber_stack() would do in real code, done by hand here since
@ this harness doesn't link against fiber_context_pico.cpp.
@
@ kCallerStackBase/kCallerStackSize/kFiberStackBase/kFiberStackSize below
@ must match the same-named constants in test_switch_context_pico.cpp (the
@ memory regions the test maps into Unicorn) -- kept in sync by hand, same as
@ this file's other address constants.
@
@ harness_run is the "everything works" entry point (r0 = fiber_stack_top,
@ set by the test before uc_emu_start). It:
@   1. sets up caller_ctx's and fiber_ctx's stack_low/stack_high + canary
@   2. init_fiber_entry()s a fresh fiber context pointed at fiber_body
@   3. sets r4-r7 to a "caller round 1" sentinel pattern
@   4. switch_context()s into the fiber for the first time (fresh-frame path)
@   5. after the fiber yields back, saves r4-r7 into caller_regs_after_r1 --
@      if switch_context() clobbered a callee-saved register instead of
@      restoring it, this is where it would show up
@   6. repeats with a "round 2" sentinel and caller_regs_after_r2, this time
@      resuming the fiber rather than starting it fresh -- the other half of
@      switch_context()'s contract
@   7. bkpt #0
@
@ fiber_body runs on its own stack (fiber_stack_top downward). Each time it
@ runs, it writes a marker word (proves it actually executed / resumed,
@ rather than the test accidentally passing some other way) and sets its own
@ r4-r7 sentinel pattern before yielding back -- the C++ side never checks
@ the fiber's own registers directly (there's no way to, once it's yielded),
@ but if switch_context() saved/restored the *wrong* context's registers,
@ the caller-side checks above would see the fiber's pattern bleed through
@ instead of its own.
@
@ harness_bad_sp and harness_bad_canary each build a "victim" FiberContext
@ with a bad sp (out of [stack_low, stack_high]) or a clobbered canary word,
@ then switch_context() into it -- both must land in
@ coro_fiber_stack_overflow() (fiber_context_pico.S) instead of resuming with
@ an untrustworthy stack.

.syntax unified
.cpu cortex-m0plus
.thumb

.equ CALLER_STACK_BASE, 0x20000
.equ CALLER_STACK_SIZE, 0x1000
.equ FIBER_STACK_BASE,  0x30000
.equ FIBER_STACK_SIZE,  0x1000
.equ ISR_STACK_BASE,    0x40000
.equ ISR_STACK_SIZE,    0x1000
.equ ZB_TARGET_STACK_BASE, 0x50000
.equ ZB_TARGET_STACK_SIZE, 0x1000
.equ STACK_CANARY,      0xc0ffeeee

.text
.align 2

.global harness_run
.thumb_func
harness_run:
    mov  r5, r0                 @ stash fiber_stack_top -- r0 is about to be clobbered

    @ caller_ctx: bounds + canary cover the harness's own stack (the caller
    @ side of every switch_context() call in this test).
    ldr  r0, =caller_ctx
    ldr  r1, =CALLER_STACK_BASE
    str  r1, [r0, #4]            @ caller_ctx.stack_low
    ldr  r2, =CALLER_STACK_SIZE
    add  r1, r1, r2
    str  r1, [r0, #8]            @ caller_ctx.stack_high
    ldr  r0, =CALLER_STACK_BASE
    ldr  r1, =STACK_CANARY
    str  r1, [r0]

    @ fiber_ctx: bounds + canary cover the fiber's own stack.
    ldr  r0, =fiber_ctx
    ldr  r1, =FIBER_STACK_BASE
    str  r1, [r0, #4]            @ fiber_ctx.stack_low
    ldr  r2, =FIBER_STACK_SIZE
    add  r1, r1, r2
    str  r1, [r0, #8]            @ fiber_ctx.stack_high
    ldr  r0, =FIBER_STACK_BASE
    ldr  r1, =STACK_CANARY
    str  r1, [r0]

    ldr  r0, =fiber_ctx
    mov  r1, r5
    ldr  r2, =fiber_body
    bl   coro_fiber_init_entry_raw

    ldr  r4, =0xaaaaaaa1
    ldr  r5, =0xaaaaaaa2
    ldr  r6, =0xaaaaaaa3
    ldr  r7, =0xaaaaaaa4
    ldr  r0, =caller_ctx
    ldr  r1, =fiber_ctx
    bl   switch_context          @ --- fresh entry into the fiber ---

    ldr  r0, =caller_regs_after_r1
    str  r4, [r0, #0]
    str  r5, [r0, #4]
    str  r6, [r0, #8]
    str  r7, [r0, #12]

    ldr  r4, =0xbbbbbbb1
    ldr  r5, =0xbbbbbbb2
    ldr  r6, =0xbbbbbbb3
    ldr  r7, =0xbbbbbbb4
    ldr  r0, =caller_ctx
    ldr  r1, =fiber_ctx
    bl   switch_context          @ --- resume the fiber (not fresh) ---

    ldr  r0, =caller_regs_after_r2
    str  r4, [r0, #0]
    str  r5, [r0, #4]
    str  r6, [r0, #8]
    str  r7, [r0, #12]

    bkpt #0                      @ done -- harness ran to completion

.global fiber_body
.thumb_func
fiber_body:
    ldr  r0, =marker1
    ldr  r1, =0x11111111
    str  r1, [r0]
    ldr  r4, =0xccccccc1
    ldr  r5, =0xccccccc2
    ldr  r6, =0xccccccc3
    ldr  r7, =0xccccccc4
    ldr  r0, =fiber_ctx
    ldr  r1, =caller_ctx
    bl   switch_context          @ yield back (1st time)

    ldr  r0, =marker2
    ldr  r1, =0x22222222
    str  r1, [r0]
    ldr  r4, =0xccccccc5
    ldr  r5, =0xccccccc6
    ldr  r6, =0xccccccc7
    ldr  r7, =0xccccccc8
    ldr  r0, =fiber_ctx
    ldr  r1, =caller_ctx
    bl   switch_context          @ yield back (2nd time)

    bkpt #1                      @ should never resume a 3rd time in this test

@ Victim FiberContext with a valid canary but sp pointing above stack_high --
@ switch_context() must reject it before ever touching r4-r11/pc from there.
.global harness_bad_sp
.thumb_func
harness_bad_sp:
    ldr  r0, =bad_sp_victim_ctx
    ldr  r1, =FIBER_STACK_BASE
    str  r1, [r0, #4]            @ stack_low
    ldr  r2, =FIBER_STACK_SIZE
    add  r2, r1, r2
    str  r2, [r0, #8]            @ stack_high
    ldr  r3, =0x1000             @ well past stack_high
    add  r2, r2, r3
    str  r2, [r0]                @ sp -- out of range

    ldr  r0, =FIBER_STACK_BASE
    ldr  r1, =STACK_CANARY
    str  r1, [r0]                @ canary itself is fine -- only sp is bad

    ldr  r0, =caller_ctx2
    ldr  r1, =CALLER_STACK_BASE
    str  r1, [r0, #4]
    ldr  r2, =CALLER_STACK_SIZE
    add  r1, r1, r2
    str  r1, [r0, #8]
    ldr  r0, =CALLER_STACK_BASE
    ldr  r1, =STACK_CANARY
    str  r1, [r0]

    ldr  r0, =caller_ctx2
    ldr  r1, =bad_sp_victim_ctx
    bl   switch_context          @ must trap in coro_fiber_stack_overflow()

    bkpt #3                      @ unreachable if the check works

@ Victim FiberContext with sp in range but a clobbered canary word.
.global harness_bad_canary
.thumb_func
harness_bad_canary:
    ldr  r0, =bad_canary_victim_ctx
    ldr  r1, =FIBER_STACK_BASE
    str  r1, [r0, #4]            @ stack_low
    ldr  r2, =FIBER_STACK_SIZE
    add  r2, r1, r2
    str  r2, [r0, #8]            @ stack_high
    subs r2, r2, #16
    str  r2, [r0]                @ sp -- comfortably in range

    ldr  r0, =FIBER_STACK_BASE
    ldr  r1, =0xdeadbeef         @ wrong value -- canary clobbered
    str  r1, [r0]

    ldr  r0, =caller_ctx2
    ldr  r1, =CALLER_STACK_BASE
    str  r1, [r0, #4]
    ldr  r2, =CALLER_STACK_SIZE
    add  r1, r1, r2
    str  r1, [r0, #8]
    ldr  r0, =CALLER_STACK_BASE
    ldr  r1, =STACK_CANARY
    str  r1, [r0]

    ldr  r0, =caller_ctx2
    ldr  r1, =bad_canary_victim_ctx
    bl   switch_context          @ must trap in coro_fiber_stack_overflow()

    bkpt #3                      @ unreachable if the check works

@ Exercises coro_pico_enable_psp_stack_raw() (fiber_context_pico.S) in
@ isolation -- the test reads back CONTROL/PSP/MSP afterward to confirm
@ SPSEL actually flipped, PSP now equals the sp this ran on beforehand (the
@ caller's own stack, undisturbed), and MSP was relocated to the
@ caller-supplied ISR_STACK_BASE+ISR_STACK_SIZE buffer rather than left
@ aliased to PSP -- see that function's big comment for why the two must
@ never alias (confirmed on real hardware to freeze under any interrupt).
@ Requires UC_MODE_MCLASS (see test_switch_context_pico.cpp) -- plain
@ UC_MODE_THUMB doesn't implement the M-profile MRS/MSR special-register
@ encodings at all and faults with UC_ERR_INSN_INVALID, discovered while
@ adding this test.
.global harness_enable_psp
.thumb_func
harness_enable_psp:
    ldr  r0, =ISR_STACK_BASE
    ldr  r1, =ISR_STACK_SIZE
    add  r0, r0, r1              @ r0 = isr_stack_top
    bl   coro_pico_enable_psp_stack_raw
    bkpt #9

@ Regression test for switch_context()'s zero-bounds skip
@ (fiber_context_pico.S's .Lno_bounds_check): a FiberContext left with
@ stack_low == stack_high == 0 models an unmanaged/caller-side context -- e.g.
@ FiberFuture::m_caller_ctx, which is default-constructed and never
@ alloc_fiber_stack()'d -- and switch_context() must resume it without any
@ bounds/canary check rather than spuriously trapping in
@ coro_fiber_stack_overflow(). That was exactly the bug that froze
@ psp_probe.cpp's fiber round-trip phase on real hardware on the very first
@ round trip (see fiber_context_pico.S's switch_context() comment).
@
@ zero_bounds_ctx is left zero-initialized (.bss) -- stack_low/stack_high are
@ deliberately never populated. harness_zero_bounds switches from it into a
@ normal, fully-bounded zb_target_ctx; zb_target_body then switches back INTO
@ zero_bounds_ctx -- that second switch_context() call is where the bug lived,
@ since zero_bounds_ctx is now the *to* side of the switch.
.global harness_zero_bounds
.thumb_func
harness_zero_bounds:
    ldr  r0, =zb_target_ctx
    ldr  r1, =ZB_TARGET_STACK_BASE
    str  r1, [r0, #4]            @ zb_target_ctx.stack_low
    ldr  r2, =ZB_TARGET_STACK_SIZE
    add  r1, r1, r2
    str  r1, [r0, #8]            @ zb_target_ctx.stack_high
    ldr  r0, =ZB_TARGET_STACK_BASE
    ldr  r1, =STACK_CANARY
    str  r1, [r0]

    ldr  r0, =zb_target_ctx
    ldr  r1, =ZB_TARGET_STACK_BASE
    ldr  r2, =ZB_TARGET_STACK_SIZE
    add  r1, r1, r2              @ r1 = stack_top
    ldr  r2, =zb_target_body
    bl   coro_fiber_init_entry_raw

    ldr  r0, =zero_bounds_ctx     @ stack_low/stack_high left at 0 -- the case under test
    ldr  r1, =zb_target_ctx
    bl   switch_context            @ --- switch INTO zb_target_ctx ---
                                    @ (zb_target_body switches back into
                                    @ zero_bounds_ctx before control returns here)

.global zb_success_bkpt
zb_success_bkpt:
    bkpt #10                       @ reached only if the return switch_context()
                                    @ resumed zero_bounds_ctx instead of trapping
                                    @ in coro_fiber_stack_overflow()

.global zb_target_body
.thumb_func
zb_target_body:
    ldr  r0, =zb_target_ctx
    ldr  r1, =zero_bounds_ctx
    bl   switch_context             @ --- switch back INTO zero_bounds_ctx ---
                                     @ (the actual regression: resuming a
                                     @ zero-bounds *to* context)
    bkpt #11                        @ unreachable if the resume above worked

.bss
.align 2
caller_ctx:             .space 16
fiber_ctx:               .space 16
caller_ctx2:             .space 16
bad_sp_victim_ctx:       .space 16
bad_canary_victim_ctx:   .space 16
marker1:                .space 4
marker2:                .space 4
caller_regs_after_r1:   .space 16
caller_regs_after_r2:   .space 16
zero_bounds_ctx:        .space 16
zb_target_ctx:          .space 16
