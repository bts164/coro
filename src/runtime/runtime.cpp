#include <coro/runtime/runtime.h>
#ifdef CORO_PICO
#include <coro/runtime/current_thread_executor.h>
#include <pico/cyw43_arch.h>
#include <pico/time.h>
#else
#include <coro/runtime/current_thread_executor.h>
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
    std::function<void()> poll_fn = enable_network
        ? std::function<void()>([]() { cyw43_arch_poll(); })
        : std::function<void()>([]() {});
    auto exec = std::make_unique<CurrentThreadExecutor>(
        std::make_unique<PollingParker>(std::move(poll_fn)));
    m_current_thread_executor = exec.get();
    m_executor = std::move(exec);
}

bool Runtime::poll() {
    return m_current_thread_executor->poll_ready_tasks();
}

PicoClock::time_point PicoClock::now() noexcept {
    return time_point(duration(static_cast<rep>(time_us_64())));
}

detail::TimerId Runtime::add_timer(Instant deadline, detail::Weak<detail::Waker> waker) {
    return m_current_thread_executor->add_timer(deadline, std::move(waker));
}

void Runtime::cancel_timer(detail::TimerId id) noexcept {
    // Also reached from ~CurrentThreadExecutor(), as it destroys unfinished tasks
    // and their futures. Its timer queue is still alive then; see the member order in
    // current_thread_executor.h.
    m_current_thread_executor->cancel_timer(id);
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
        m_executor = std::make_unique<CurrentThreadExecutor>(this);
    else
        m_executor = std::make_unique<WorkStealingExecutor>(this, num_threads);
}

Runtime::~Runtime() {
    // Destruction order (reverse declaration order):
    //   1. m_executor — joins all worker threads; no more waker->wake() calls after this.
    //      Its tasks' IoRegistrations deregister from m_io_driver here.
    //   2. m_blocking_pool — joins blocking pool threads.
    //   3. m_io_driver — closes the epoll and eventfd.
    // No explicit action needed here; member destructors fire in the right order.
}

detail::TimerId Runtime::add_timer(Instant deadline, detail::Weak<detail::Waker> waker) {
    if (!turns_io_driver())
        throw std::logic_error(
            "coro timer: this runtime's executor never turns the IoDriver, so the "
            "timer could never fire; use Runtime(n)");
    return m_io_driver.add_timer(deadline, std::move(waker));
}

void Runtime::cancel_timer(detail::TimerId id) noexcept {
    // Must not touch m_executor: this is also reached from the executor's destructor,
    // as it destroys unfinished tasks and their futures. m_io_driver outlives it.
    m_io_driver.cancel_timer(id);
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
