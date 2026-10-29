// lwIP backend for coro::detail::sys::Poller / PollWaker, on the Pico SDK's
// async_context. See include/coro/detail/sys/poller.h for the backend contract and
// doc/design/pico_port.md, "Poller" and "Wake sources".
//
// Readiness comes from lwIP's callbacks and from notify objects' async_context
// workers, which call Poller::mark_ready(). poll() runs the context (and with it
// the CYW43 driver, lwIP's timers and those workers), drains lwIP's loopback queue,
// and sleeps in async_context_wait_for_work_until() when nothing is ready.

#include "io_object.h"

#include <coro/detail/sys/notify.h>
#include <coro/detail/sys/socket.h>

#include <lwip/netif.h>
#include <pico/async_context.h>
#include <pico/time.h>

#include <cassert>
#include <cerrno>
#include <cstdint>
#include <system_error>
#include <vector>

namespace coro::detail::sys {

// ---------------------------------------------------------------------------
// Module state: whether lwIP runs, and the deferred teardown
// ---------------------------------------------------------------------------

namespace {

bool g_lwip_running = false;
bool g_in_service   = false;

struct Deferred {
    IoObject*      obj;
    async_context* context;   // non-null: detach a notify worker from it; null: close
};
std::vector<Deferred> g_deferred;

// The most loopback passes one service pass makes. Each pass delivers every packet
// queued so far, and a TCP exchange over loopback settles in a few; the cap only
// stops a pathological exchange from keeping the loop here. Packets still queued
// after it keep the Poller from sleeping, so nothing is lost.
constexpr int kMaxLoopbackPasses = 16;

// The longest sleep the Poller asks for, so that the deadline arithmetic cannot
// overflow. A longer wait just wakes once more.
constexpr uint64_t kMaxSleepUs = 3600ull * 1'000'000ull;

void run_deferred() noexcept {
    // Taken out first: a close can be requested from inside another one's teardown.
    while (!g_deferred.empty()) {
        std::vector<Deferred> batch;
        batch.swap(g_deferred);
        for (const Deferred& d : batch) {
            if (d.context != nullptr)
                detach_notify(static_cast<LwipNotify*>(d.obj), d.context);
            else
                close_object_deferred(d.obj);   // not in a service pass now: immediate
        }
    }
}

} // namespace

bool lwip_running() noexcept { return g_lwip_running; }
void set_lwip_running(bool running) noexcept { g_lwip_running = running; }

void require_lwip(const char* what) {
    if (!g_lwip_running) throw_errno(ENETDOWN, what);
}

bool in_service() noexcept { return g_in_service; }

void detach_notify_deferred(LwipNotify* notify, async_context* context) noexcept {
    if (g_in_service)
        g_deferred.push_back({notify, context});   // bad_alloc terminates: noexcept
    else
        detach_notify(notify, context);
}

void close_object_deferred(IoObject* obj) noexcept {
    if (g_in_service) {
        g_deferred.push_back({obj, nullptr});
        return;
    }
    switch (obj->kind) {
    case IoObject::Kind::Tcp:         close_tcp(static_cast<LwipTcp*>(obj));           break;
    case IoObject::Kind::TcpListener: close_listener(static_cast<LwipListener*>(obj)); break;
    case IoObject::Kind::Udp:         close_udp(static_cast<LwipUdp*>(obj));           break;
    case IoObject::Kind::DnsQuery:    close_dns_query(static_cast<LwipDnsQuery*>(obj)); break;
    case IoObject::Kind::Notify:      close_notify(static_cast<LwipNotify*>(obj));     break;
    }
}

// ---------------------------------------------------------------------------
// Poller
// ---------------------------------------------------------------------------

Poller::Poller(PollerOptions options)
    : m_context(options.context)
    , m_lwip(options.lwip)
{
    assert(m_context != nullptr && "the lwIP Poller needs an async_context");
    if (m_lwip) set_lwip_running(true);
}

Poller::~Poller() {
    run_deferred();
    if (m_lwip) set_lwip_running(false);
}

void Poller::register_fd(RawFd fd, void* key, Interest /*interest*/, Trigger trigger) {
    // The interest set is ignored, as epoll's is for errors and hang-ups: every
    // readiness change is reported, and the driver wakes only the waiters it has.
    if (trigger == Trigger::Level)
        throw std::system_error(ENOTSUP, std::generic_category(),
                                "lwIP Poller: level-triggered registrations are not supported");
    assert(fd->poller == nullptr && "IoObject registered twice");
    fd->key    = key;
    fd->poller = this;
    // Readiness seen while unregistered is dropped: a new registration starts
    // ready in both directions, so the first operation looks for itself.
    fd->flags  = 0;
    if (fd->kind == IoObject::Kind::Notify)
        attach_notify(static_cast<LwipNotify*>(fd), m_context);
}

void Poller::reregister_fd(RawFd, void*, Interest, Trigger) {
    // Only level-triggered registrations change their interest set.
    throw std::system_error(ENOTSUP, std::generic_category(),
                            "lwIP Poller: re-registration is not supported");
}

bool Poller::deregister_fd(RawFd fd) noexcept {
    if (fd == nullptr || fd->poller != this) return false;
    unlink_ready(fd);
    fd->key    = nullptr;
    fd->poller = nullptr;
    fd->flags  = 0;
    if (fd->kind == IoObject::Kind::Notify) {
        auto* notify = static_cast<LwipNotify*>(fd);
        // First: from here on notify_signal() drops signals rather than marking a
        // worker that is about to leave the context. RACE: an ISR on the other core
        // that read the pointer just before still marks it; see io_object.h.
        notify->context = nullptr;
        detach_notify_deferred(notify, m_context);
    }
    return true;
}

void Poller::mark_ready(IoObject* obj, uint8_t bits) noexcept {
    obj->flags = static_cast<uint8_t>(obj->flags | (bits & IoObject::kReadiness));
    if (obj->key == nullptr || (obj->flags & IoObject::kQueued) != 0) return;
    obj->flags = static_cast<uint8_t>(obj->flags | IoObject::kQueued);
    obj->next_ready = nullptr;
    if (m_ready_tail != nullptr) m_ready_tail->next_ready = obj;
    else                         m_ready_head = obj;
    m_ready_tail = obj;
}

void Poller::unlink_ready(IoObject* obj) noexcept {
    if ((obj->flags & IoObject::kQueued) == 0) return;
    // Singly linked: a scan. The list holds the objects that turned ready since the
    // last poll(), a handful.
    IoObject* prev = nullptr;
    for (IoObject* cur = m_ready_head; cur != nullptr; prev = cur, cur = cur->next_ready) {
        if (cur != obj) continue;
        if (prev != nullptr) prev->next_ready = cur->next_ready;
        else                 m_ready_head     = cur->next_ready;
        if (m_ready_tail == cur) m_ready_tail = prev;
        break;
    }
    obj->next_ready = nullptr;
    obj->flags = static_cast<uint8_t>(obj->flags & ~IoObject::kQueued);
}

bool Poller::loopback_pending() const noexcept {
#if defined(ENABLE_LOOPBACK) && ENABLE_LOOPBACK && !LWIP_NETIF_LOOPBACK_MULTITHREADING
    if (!m_lwip) return false;
    struct netif* netif;
    NETIF_FOREACH(netif) {
        if (netif->loop_first != nullptr) return true;
    }
#endif
    return false;
}

void Poller::service() {
    g_in_service = true;
    // The CYW43 driver, lwIP's timers, and every pending worker (notify objects, the
    // PollWaker): their callbacks mark objects ready.
    async_context_poll(m_context);
#if defined(ENABLE_LOOPBACK) && ENABLE_LOOPBACK && !LWIP_NETIF_LOOPBACK_MULTITHREADING
    // Packets sent to the device's own addresses wait in lwIP's loopback queue until
    // netif_poll_all(), and delivering one can queue another (a SYN brings a
    // SYN-ACK). The queue is not an async_context wake source, so it is emptied here.
    if (m_lwip) {
        for (int i = 0; i < kMaxLoopbackPasses && loopback_pending(); ++i)
            netif_poll_all();
    }
#endif
    g_in_service = false;
    run_deferred();
}

void Poller::poll(std::vector<Event>& out, std::optional<std::chrono::nanoseconds> timeout) {
    service();
    // Sleep only with nothing to report and nothing left in the loopback queue.
    // async_context_wait_for_work_until() ends early for any
    // async_context_set_work_pending() (the radio's interrupt, a notify signal, an
    // unpark), and is bounded by the context's next timed worker (lwIP's timers) as
    // well as by `timeout`. Its semaphore is latched, so a wake raised since the
    // service pass above returns at once: no wake is lost between the check and the
    // sleep.
    if (m_ready_head == nullptr && !loopback_pending() &&
            (!timeout || timeout->count() > 0)) {
        absolute_time_t until = at_the_end_of_time;
        if (timeout) {
            const auto ns = static_cast<uint64_t>(timeout->count());
            uint64_t us = ns / 1000 + (ns % 1000 != 0 ? 1 : 0);   // round up
            if (us > kMaxSleepUs) us = kMaxSleepUs;
            until = make_timeout_time_us(us);
        }
        async_context_wait_for_work_until(m_context, until);
        service();
    }

    for (IoObject* obj = m_ready_head; obj != nullptr;) {
        IoObject* const next = obj->next_ready;
        Event ev;
        ev.key      = obj->key;
        ev.readable = (obj->flags & IoObject::kReadable) != 0;
        ev.writable = (obj->flags & IoObject::kWritable) != 0;
        ev.error    = (obj->flags & IoObject::kError) != 0;
        ev.hup      = (obj->flags & IoObject::kHup) != 0;
        obj->flags      = 0;
        obj->next_ready = nullptr;
        out.push_back(ev);
        obj = next;
    }
    m_ready_head = nullptr;
    m_ready_tail = nullptr;
}

// ---------------------------------------------------------------------------
// PollWaker: a notify object registered under the waker's key
// ---------------------------------------------------------------------------

PollWaker::PollWaker(Poller& poller, void* key) : m_poller(poller) {
    m_fd = notify_open();
    try {
        m_poller.register_fd(m_fd, key, Interest::read());
    } catch (...) {
        close_socket(m_fd);
        throw;
    }
}

PollWaker::~PollWaker() {
    m_poller.deregister_fd(m_fd);
    close_socket(m_fd);
}

void PollWaker::wake() noexcept {
    // Marks the notify worker pending, which releases the context's semaphore: a
    // sleeping poll() returns, and a wake with nobody sleeping is kept.
    notify_signal(m_fd);
}

void PollWaker::reset() noexcept {
    // Nothing to consume: the notify object's readiness was cleared when poll()
    // reported it.
}

} // namespace coro::detail::sys
