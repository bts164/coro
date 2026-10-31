// Windows backend for FiberContext / switch_context(): Win32 fibers. See
// include/coro/detail/fiber_context.h and doc/design/fiber.md, "Windows backend".

#include <coro/detail/fiber_context.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <exception>
#include <system_error>

// AddressSanitizer tracks one stack per thread. Without being told of each switch it
// takes a fiber's stack for unrelated memory: a throw on a fiber then leaves the
// fiber's stale redzones poisoned (ASan clears only the stack it knows), and the
// exception machinery's own writes there are reported as stack-buffer-underflow. The
// __sanitizer_{start,finish}_switch_fiber() pair names the stack each switch goes to,
// as in fiber_context_posix.cpp.
#if defined(__SANITIZE_ADDRESS__)
#define CORO_FIBER_ASAN 1
#include <sanitizer/asan_interface.h>
#include <sanitizer/common_interface_defs.h>
#endif

namespace coro::detail {

namespace {

#if defined(CORO_FIBER_ASAN)
// The running fiber's stack, from the thread's TEB, which SwitchToFiber() keeps
// current: the reserved range, including the part not yet committed.
void record_stack(FiberContext& fc) {
    ULONG_PTR low  = 0;
    ULONG_PTR high = 0;
    ::GetCurrentThreadStackLimits(&low, &high);
    fc.asan_stack_bottom = reinterpret_cast<void*>(low);
    fc.asan_stack_bytes  = static_cast<size_t>(high - low);
}
#endif

// A thread must be a fiber itself before it can switch to one. The first
// switch_context() on a thread converts it, and the conversion is undone when the
// thread exits, on the thread's own fiber (a thread exits from its original stack).
// A thread some other code already made a fiber is left as it was.
struct ThreadFiber {
    bool converted = false;

    ~ThreadFiber() {
        if (converted) ::ConvertFiberToThread();
    }

    void ensure() {
        if (converted || ::IsThreadAFiber()) return;
        // FIBER_FLAG_FLOAT_SWITCH: floating-point state is saved and restored with
        // each switch, as ucontext does on POSIX.
        if (::ConvertThreadToFiberEx(nullptr, FIBER_FLAG_FLOAT_SWITCH) == nullptr)
            throw std::system_error(static_cast<int>(::GetLastError()), std::system_category(),
                                    "ConvertThreadToFiberEx");
        converted = true;
    }
};

thread_local ThreadFiber t_thread_fiber;

// A fiber's start routine. `entry` (FiberFuture<T>::trampoline()) never returns: it
// ends by switching back to its caller, and is deleted from there. A fiber whose
// start routine returned would end its thread.
void WINAPI fiber_start(void* entry) {
#if defined(CORO_FIBER_ASAN)
    // The switch here named no stack: CreateFiberEx() chose it, and its bounds can only
    // be read once running on it. So the first switch is finished with none, then a
    // second, in place, names this stack.
    __sanitizer_finish_switch_fiber(nullptr, nullptr, nullptr);
    FiberContext self;
    record_stack(self);
    __sanitizer_start_switch_fiber(&self.asan_fake_stack, self.asan_stack_bottom,
                                   self.asan_stack_bytes);
    __sanitizer_finish_switch_fiber(self.asan_fake_stack, nullptr, nullptr);
#endif
    reinterpret_cast<void (*)()>(entry)();
    std::terminate();
}

} // namespace

void alloc_fiber_stack(FiberContext& fc, size_t stack_size) {
    fc.stack_size = stack_size;
}

void free_fiber_stack(FiberContext& fc) noexcept {
#if defined(CORO_FIBER_ASAN)
    // A finished fiber never returns from its last frames, so their redzones stay
    // poisoned, and DeleteFiber() frees the stack where ASan can't see it (POSIX's
    // munmap() is intercepted and clears them). The next fiber's stack may be given
    // the same memory, and would start out with that poison.
    if (fc.owned && fc.asan_stack_bottom != nullptr)
        __asan_unpoison_memory_region(fc.asan_stack_bottom, fc.asan_stack_bytes);
    fc.asan_stack_bottom = nullptr;
    fc.asan_stack_bytes  = 0;
#endif
    if (fc.owned && fc.fiber != nullptr) ::DeleteFiber(fc.fiber);
    fc.fiber = nullptr;
    fc.owned = false;
}

FiberContext::~FiberContext() { free_fiber_stack(*this); }

void init_fiber_entry(FiberContext& fc, void (*entry)()) {
    // The stack is reserved at `stack_size` (rounded up by the OS) and committed as
    // it grows, below a guard page: an overrun raises a stack overflow exception
    // instead of corrupting adjacent memory, as the POSIX backend's guard page does.
    void* fiber = ::CreateFiberEx(0, fc.stack_size, FIBER_FLAG_FLOAT_SWITCH, &fiber_start,
                                  reinterpret_cast<void*>(entry));
    if (fiber == nullptr)
        throw std::system_error(static_cast<int>(::GetLastError()), std::system_category(),
                                "CreateFiberEx");
    free_fiber_stack(fc);
    fc.fiber = fiber;
    fc.owned = true;
}

void switch_context(FiberContext* from, FiberContext* to) {
    t_thread_fiber.ensure();
    // The fiber running now: the thread's own for a caller-side context, which may
    // be a different thread's each time (the task was resumed on another worker), or
    // `from`'s own fiber when a fiber switches back to its caller.
    if (!from->owned) from->fiber = ::GetCurrentFiber();
#if defined(CORO_FIBER_ASAN)
    // `from`'s stack is the running one, so it is known here, for the switch back to
    // it. `to`'s was recorded when it last switched away, or is null if it never ran
    // (fiber_start() handles that case).
    record_stack(*from);
    __sanitizer_start_switch_fiber(&from->asan_fake_stack, to->asan_stack_bottom,
                                   to->asan_stack_bytes);
#endif
    ::SwitchToFiber(to->fiber);
#if defined(CORO_FIBER_ASAN)
    // Back on `from`'s stack, perhaps on another thread.
    __sanitizer_finish_switch_fiber(from->asan_fake_stack, nullptr, nullptr);
#endif
}

} // namespace coro::detail
