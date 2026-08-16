#include <coro/task/fiber.h>

namespace coro::detail {

// See fiber.h's declarations: plain globals under CORO_PICO (no hardware
// TLS / __aeabi_read_tp on Cortex-M0+), thread_local everywhere else.
#ifdef CORO_PICO
Context*      t_current_fiber_ctx = nullptr;
FiberContext* t_current_fiber_running_ctx = nullptr;
FiberContext* t_current_fiber_caller_ctx  = nullptr;
void*         t_fiber_start_arg = nullptr;
#else
thread_local Context*      t_current_fiber_ctx = nullptr;
thread_local FiberContext* t_current_fiber_running_ctx = nullptr;
thread_local FiberContext* t_current_fiber_caller_ctx  = nullptr;
thread_local void*         t_fiber_start_arg = nullptr;
#endif

void fiber_yield() {
    if (!t_current_fiber_ctx)
        throw std::logic_error("fiber_yield() called outside a fiber");
    switch_context(t_current_fiber_running_ctx, t_current_fiber_caller_ctx);
}

} // namespace coro::detail
