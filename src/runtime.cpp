#include <coro/runtime/runtime.h>
#ifdef CORO_PICO
#include <coro/pico/pico_executor.h>
#else
#include <coro/runtime/single_threaded_executor.h>
#include <coro/runtime/work_stealing_executor.h>
#endif
#include <stdexcept>

namespace coro {

namespace {
    thread_local Runtime* t_current_runtime = nullptr;
} // namespace

#ifdef CORO_PICO
Runtime::Runtime() {
    auto pico = std::make_unique<PicoExecutor>();
    m_pico_executor = pico.get();
    m_executor = std::move(pico);
}

bool Runtime::poll() {
    return m_pico_executor->poll_ready_tasks();
}
#else
Runtime::Runtime(std::size_t num_threads)
    : m_blocking_pool(this)
{
    if (num_threads <= 1)
        m_executor = std::make_unique<SingleThreadedExecutor>();
    else
        m_executor = std::make_unique<WorkStealingExecutor>(this, num_threads);
}

Runtime::~Runtime() {
    // Destruction order (reverse declaration order):
    //   1. m_executor — joins all worker threads; no more waker->wake() calls after this.
    //   2. m_blocking_pool — joins blocking pool threads.
    //   3. m_uv_executor — stops the uv thread and closes the loop last.
    // No explicit action needed here; member destructors fire in the right order.
}
#endif

void set_current_runtime(Runtime* rt) {
    t_current_runtime = rt;
}

Runtime& current_runtime() {
    if (!t_current_runtime)
        throw std::runtime_error("coro::current_runtime(): no runtime active on this thread");
    return *t_current_runtime;
}

} // namespace coro
