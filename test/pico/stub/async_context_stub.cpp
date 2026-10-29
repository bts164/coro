// Host stub of the Pico SDK's poll-mode async_context and of pico_lwip_nosys. See
// pico/async_context.h for the behaviour it reproduces, and doc/design/pico_port.md,
// "Testing on the host".
//
// The context's permit is a bool under a std::mutex with a condition variable: the
// SDK's binary semaphore, created with a permit (sem_init(&sem, 1, 1)). Workers'
// work_pending flags are read and written under the same mutex, because
// async_context_set_work_pending() is called from another thread here (IsrTrigger's
// thread standing in for an interrupt handler).

#include <pico/async_context.h>
#include <pico/async_context_poll.h>
#include <pico/lwip_nosys.h>
#include <pico/time.h>

#include <lwip/init.h>
#include <lwip/timeouts.h>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

struct async_context_stub_state {
    std::mutex              mutex;
    std::condition_variable cv;
    bool                    permit = true;   // GUARDED BY mutex
    std::thread::id         owner;           // the thread that polls; the SDK's core_num
    std::vector<async_when_pending_worker_t*> when_pending;   // owner thread only
    std::vector<async_at_time_worker_t*>      at_time;        // owner thread only
};

namespace {

async_context_stub_state& state_of(async_context_t* context) {
    assert(context != nullptr && context->state != nullptr);
    // Polling, or adding and removing workers, from another core panics on the board.
    assert(context->state->owner == std::this_thread::get_id() &&
           "async_context used from a thread other than the one that created it");
    return *context->state;
}

} // namespace

bool async_context_poll_init_with_defaults(async_context_poll_t* self) {
    static constexpr async_context_type_t kPollType{ASYNC_CONTEXT_POLL};
    self->core.type  = &kPollType;
    self->core.state = new async_context_stub_state();
    self->core.state->owner = std::this_thread::get_id();
    return true;
}

void async_context_deinit(async_context_t* context) {
    delete context->state;
    context->state = nullptr;
}

bool async_context_add_when_pending_worker(async_context_t* context,
                                           async_when_pending_worker_t* worker) {
    auto& st = state_of(context);
    std::lock_guard lock(st.mutex);
    if (std::find(st.when_pending.begin(), st.when_pending.end(), worker) != st.when_pending.end())
        return false;
    st.when_pending.push_back(worker);
    return true;
}

bool async_context_remove_when_pending_worker(async_context_t* context,
                                              async_when_pending_worker_t* worker) {
    auto& st = state_of(context);
    std::lock_guard lock(st.mutex);
    auto it = std::find(st.when_pending.begin(), st.when_pending.end(), worker);
    if (it == st.when_pending.end()) return false;
    st.when_pending.erase(it);
    return true;
}

void async_context_set_work_pending(async_context_t* context,
                                    async_when_pending_worker_t* worker) {
    // Any thread: no owner check.
    // Notified under the mutex, as an interrupt handler returns before the code it
    // interrupted resumes. Notified after unlocking, the owner could take the permit,
    // finish and deinit the context while this thread is still in notify_all(): a
    // use-after-free of the condition variable.
    auto& st = *context->state;
    std::lock_guard lock(st.mutex);
    worker->work_pending = true;
    st.permit = true;
    st.cv.notify_all();
}

bool async_context_add_at_time_worker(async_context_t* context, async_at_time_worker_t* worker) {
    auto& st = state_of(context);
    std::lock_guard lock(st.mutex);
    if (std::find(st.at_time.begin(), st.at_time.end(), worker) != st.at_time.end())
        return false;
    st.at_time.push_back(worker);
    return true;
}

bool async_context_add_at_time_worker_in_ms(async_context_t* context,
                                            async_at_time_worker_t* worker, uint32_t ms) {
    worker->next_time = make_timeout_time_ms(ms);
    return async_context_add_at_time_worker(context, worker);
}

bool async_context_remove_at_time_worker(async_context_t* context,
                                         async_at_time_worker_t* worker) {
    auto& st = state_of(context);
    std::lock_guard lock(st.mutex);
    auto it = std::find(st.at_time.begin(), st.at_time.end(), worker);
    if (it == st.at_time.end()) return false;
    st.at_time.erase(it);
    return true;
}

void async_context_poll(async_context_t* context) {
    auto& st = state_of(context);
    // Due at-time workers, removed before they run (they re-add themselves to
    // repeat), then the when-pending workers whose flag is set, cleared first so a
    // signal during do_work runs it again next time. Called outside the mutex: a
    // worker may add or remove workers, or mark itself pending.
    std::vector<async_at_time_worker_t*>      due;
    std::vector<async_when_pending_worker_t*> pending;
    {
        std::lock_guard lock(st.mutex);
        const absolute_time_t now = get_absolute_time();
        for (auto it = st.at_time.begin(); it != st.at_time.end();) {
            if ((*it)->next_time <= now) {
                due.push_back(*it);
                it = st.at_time.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (auto* worker : due) worker->do_work(context, worker);
    {
        std::lock_guard lock(st.mutex);
        for (auto* worker : st.when_pending) {
            if (worker->work_pending) {
                worker->work_pending = false;
                pending.push_back(worker);
            }
        }
    }
    for (auto* worker : pending) worker->do_work(context, worker);
}

void async_context_wait_for_work_until(async_context_t* context, absolute_time_t until) {
    auto& st = state_of(context);
    std::unique_lock lock(st.mutex);
    absolute_time_t deadline = until;
    for (const auto* worker : st.at_time) deadline = std::min(deadline, worker->next_time);
    while (!st.permit) {
        const absolute_time_t now = get_absolute_time();
        if (now >= deadline) return;   // timed out: no permit to consume
        if (deadline == at_the_end_of_time)
            st.cv.wait(lock);
        else
            st.cv.wait_for(lock, std::chrono::microseconds(deadline - now));
    }
    st.permit = false;
}

// ---------------------------------------------------------------------------
// pico_lwip_nosys: lwIP's timers on an async_context, as the SDK's lwip_nosys.c
// ---------------------------------------------------------------------------

namespace {

async_context_t* g_lwip_context = nullptr;

void lwip_timeout_reached(async_context_t* /*context*/, async_at_time_worker_t* /*worker*/) {
    sys_check_timeouts();
}

async_at_time_worker_t g_lwip_timeout_worker = {
    nullptr, &lwip_timeout_reached, 0, nullptr,
};

// Always pending: after every poll, re-reads lwIP's next timeout (which tasks may
// have changed) and reschedules the at-time worker. Sets its own flag directly, as
// the SDK's does, so it never releases the permit and never keeps a sleep short.
void update_next_timeout(async_context_t* context, async_when_pending_worker_t* worker) {
    worker->work_pending = true;
    async_context_remove_at_time_worker(context, &g_lwip_timeout_worker);
    const u32_t sleep_ms = sys_timeouts_sleeptime();
    if (sleep_ms != SYS_TIMEOUTS_SLEEPTIME_INFINITE)
        async_context_add_at_time_worker_in_ms(context, &g_lwip_timeout_worker, sleep_ms);
}

async_when_pending_worker_t g_update_worker = {
    nullptr, &update_next_timeout, false, nullptr,
};

} // namespace

bool lwip_nosys_init(async_context_t* context) {
    assert(g_lwip_context == nullptr && "lwip_nosys_init() on two contexts at once");
    static bool done_lwip_init = false;
    if (!done_lwip_init) {
        lwip_init();
        done_lwip_init = true;
    }
    g_lwip_context = context;
    g_update_worker.work_pending = true;
    async_context_add_when_pending_worker(context, &g_update_worker);
    return true;
}

void lwip_nosys_deinit(async_context_t* context) {
    assert(context == g_lwip_context);
    async_context_remove_at_time_worker(context, &g_lwip_timeout_worker);
    async_context_remove_when_pending_worker(context, &g_update_worker);
    g_lwip_context = nullptr;
}
