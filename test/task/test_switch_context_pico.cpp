// Validates the real ARMv6-M switch_context()/init_fiber_entry()/
// coro_pico_enable_psp_stack() primitives (src/detail/fiber_context_pico.S)
// under Unicorn -- see doc/design/fiber.md's "Testing strategy" and "Stack
// model (Pico backend)". Wired into coro's real CORO_PICO build
// (cmake/platforms/pico.cmake); this validates the assembly itself in
// isolation, independent of that integration.
//
// Drives test/task/fixtures/switch_context_harness.S (see that file for
// what it does) rather than calling the primitives directly from C++: there's
// no C++ on this side of the fence at all, only Unicorn-emulated ARM code.
// The two source files are cross-assembled and linked together with a fixed
// -Ttext=0x10000 base by the CMake custom_command that produces
// SWITCH_CONTEXT_TEST_BIN_PATH, which is why every address below is a literal
// constant rather than something looked up at runtime -- confirmed against
// that exact link with arm-none-eabi-nm/objdump when the harness was written;
// re-check them (nm on the linked ELF, not just the .bin) if either .S file's
// instruction count changes.

#include <unicorn/unicorn.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <fstream>
#include <iterator>
#include <vector>

#ifndef SWITCH_CONTEXT_TEST_BIN_PATH
#error "SWITCH_CONTEXT_TEST_BIN_PATH must be defined by CMake to the path of switch_context_test.bin"
#endif

namespace {

constexpr uint64_t kCodeBase = 0x10000;
constexpr uint64_t kCodeSize = 0x2000; // covers .text (harness_run/fiber_body/switch_context/
                                        // init_fiber_entry) and the .bss context/marker/register
                                        // scratch words the linker placed right after it

constexpr uint64_t kCallerStackBase = 0x20000;
constexpr uint64_t kCallerStackSize = 0x1000;
constexpr uint64_t kFiberStackBase  = 0x30000;
constexpr uint64_t kFiberStackSize  = 0x1000;
constexpr uint64_t kIsrStackBase    = 0x40000;
constexpr uint64_t kIsrStackSize    = 0x1000;
constexpr uint64_t kZbTargetStackBase = 0x50000;
constexpr uint64_t kZbTargetStackSize = 0x1000;

// From `arm-none-eabi-nm`/`objdump -d` on the linked harness+primitives ELF
// (test/build/*/switch_context_test.elf) -- re-check these (nm on the linked
// ELF, not just the .bin) if either .S file's instruction count changes;
// every address below shifts together since they're all offsets into one
// link.
constexpr uint64_t kHarnessRun          = 0x10084;
constexpr uint64_t kHarnessSuccessBkpt  = 0x100e8; // harness_run's own `bkpt #0` -- reaching this
                                                    // and nowhere else proves both switch_context()
                                                    // round trips completed, in order
constexpr uint64_t kHarnessBadSp        = 0x10118;
constexpr uint64_t kHarnessBadCanary    = 0x1014c;
constexpr uint64_t kHarnessEnablePsp    = 0x1017e; // harness_enable_psp -- see the EnablesPspStack test below
constexpr uint64_t kOverflowTrap        = 0x10048; // coro_fiber_stack_overflow()'s `bkpt #2` --
                                                    // its very first instruction, since it's
                                                    // otherwise just an infinite loop
constexpr uint64_t kHarnessZeroBounds   = 0x1018a; // harness_zero_bounds
constexpr uint64_t kZbSuccessBkpt       = 0x101b2; // zb_success_bkpt's `bkpt #10`
constexpr uint64_t kMarker1             = 0x112a8;
constexpr uint64_t kMarker2             = 0x112ac;
constexpr uint64_t kCallerRegsAfterR1   = 0x112b0; // r4-r7, 4 words
constexpr uint64_t kCallerRegsAfterR2   = 0x112c0; // r4-r7, 4 words

std::vector<uint8_t> read_fixture(const char* path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

// bkpt raises an ARM interrupt; without a hook Unicorn would report it as an
// error from uc_emu_start(). Hooking it and calling uc_emu_stop() turns
// "the harness reached one of its two `bkpt` instructions" into a clean,
// UC_ERR_OK stop -- which specific bkpt fired (the harness's success path at
// kHarnessSuccessBkpt vs. fiber_body's "should never get here" trap) is then
// distinguished by reading PC back afterward, not by intno.
void stop_on_interrupt(uc_engine* uc, uint32_t /*intno*/, void* /*user_data*/) {
    uc_emu_stop(uc);
}

uint32_t read_u32(uc_engine* uc, uint64_t addr) {
    uint32_t value = 0;
    EXPECT_EQ(uc_mem_read(uc, addr, &value, sizeof(value)), UC_ERR_OK);
    return value;
}

} // namespace

TEST(SwitchContextPico, FreshEntryThenResumeRoundTripsCorrectly) {
    auto code = read_fixture(SWITCH_CONTEXT_TEST_BIN_PATH);
    ASSERT_FALSE(code.empty());

    uc_engine* uc = nullptr;
    // UC_MODE_MCLASS is required, not optional -- plain UC_MODE_THUMB doesn't
    // implement the M-profile MRS/MSR special-register encodings (PSP/MSP/
    // CONTROL) that coro_pico_enable_psp_stack() uses (see EnablesPspStack
    // below) and faults with UC_ERR_INSN_INVALID on them. switch_context()/
    // init_fiber_entry() never touch those registers, so this mode switch is
    // a no-op for the tests above that predate it.
    ASSERT_EQ(uc_open(UC_ARCH_ARM, static_cast<uc_mode>(UC_MODE_THUMB | UC_MODE_MCLASS), &uc), UC_ERR_OK);

    ASSERT_EQ(uc_mem_map(uc, kCodeBase, kCodeSize, UC_PROT_ALL), UC_ERR_OK);
    ASSERT_EQ(uc_mem_map(uc, kCallerStackBase, kCallerStackSize, UC_PROT_READ | UC_PROT_WRITE), UC_ERR_OK);
    ASSERT_EQ(uc_mem_map(uc, kFiberStackBase, kFiberStackSize, UC_PROT_READ | UC_PROT_WRITE), UC_ERR_OK);
    ASSERT_EQ(uc_mem_write(uc, kCodeBase, code.data(), code.size()), UC_ERR_OK);

    uc_hook hook{};
    ASSERT_EQ(uc_hook_add(uc, &hook, UC_HOOK_INTR, (void*)stop_on_interrupt, nullptr, 1, 0), UC_ERR_OK);

    uint64_t caller_sp = kCallerStackBase + kCallerStackSize;
    ASSERT_EQ(uc_reg_write(uc, UC_ARM_REG_SP, &caller_sp), UC_ERR_OK);
    uint32_t fiber_stack_top = kFiberStackBase + kFiberStackSize;
    ASSERT_EQ(uc_reg_write(uc, UC_ARM_REG_R0, &fiber_stack_top), UC_ERR_OK);

    auto err = uc_emu_start(uc, kHarnessRun | 1, 0, 0, 0);
    EXPECT_EQ(err, UC_ERR_OK) << uc_strerror(err);

    uint32_t pc = 0;
    ASSERT_EQ(uc_reg_read(uc, UC_ARM_REG_PC, &pc), UC_ERR_OK);
    // Reaching harness_run's own bkpt (not fiber_body's) proves the harness ran
    // its full sequence -- fresh entry, round-1 yield, resume, round-2 yield --
    // rather than stopping early or wandering into the wrong code.
    EXPECT_EQ(pc, kHarnessSuccessBkpt);

    // fiber_body actually ran (both times) rather than the fresh-entry frame
    // jumping into garbage or being skipped.
    EXPECT_EQ(read_u32(uc, kMarker1), 0x11111111u);
    EXPECT_EQ(read_u32(uc, kMarker2), 0x22222222u);

    // switch_context() saved and restored the *caller's* r4-r7 across both
    // round trips -- if it saved/restored the wrong context (or not at all),
    // these would instead show the fiber's 0xccccccc* sentinel pattern, or
    // whatever garbage was left over from the previous round.
    EXPECT_EQ(read_u32(uc, kCallerRegsAfterR1 + 0),  0xaaaaaaa1u);
    EXPECT_EQ(read_u32(uc, kCallerRegsAfterR1 + 4),  0xaaaaaaa2u);
    EXPECT_EQ(read_u32(uc, kCallerRegsAfterR1 + 8),  0xaaaaaaa3u);
    EXPECT_EQ(read_u32(uc, kCallerRegsAfterR1 + 12), 0xaaaaaaa4u);
    EXPECT_EQ(read_u32(uc, kCallerRegsAfterR2 + 0),  0xbbbbbbb1u);
    EXPECT_EQ(read_u32(uc, kCallerRegsAfterR2 + 4),  0xbbbbbbb2u);
    EXPECT_EQ(read_u32(uc, kCallerRegsAfterR2 + 8),  0xbbbbbbb3u);
    EXPECT_EQ(read_u32(uc, kCallerRegsAfterR2 + 12), 0xbbbbbbb4u);

    uc_close(uc);
}

namespace {

// Shared by both overflow tests below: harness_bad_sp/harness_bad_canary each
// build their own victim FiberContext internally (see switch_context_harness.S),
// so the only per-test input is which entry point to run.
void run_overflow_entry_point(uint64_t entry_point) {
    auto code = read_fixture(SWITCH_CONTEXT_TEST_BIN_PATH);
    ASSERT_FALSE(code.empty());

    uc_engine* uc = nullptr;
    // UC_MODE_MCLASS is required, not optional -- plain UC_MODE_THUMB doesn't
    // implement the M-profile MRS/MSR special-register encodings (PSP/MSP/
    // CONTROL) that coro_pico_enable_psp_stack() uses (see EnablesPspStack
    // below) and faults with UC_ERR_INSN_INVALID on them. switch_context()/
    // init_fiber_entry() never touch those registers, so this mode switch is
    // a no-op for the tests above that predate it.
    ASSERT_EQ(uc_open(UC_ARCH_ARM, static_cast<uc_mode>(UC_MODE_THUMB | UC_MODE_MCLASS), &uc), UC_ERR_OK);

    ASSERT_EQ(uc_mem_map(uc, kCodeBase, kCodeSize, UC_PROT_ALL), UC_ERR_OK);
    ASSERT_EQ(uc_mem_map(uc, kCallerStackBase, kCallerStackSize, UC_PROT_READ | UC_PROT_WRITE), UC_ERR_OK);
    ASSERT_EQ(uc_mem_map(uc, kFiberStackBase, kFiberStackSize, UC_PROT_READ | UC_PROT_WRITE), UC_ERR_OK);
    ASSERT_EQ(uc_mem_write(uc, kCodeBase, code.data(), code.size()), UC_ERR_OK);

    uc_hook hook{};
    ASSERT_EQ(uc_hook_add(uc, &hook, UC_HOOK_INTR, (void*)stop_on_interrupt, nullptr, 1, 0), UC_ERR_OK);

    uint64_t caller_sp = kCallerStackBase + kCallerStackSize;
    ASSERT_EQ(uc_reg_write(uc, UC_ARM_REG_SP, &caller_sp), UC_ERR_OK);

    auto err = uc_emu_start(uc, (entry_point | 1), 0, 0, 0);
    EXPECT_EQ(err, UC_ERR_OK) << uc_strerror(err);

    uint32_t pc = 0;
    ASSERT_EQ(uc_reg_read(uc, UC_ARM_REG_PC, &pc), UC_ERR_OK);
    // Landing here (coro_fiber_stack_overflow()'s own bkpt, not the harness's
    // "unreachable" bkpt #3 further down) proves switch_context() rejected
    // the victim context instead of resuming with a bad sp/canary.
    EXPECT_EQ(pc, kOverflowTrap);

    uc_close(uc);
}

} // namespace

TEST(SwitchContextPico, RejectsOutOfRangeStackPointer) {
    run_overflow_entry_point(kHarnessBadSp);
}

TEST(SwitchContextPico, RejectsClobberedCanary) {
    run_overflow_entry_point(kHarnessBadCanary);
}

// Exercises coro_pico_enable_psp_stack_raw() in isolation (harness_enable_psp:
// points r0 at a dedicated ISR stack buffer, calls it, then bkpt) -- see
// doc/design/fiber.md's "Stack model (Pico backend)". Its only real call
// site is the coro_pico_enable_psp_stack() wrapper in fiber_context.h (used
// by CurrentThreadExecutor::wait_for_completion()), which this test can't
// drive directly (no C++ runs under Unicorn here), so this confirms the
// primitive itself does what that call site depends on: flips CONTROL.SPSEL
// from MSP to PSP without moving the caller's own stack (PSP ends up equal
// to whatever sp was already in use), while relocating MSP to the supplied
// buffer -- NOT leaving it aliased to PSP, which is exactly the real-hardware
// freeze this function used to cause (see fiber_context_pico.S's big comment
// at the definition).
TEST(SwitchContextPico, EnablesPspStack) {
    auto code = read_fixture(SWITCH_CONTEXT_TEST_BIN_PATH);
    ASSERT_FALSE(code.empty());

    uc_engine* uc = nullptr;
    ASSERT_EQ(uc_open(UC_ARCH_ARM, static_cast<uc_mode>(UC_MODE_THUMB | UC_MODE_MCLASS), &uc), UC_ERR_OK);

    ASSERT_EQ(uc_mem_map(uc, kCodeBase, kCodeSize, UC_PROT_ALL), UC_ERR_OK);
    ASSERT_EQ(uc_mem_map(uc, kCallerStackBase, kCallerStackSize, UC_PROT_READ | UC_PROT_WRITE), UC_ERR_OK);
    ASSERT_EQ(uc_mem_map(uc, kIsrStackBase, kIsrStackSize, UC_PROT_READ | UC_PROT_WRITE), UC_ERR_OK);
    ASSERT_EQ(uc_mem_write(uc, kCodeBase, code.data(), code.size()), UC_ERR_OK);

    uc_hook hook{};
    ASSERT_EQ(uc_hook_add(uc, &hook, UC_HOOK_INTR, (void*)stop_on_interrupt, nullptr, 1, 0), UC_ERR_OK);

    uint64_t caller_sp = kCallerStackBase + kCallerStackSize;
    ASSERT_EQ(uc_reg_write(uc, UC_ARM_REG_SP, &caller_sp), UC_ERR_OK);

    uint32_t control_before = 0;
    ASSERT_EQ(uc_reg_read(uc, UC_ARM_REG_CONTROL, &control_before), UC_ERR_OK);
    ASSERT_EQ(control_before & 2u, 0u) << "test precondition: starts on MSP (SPSEL clear)";

    auto err = uc_emu_start(uc, kHarnessEnablePsp | 1, 0, 0, 0);
    EXPECT_EQ(err, UC_ERR_OK) << uc_strerror(err);

    uint32_t pc = 0;
    ASSERT_EQ(uc_reg_read(uc, UC_ARM_REG_PC, &pc), UC_ERR_OK);
    EXPECT_EQ(pc, kHarnessEnablePsp + 10); // harness_enable_psp's own bkpt, right after the call

    uint32_t control_after = 0, psp_after = 0, msp_after = 0, sp_after = 0;
    ASSERT_EQ(uc_reg_read(uc, UC_ARM_REG_CONTROL, &control_after), UC_ERR_OK);
    ASSERT_EQ(uc_reg_read(uc, UC_ARM_REG_PSP, &psp_after), UC_ERR_OK);
    ASSERT_EQ(uc_reg_read(uc, UC_ARM_REG_MSP, &msp_after), UC_ERR_OK);
    ASSERT_EQ(uc_reg_read(uc, UC_ARM_REG_SP, &sp_after), UC_ERR_OK);

    EXPECT_NE(control_after & 2u, 0u) << "CONTROL.SPSEL should be set (PSP) after the call";
    EXPECT_EQ(psp_after, static_cast<uint32_t>(caller_sp)) << "PSP should equal the sp we ran on -- no discontinuity for the caller's own stack";
    EXPECT_EQ(sp_after, static_cast<uint32_t>(caller_sp)) << "sp mnemonic now resolves to PSP";
    EXPECT_EQ(msp_after, static_cast<uint32_t>(kIsrStackBase + kIsrStackSize))
        << "MSP must be relocated to the dedicated ISR buffer, NOT left aliased to PSP -- "
           "that aliasing is exactly what froze real hardware (see fiber_context_pico.S)";

    uc_close(uc);
}

// Regression test for switch_context()'s zero-bounds skip
// (fiber_context_pico.S's .Lno_bounds_check): a FiberContext left with
// stack_low == stack_high == 0 models an unmanaged/caller-side context -- e.g.
// FiberFuture::m_caller_ctx, which is default-constructed and never
// alloc_fiber_stack()'d -- and switch_context() must resume it without
// running the bounds/canary check at all, rather than reading stack_high == 0
// as "sp > 0" and spuriously trapping. That exact bug froze psp_probe.cpp's
// fiber round-trip phase on real hardware on the very first round trip (see
// fiber_context_pico.S's switch_context() comment) and was never caught here
// before, since every other FiberContext this suite constructs
// (caller_ctx/fiber_ctx/caller_ctx2/*_victim_ctx) is given real, non-zero
// bounds by hand in switch_context_harness.S.
TEST(SwitchContextPico, ResumesZeroBoundsContextWithoutTrapping) {
    auto code = read_fixture(SWITCH_CONTEXT_TEST_BIN_PATH);
    ASSERT_FALSE(code.empty());

    uc_engine* uc = nullptr;
    ASSERT_EQ(uc_open(UC_ARCH_ARM, static_cast<uc_mode>(UC_MODE_THUMB | UC_MODE_MCLASS), &uc), UC_ERR_OK);

    ASSERT_EQ(uc_mem_map(uc, kCodeBase, kCodeSize, UC_PROT_ALL), UC_ERR_OK);
    ASSERT_EQ(uc_mem_map(uc, kCallerStackBase, kCallerStackSize, UC_PROT_READ | UC_PROT_WRITE), UC_ERR_OK);
    ASSERT_EQ(uc_mem_map(uc, kZbTargetStackBase, kZbTargetStackSize, UC_PROT_READ | UC_PROT_WRITE), UC_ERR_OK);
    ASSERT_EQ(uc_mem_write(uc, kCodeBase, code.data(), code.size()), UC_ERR_OK);

    uc_hook hook{};
    ASSERT_EQ(uc_hook_add(uc, &hook, UC_HOOK_INTR, (void*)stop_on_interrupt, nullptr, 1, 0), UC_ERR_OK);

    // zero_bounds_ctx has no backing buffer of its own -- like the real
    // m_caller_ctx, it just reuses whatever stack the CPU is already running
    // on (here, the caller stack region) for its actual push/pop scratch.
    uint64_t caller_sp = kCallerStackBase + kCallerStackSize;
    ASSERT_EQ(uc_reg_write(uc, UC_ARM_REG_SP, &caller_sp), UC_ERR_OK);

    auto err = uc_emu_start(uc, kHarnessZeroBounds | 1, 0, 0, 0);
    EXPECT_EQ(err, UC_ERR_OK) << uc_strerror(err);

    uint32_t pc = 0;
    ASSERT_EQ(uc_reg_read(uc, UC_ARM_REG_PC, &pc), UC_ERR_OK);
    // Landing at kOverflowTrap means switch_context() rejected the zero-bounds
    // context instead of resuming it -- the exact regression this test exists
    // to catch. Landing at zb_success_bkpt (not zb_target_body's own
    // "unreachable" bkpt #11 further down) proves the round trip through the
    // zero-bounds context completed normally.
    EXPECT_NE(pc, kOverflowTrap);
    EXPECT_EQ(pc, kZbSuccessBkpt);

    uc_close(uc);
}
