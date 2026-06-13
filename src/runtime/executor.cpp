#include <coro/runtime/executor.h>
#include <coro/detail/task.h>

namespace coro {

Executor::~Executor() = default;

} // namespace coro

// Definition of the current task pointer.
// Set to the running Task* before each poll() call and cleared to nullptr after.
#ifdef CORO_PICO
coro::detail::TaskBase* coro::detail::TaskBase::current = nullptr;
#else
thread_local coro::detail::TaskBase* coro::detail::TaskBase::current = nullptr;
#endif
