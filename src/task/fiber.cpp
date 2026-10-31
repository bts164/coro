#include <coro/task/fiber.h>

// Out of line on purpose, and kept so under LTO: each call must compute the
// thread-local's address on the thread that is running. See fiber.h.
#if defined(_MSC_VER)
#define CORO_FIBER_NOINLINE __declspec(noinline)
#else
#define CORO_FIBER_NOINLINE __attribute__((noinline))
#endif

namespace coro::detail {

namespace {

// Plain globals under CORO_PICO, not thread_local: Cortex-M0+ has no hardware
// TLS, and thread_local compiles down to a call to __aeabi_read_tp, which
// bare-metal newlib doesn't implement -- undefined reference at link time. A
// plain global is equivalent on the single-threaded CurrentThreadExecutor -- see
// coro_scope.h's t_current_coro for the same fix applied earlier.
#ifdef CORO_PICO
FiberFrame t_fiber_frame;
void*      t_fiber_start_arg = nullptr;
#else
thread_local FiberFrame t_fiber_frame;
thread_local void*      t_fiber_start_arg = nullptr;
#endif

} // namespace

CORO_FIBER_NOINLINE FiberFrame enter_fiber(FiberFrame frame, void* start_arg) noexcept {
    const FiberFrame previous = t_fiber_frame;
    t_fiber_frame     = frame;
    t_fiber_start_arg = start_arg;
    return previous;
}

CORO_FIBER_NOINLINE void leave_fiber(FiberFrame previous) noexcept {
    t_fiber_frame = previous;
}

CORO_FIBER_NOINLINE Context* current_fiber_context() noexcept {
    return t_fiber_frame.ctx;
}

CORO_FIBER_NOINLINE void* fiber_start_arg() noexcept {
    return t_fiber_start_arg;
}

void fiber_yield() {
    // Reads the frame before switching and nothing after, so a resume on another
    // worker can't see a stale copy here.
    const FiberFrame frame = t_fiber_frame;
    if (!frame.ctx)
        throw std::logic_error("fiber_yield() called outside a fiber");
    switch_context(frame.running, frame.caller);
}

} // namespace coro::detail
