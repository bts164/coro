#include <coro/runtime/runtime.h>
#ifdef CORO_PICO
#include <coro/runtime/current_thread_executor.h>
#include <pico/cyw43_arch.h>
#include <pico/time.h>
#include <lwip/netif.h>
#include <lwip/timeouts.h>
#else
#include <coro/runtime/current_thread_executor.h>
#include <coro/runtime/work_stealing_executor.h>
#endif
#include <exception>
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
namespace {
// Delivers packets sent to 127.0.0.1 or to the device's own address. lwIP only
// queues those in a NO_SYS build, and neither the SDK nor the CYW43 driver drains
// the queue.
void deliver_loopback() {
#if defined(ENABLE_LOOPBACK) && ENABLE_LOOPBACK && !LWIP_NETIF_LOOPBACK_MULTITHREADING
    netif_poll_all();
#endif
}
} // namespace

Runtime::Runtime(PicoNetwork network) {
    // See PicoNetwork in runtime.h. The modes differ in what is called, not in a
    // flag checked on every iteration, because cyw43_arch_poll() must not be called
    // at all unless cyw43_arch_init() ran.
    std::function<void()> poll_fn;
    switch (network) {
    case PicoNetwork::None:
        poll_fn = [] {};
        break;
    case PicoNetwork::Lwip:
        // cyw43_arch_poll() would run lwIP's timers; here nothing else does.
        poll_fn = [] {
            sys_check_timeouts();
            deliver_loopback();
        };
        break;
    case PicoNetwork::Cyw43:
        poll_fn = [] {
            cyw43_arch_poll();
            deliver_loopback();
        };
        break;
    }
    auto exec = std::make_unique<CurrentThreadExecutor>(
        std::make_unique<PollingParker>(std::move(poll_fn)));
    m_current_thread_executor = exec.get();
    m_executor = std::move(exec);
}

Runtime::~Runtime() {
    shutdown();
}

void Runtime::shutdown() noexcept {
    if (m_shut_down) return;
    // One thread, no blocking pool: cancel every task and run the loop until the
    // executor owns none. A task spawned meanwhile is born cancelled and is drained
    // by the same loop. See doc/design/runtime_shutdown.md.
    Runtime* const prev = t_current_runtime;
    set_current_runtime(this);
    m_current_thread_executor->begin_shutdown();
    m_current_thread_executor->run_until(
        [this] { return !m_current_thread_executor->has_tasks(); });
    set_current_runtime(prev);
    m_shut_down = true;
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
    // Also reached from ~CurrentThreadExecutor() if it still holds a task (a
    // standalone executor; a Runtime drains its tasks in shutdown() first). Its
    // timer queue is still alive then; see the member order in
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
    m_turns_io_driver = m_executor->turns_io_driver();
}

Runtime::~Runtime() {
    // Empties the executor and the pool and stops their threads. What is left is
    // destroyed in reverse declaration order: the (already null) executor, the pool,
    // then m_io_driver, which closes the epoll and eventfd.
    shutdown();
}

namespace {
[[noreturn]] void throw_shutdown_on_own_thread() {
    throw std::logic_error(
        "coro::Runtime::shutdown() called from one of the runtime's own tasks or "
        "blocking callables: it would wait for itself");
}
} // namespace

void Runtime::shutdown() noexcept {
    // A task or blocking callable of this runtime would wait here for itself to
    // finish. The exception leaves a noexcept function, so this terminates: a
    // deadlock made loud.
    //
    // Not `t_current_runtime == this`: that is also true on a thread that merely
    // called set_current_runtime(), which may shut the runtime down.
    const detail::TaskBase* const running = detail::TaskBase::current;
    if (m_executor != nullptr &&
            ((running != nullptr && running->owning_executor == m_executor.get()) ||
             m_blocking_pool.on_pool_thread()))
        throw_shutdown_on_own_thread();

    {
        std::lock_guard lock(m_shutdown_mutex);
        if (m_shutdown_started) return;
        m_shutdown_started = true;
    }

    // 1. Cancel everything, in both places, and close both: from here on a task
    //    spawned on the executor or submitted to the pool is born cancelled.
    //
    //    RACE: the two are closed one after the other, not atomically. A task
    //    handed to the second before it closes is not born cancelled, but it is
    //    then in the set that begin_shutdown() cancels.
    m_executor->begin_shutdown();
    m_blocking_pool.begin_shutdown();

    // 2. Run until neither holds a task. Timers still fire and I/O is still
    //    dispatched, so cleanup that needs the runtime works.
    //
    //    RACE (a task moving between the two): a blocking callable's last act can
    //    be to spawn a task, and a task's last act can be to spawn a blocking job.
    //    Looking at the executor and then at the pool could find each empty while a
    //    task moved from the one not yet looked at to the one already seen. So:
    //     - this check runs with m_shutdown_mutex held;
    //     - a task that arrives after its destination closed is announced by
    //       shutdown_progress(), which takes m_shutdown_mutex, before its spawner
    //       returns from the spawn. The spawner is itself a live task and stays in
    //       its own list until then.
    //    While this thread holds the mutex, then, a spawner that has handed a task
    //    over but not yet announced it is blocked and still counted; one that has
    //    announced it did so before this check began, and the new task is counted
    //    (or has already finished). Only live tasks spawn, so once both are empty
    //    nothing can refill them.
    //
    //    RACE (lost wake-up): a task that finishes is removed from its list first,
    //    and shutdown_progress() takes m_shutdown_mutex afterwards. The check
    //    below either sees the removal, or the notifier waits for the mutex until
    //    this thread is in wait() (or back in the executor loop, whose unpark is
    //    remembered).
    const auto quiescent = [this] {   // m_shutdown_mutex held
        return !m_executor->has_tasks() && !m_blocking_pool.has_tasks();
    };

    if (m_executor->runs_on_calling_thread()) {
        // Runtime(1): nothing polls the tasks unless this thread does. Blocking
        // callables finish on their own threads and unpark this one through
        // shutdown_progress().
        {
            std::lock_guard lock(m_shutdown_mutex);
            m_shutdown_driver = m_executor.get();
        }
        Runtime* const          prev_runtime = t_current_runtime;
        detail::TaskBase* const prev_task    = detail::TaskBase::current;
        set_current_runtime(this);
        m_executor->run_until([this, &quiescent] {
            std::lock_guard lock(m_shutdown_mutex);
            return quiescent();
        });
        // The executor's loop leaves `current` null; put back whatever the caller had
        // (a task of another runtime that owns this one).
        detail::TaskBase::current = prev_task;
        set_current_runtime(prev_runtime);
        {
            std::lock_guard lock(m_shutdown_mutex);
            m_shutdown_driver = nullptr;
        }
    } else {
        // The executor's workers do the draining; wait for them and the pool.
        std::unique_lock lock(m_shutdown_mutex);
        m_shutdown_cv.wait(lock, quiescent);
    }

    // 3. No task is left, so no thread has anything more to run. Stop them. The
    //    pool first: its last thread may still be inside shutdown_progress(), and
    //    stop() waits for it. Each executor worker is joined by the executor's
    //    destructor, likewise after it has left shutdown_progress().
    m_blocking_pool.stop();
    m_executor.reset();

    // 4. Nothing turns the I/O driver any more. No task of this runtime is left to
    //    care, but a waiter the runtime does not own may be parked on one of its
    //    timers or registrations: a thread outside the runtime, or a task of another
    //    runtime. Wake them all; their futures then fail instead of waiting for
    //    good. See IoDriver::shutdown().
    //
    //    RACE: such a waiter may be registering at this very moment. The driver
    //    orders that against its own shutdown: the registration is either woken
    //    here or refused.
    m_io_driver.shutdown();
    m_shut_down = true;
}

void Runtime::shutdown_progress() noexcept {
    // Everything under the mutex, the notify included: the thread in shutdown()
    // cannot then get past its check and tear the runtime down while this thread
    // is still between the unlock and the notify.
    std::lock_guard lock(m_shutdown_mutex);
    m_shutdown_cv.notify_all();
    if (m_shutdown_driver != nullptr) m_shutdown_driver->recheck_run_until();
}

detail::TimerId Runtime::add_timer(Instant deadline, detail::Weak<detail::Waker> waker) {
    if (!turns_io_driver())
        throw std::logic_error(
            "coro timer: this runtime's executor never turns the IoDriver, so the "
            "timer could never fire; use Runtime(n)");
    // Not check_running(): this may be called by a thread outside the runtime while
    // another thread is in shutdown(), and m_shut_down is not synchronized for
    // that. The timer queue decides instead, under its own mutex: the timer is
    // either in the queue when IoDriver::shutdown() empties it, and is woken, or
    // is refused (std::runtime_error).
    return m_io_driver.add_timer(deadline, std::move(waker));
}

void Runtime::cancel_timer(detail::TimerId id) noexcept {
    // Must not touch m_executor: this is also reached after shutdown(), from the
    // destructor of a future that outlived the runtime's tasks (one waited on by a
    // thread outside the runtime, say). m_io_driver is alive until ~Runtime().
    m_io_driver.cancel_timer(id);
}
#endif

void Runtime::throw_shut_down() {
    std::rethrow_exception(shut_down_error());
}

std::exception_ptr Runtime::shut_down_error() {
    return std::make_exception_ptr(
        std::runtime_error("coro::Runtime: the runtime has been shut down"));
}

void Runtime::throw_cancelled_by_shutdown() {
    throw std::runtime_error(
        "coro::Runtime::block_on(): the runtime shut down before the future completed");
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
