#include <coro/task/spawn_blocking.h>
#include <coro/runtime/runtime.h>
#include <thread>

namespace coro {

BlockingPool::BlockingPool(Runtime* rt, std::size_t max_threads)
    : m_runtime(rt), m_max_threads(max_threads)
{}

BlockingPool::~BlockingPool() {
    {
        std::lock_guard lock(m_mutex);
        m_stop = true;
    }
    m_cv.notify_all();

    // Wait for all threads to exit. Threads are detached so we can't join them;
    // instead each thread decrements m_total_threads and notifies before exiting.
    std::unique_lock lock(m_mutex);
    m_cv.wait(lock, [this] { return m_total_threads == 0; });
}

void BlockingPool::submit(std::function<void()> work_item) {
    std::lock_guard lock(m_mutex);

    m_queue.push_back(std::move(work_item));

    if (m_idle_threads > 0) {
        // An idle thread will pick this up.
        m_cv.notify_one();
    } else if (m_total_threads < m_max_threads) {
        // Spin up a new thread.
        ++m_total_threads;
        std::thread([this] { worker_loop(); }).detach();
    }
    // else: at capacity — work will be picked up when a thread becomes idle.
}

void BlockingPool::worker_loop() {
    // Set the thread-local runtime so recursive spawn_blocking calls and
    // blocking_get() work correctly from within this blocking thread.
    set_current_runtime(m_runtime);

    while (true) {
        std::function<void()> work;
        {
            std::unique_lock lock(m_mutex);
            ++m_idle_threads;

            // Wait for work, shutdown, or keep-alive timeout.
            bool got_work = m_cv.wait_for(lock, kKeepAlive, [this] {
                return !m_queue.empty() || m_stop;
            });

            --m_idle_threads;

            if (!m_queue.empty()) {
                work = std::move(m_queue.back());
                m_queue.pop_back();
            } else {
                // Timed out with no work, or stop was set — exit.
                --m_total_threads;
                m_cv.notify_all();  // wake destructor if it's waiting on m_total_threads==0
                return;
            }
        }

        // Execute outside the lock.
        work();
    }
}

namespace detail {

void submit_blocking_work(std::function<void()> work) {
    current_runtime().blocking_pool().submit(std::move(work));
}

} // namespace detail

} // namespace coro
