// lwIP backend for the notify seam (include/coro/detail/sys/notify.h): each notify
// object has its own async_context when-pending worker. notify_signal() marks it
// pending, which the SDK documents as safe from an interrupt handler and which ends
// the Poller's sleep; the worker then runs on the executor's thread and reports the
// object readable. See doc/design/pico_port.md, "Notify objects".

#include "io_object.h"

#include <coro/detail/sys/notify.h>

#include <pico/async_context.h>

namespace coro::detail::sys {

namespace {

// Runs inside async_context_poll(), on the executor's thread. The SDK clears
// work_pending before calling it, so a signal that lands while it runs marks the
// worker pending again and it runs on the next poll: each signal is an edge.
void notify_do_work(async_context_t* /*context*/, async_when_pending_worker_t* worker) {
    mark_ready(static_cast<LwipNotify*>(worker->user_data), IoObject::kReadable);
}

} // namespace

LwipNotify::LwipNotify() : IoObject(Kind::Notify) {
    worker.do_work   = &notify_do_work;
    worker.user_data = this;
}

RawFd notify_open() {
    return new LwipNotify();
}

void notify_signal(RawFd fd) noexcept {
    auto* notify = static_cast<LwipNotify*>(fd);
    // Read once: deregistration clears it on the executor's thread. A signal while
    // unregistered is dropped; nothing waits then, and the caller's own state
    // records what happened.
    async_context* const context = notify->context;
    if (context != nullptr)
        async_context_set_work_pending(context, &notify->worker);
}

void attach_notify(LwipNotify* notify, async_context* context) noexcept {
    async_context_add_when_pending_worker(context, &notify->worker);
    // Published last, so a signal never marks a worker that is not on the list.
    notify->context = context;
}

void detach_notify(LwipNotify* notify, async_context* context) noexcept {
    // work_pending is left alone: an ISR on the other core may be setting it (see
    // the RACE note on LwipNotify::context). A stale flag only makes the worker run
    // once after it is next attached, reporting a spurious readable.
    async_context_remove_when_pending_worker(context, &notify->worker);
}

void close_notify(LwipNotify* notify) noexcept {
    delete notify;
}

} // namespace coro::detail::sys
