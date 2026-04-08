#include <coro/runtime/runtime.h>
#include <coro/runtime/single_threaded_executor.h>
#include <coro/runtime/work_stealing_executor.h>
#include <stdexcept>

namespace coro {

namespace {
    thread_local Runtime* t_current_runtime = nullptr;
} // namespace

Runtime::Runtime(std::size_t num_threads)
    : m_blocking_pool(this)
{
    if (num_threads <= 1)
        m_executor = std::make_unique<SingleThreadedExecutor>();
    else
        m_executor = std::make_unique<WorkStealingExecutor>(this, num_threads);
}

Runtime::~Runtime() {
    // m_executor is declared after m_io_service, so it is destroyed first (reverse
    // declaration order). The executor destructor joins all worker threads, ensuring
    // no further waker->wake() calls arrive before IoService shuts down.
    // IoService::~IoService() then stops the I/O thread and closes the loop.
    // No explicit action needed here.
}

void set_current_runtime(Runtime* rt) {
    t_current_runtime = rt;
}

Runtime& current_runtime() {
    if (!t_current_runtime)
        throw std::runtime_error("coro::current_runtime(): no runtime active on this thread");
    return *t_current_runtime;
}

} // namespace coro
