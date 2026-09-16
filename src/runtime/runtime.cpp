#include <coro/runtime/runtime.h>
#ifdef CORO_PICO
#include <coro/runtime/current_thread_executor.h>
#include <pico/cyw43_arch.h>
#include <pico/time.h>
#else
#include <coro/runtime/single_threaded_executor.h>
#include <coro/runtime/work_stealing_executor.h>
#endif
#include <stdexcept>

namespace coro {

namespace {
#ifdef CORO_PICO
    Runtime* t_current_runtime = nullptr;
#else
    thread_local Runtime* t_current_runtime = nullptr;
#endif
} // namespace

#ifdef CORO_PICO
Runtime::Runtime(bool enable_network) {
    // See runtime.h's doc comment: skipping cyw43_arch_poll() entirely (rather
    // than e.g. having it check a flag on every call) matters on boards with
    // no CYW43 chip at all -- calling it there touches driver state that was
    // never initialized (cyw43_arch_init() never ran), which is undefined
    // behavior, not just a no-op.
    CurrentThreadExecutor::PollFn poll_fn = enable_network
        ? CurrentThreadExecutor::PollFn([]() { cyw43_arch_poll(); })
        : CurrentThreadExecutor::PollFn([]() {});
    auto exec = std::make_unique<CurrentThreadExecutor>(
        []() -> uint64_t { return time_us_64(); },
        std::move(poll_fn)
    );
    m_current_thread_executor = exec.get();
    m_executor = std::move(exec);
}

bool Runtime::poll() {
    return m_current_thread_executor->poll_ready_tasks();
}

uint64_t Runtime::now_us() const {
    return m_current_thread_executor->now_us();
}

void Runtime::schedule_timer(uint64_t deadline_us, detail::Rc<detail::Waker> waker) {
    m_current_thread_executor->schedule_timer(deadline_us, std::move(waker));
}

void Runtime::register_isr_poll(IsrPollEntry* entry, detail::Rc<detail::Waker> waker) {
    m_current_thread_executor->add_isr_poll(entry, std::move(waker));
}

void Runtime::remove_isr_poll(IsrPollEntry* entry) {
    m_current_thread_executor->remove_isr_poll(entry);
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
