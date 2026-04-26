#include <coro/runtime/executor.h>
#include <coro/detail/task.h>

namespace coro {

Executor::~Executor() = default;

} // namespace coro

// Definition of the thread-local current task pointer.
// Set to the running Task* before each poll() call and cleared to nullptr after.
thread_local coro::detail::TaskBase* coro::detail::TaskBase::current = nullptr;
