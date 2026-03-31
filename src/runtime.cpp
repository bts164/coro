#include <coro/runtime/runtime.h>
#include <coro/runtime/single_threaded_executor.h>
#include <coro/runtime/work_sharing_executor.h>
#include <stdexcept>

namespace coro {

namespace {
    thread_local Runtime* t_current_runtime = nullptr;
} // namespace

Runtime::Runtime(std::size_t num_threads) {
    if (num_threads <= 1)
        m_executor = std::make_unique<SingleThreadedExecutor>();
    else
        m_executor = std::make_unique<WorkSharingExecutor>(num_threads, this);
}

Runtime::~Runtime() {
    // Stop the timer thread before the executor is destroyed. The timer thread fires
    // wakers that call executor::enqueue(), which touches executor-owned state
    // (m_remote_cv). If we let the executor be destroyed first (the default member
    // destruction order: m_executor before m_timer_service), the timer thread can
    // call into already-destroyed executor memory — a use-after-free / data race.
    //
    // After stop() returns the timer thread has joined and will never call wake() again,
    // so it is safe for the executor (and its condition variables) to be destroyed next.
    m_timer_service.stop();
    // m_executor and m_timer_service are then destroyed in reverse declaration order
    // by the implicit member destructors that follow this body.
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
