#include <coro/runtime/io_driver.h>
#include <coro/detail/trace.h>

#include <array>
#include <cassert>
#include <stdexcept>
#include <utility>
#include <vector>

// ThreadSanitizer detection: GCC defines __SANITIZE_THREAD__, Clang has the feature test.
#if defined(__SANITIZE_THREAD__)
#define CORO_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define CORO_TSAN 1
#endif
#endif

#ifdef CORO_TSAN
#include <sanitizer/tsan_interface.h>
#endif

namespace coro {

namespace {

// A ScheduledIo built on one thread reaches the turning thread only through the kernel:
// add() hands its address to epoll_ctl(), and epoll_wait() returns it to turn(). The
// kernel orders the two (both take the epoll instance's lock), but TSan can't see that
// and reports dispatch()'s first touch of the object as a race. These tell TSan about
// the ordering the kernel already provides. Annotations, not a mutex: a mutex taken per
// turn would put a shared cache line on the hot path for the sake of a tool. They
// compile to nothing outside TSan builds.
inline void publish_to_poller([[maybe_unused]] const void* io) noexcept {
#ifdef CORO_TSAN
    __tsan_release(const_cast<void*>(io));
#endif
}

inline void receive_from_poller([[maybe_unused]] const void* io) noexcept {
#ifdef CORO_TSAN
    __tsan_acquire(const_cast<void*>(io));
#endif
}

} // namespace

// ---------------------------------------------------------------------------
// IoDriver
// ---------------------------------------------------------------------------

IoDriver::IoDriver(detail::sys::PollerOptions options)
    : m_poller(options)
    , m_unpark(m_poller, &m_unpark_key)
{}

// Members tear down in reverse order: pending releases, then m_unpark (which
// deregisters from m_poller), then m_poller. Any IoRegistration still alive at this
// point dangles — see the @warning on IoDriver.
IoDriver::~IoDriver() = default;

std::size_t IoDriver::turn(std::optional<std::chrono::nanoseconds> timeout) {
    std::lock_guard turn_lock(m_turn_mutex);
    return turn_locked(timeout);
}

std::optional<std::size_t> IoDriver::try_turn(std::optional<std::chrono::nanoseconds> timeout) {
    std::unique_lock turn_lock(m_turn_mutex, std::try_to_lock);
    if (!turn_lock.owns_lock()) return std::nullopt;
    return turn_locked(timeout);
}

std::size_t IoDriver::turn_locked(std::optional<std::chrono::nanoseconds> timeout) {
    // Release registrations dropped before this turn. Safe: the previous turn's
    // batch has been fully dispatched (we hold m_turn_mutex), and each of these
    // was removed from the poller before it was queued here, so the poll() below
    // cannot return it. (A deregister that races with the poll() below lands in
    // m_pending_release AFTER this swap, so it survives until the next turn.)
    //
    // Relaxed: the count is only a hint for skipping the lock; the mutex orders the
    // list itself. A deregister() racing with this load is caught by the next turn.
    if (m_pending_release_count.load(std::memory_order_relaxed) != 0) {
        std::vector<detail::Rc<detail::ScheduledIo>> released;
        {
            std::lock_guard lock(m_release_mutex);
            released.swap(m_pending_release);
            m_pending_release_count.store(0, std::memory_order_relaxed);
        }
        released.clear();  // destroy outside m_release_mutex
    }

    // After shutdown() nothing can end a wait: no event wakes anyone who would then
    // unpark, and every timer is gone. So a turn only collects what is ready. It is
    // also what lets shutdown() take the turn: see there.
    if (m_shut_down.load(std::memory_order_acquire)) timeout = std::chrono::nanoseconds{0};

    m_events.clear();
    // The earliest timer bounds the wait. From here to end_wait_and_fire_expired()
    // below, an add_timer() with an earlier deadline unparks this poll; see
    // TimerQueue::begin_wait(). That window includes dispatch, where such an unpark
    // only makes the next turn return at once (one extra turn).
    {
        // No wait (nullopt) means block until something happens.
        const auto wait = m_timers.begin_wait(timeout);
        detail::trace::IoWaitZone zone(!wait || wait->count() > 0);
        m_poller.poll(m_events, wait);
    }

    detail::trace::IoDispatchZone zone;
    std::size_t dispatched = 0;
    for (const auto& event : m_events) {
        if (event.key == &m_unpark_key) {
            m_unpark.reset();
            continue;
        }
        dispatch(event);
        ++dispatched;
    }
    dispatched += m_timers.end_wait_and_fire_expired();
    zone.dispatched(dispatched);
    return dispatched;
}

void IoDriver::unpark() noexcept {
    m_unpark.wake();
}

void IoDriver::shutdown() noexcept {
    // Before any wake below, and before the timers are woken: a waiter that
    // shutdown() wakes must find the flag set when it polls again.
    m_shut_down.store(true, std::memory_order_release);

    // Everything else runs with the turn held, so that a waker woken here runs under
    // the same mutual exclusion as one woken by turn(). A level-triggered
    // registration's waker does its work inline and depends on it.
    //
    // Race (handled): a thread may be blocked in turn(), holding the mutex. The
    // unpark makes that poll return. A turn that starts afterwards reads the flag
    // set above and does not block, so the mutex is released again promptly however
    // often that thread turns. (If it read the flag just before the store, its poll
    // still finds the unpark.) Runtime::shutdown() calls this with its executor
    // threads already stopped, so there the lock is free.
    unpark();
    std::lock_guard turn_lock(m_turn_mutex);

    // An allocation failure below terminates (noexcept), as it would in the
    // destructor path this runs on.
    std::vector<detail::Rc<detail::Waker>> wakers;
    {
        std::lock_guard lock(m_registrations_mutex);
        if (m_closed) return;
        m_closed = true;

        // Race (handled): a deregister() on another thread unlinks under
        // m_registrations_mutex before it gives up its reference, so every node
        // reached here is alive. An add() either linked its node before this lock
        // was taken, or finds m_closed and throws.
        for (detail::ScheduledIo* io = m_registrations; io != nullptr; io = io->m_next) {
            std::lock_guard io_lock(io->m_mutex);
            // Race (handled): poll_ready() stores its waker under this same mutex.
            // It either stored before this and is woken below, or runs after and
            // sees m_shutdown.
            io->m_shutdown = true;
            if (io->m_level) {
                // A persistent waker stays in its slot, as in dispatch().
                if (auto waker = io->m_reader.lock()) wakers.push_back(std::move(waker));
                continue;
            }
            if (auto reader = std::exchange(io->m_reader, {}).lock())
                wakers.push_back(std::move(reader));
            if (auto writer = std::exchange(io->m_writer, {}).lock())
                wakers.push_back(std::move(writer));
            // Every listed waiter, whatever its direction. Into the vector rather
            // than in batches like dispatch(): shutdown() may allocate.
            std::array<detail::Rc<detail::Waker>, kWakeBatch> batch;
            for (;;) {
                std::size_t count = 0;
                const bool more = take_listed_wakers(*io, true, true, true,
                                                     batch.data(), batch.size(), count);
                for (std::size_t i = 0; i < count; ++i) wakers.push_back(std::move(batch[i]));
                if (!more) break;
            }
        }
    }
    // Wake outside m_registrations_mutex and the ScheduledIo mutexes, as dispatch()
    // does: a level-triggered waker deregisters fds from inside wake(), which takes
    // both. The strong references keep every waker alive through the loop, so one
    // that drops the owner of a later one does not leave it dangling.
    for (const auto& waker : wakers) waker->wake();

    m_timers.close_and_wake_all();
}

detail::TimerId IoDriver::add_timer(Instant deadline, detail::Weak<detail::Waker> waker) {
    const auto inserted = m_timers.insert(deadline, std::move(waker));
    if (inserted.closed)
        throw std::runtime_error(
            "coro::IoDriver: the driver has been shut down (its Runtime has shut down)");
    // Race (benign): the holder may wake for another reason between insert() and
    // unpark(). The unpark then makes its next turn return at once: one extra turn.
    if (inserted.unpark) unpark();
    return inserted.id;
}

void IoDriver::cancel_timer(detail::TimerId id) noexcept {
    // No unpark: a holder blocked for this timer's deadline wakes then, finds nothing
    // to fire, and turns again. One extra turn, and only if nothing else woke it.
    m_timers.cancel(id);
}

bool IoDriver::take_listed_wakers(detail::ScheduledIo& io, bool readable, bool writable,
                                  bool all, detail::Rc<detail::Waker>* out,
                                  std::size_t limit, std::size_t& count) {
    count = 0;
    IoWaiter* waiter = io.m_waiters.front();
    while (waiter != nullptr) {
        IoWaiter* const next = waiter->next;
        const bool match = all ||
            (waiter->m_direction == IoDirection::Read ? readable : writable);
        if (match) {
            if (count == limit) return true;
            io.m_waiters.remove(waiter);
            waiter->m_linked = false;
            waiter->m_ready  = true;
            if (auto w = std::exchange(waiter->m_waker, {}).lock()) out[count++] = std::move(w);
        }
        waiter = next;
    }
    return false;
}

void IoDriver::dispatch(const detail::sys::Event& event) {
    auto* io = static_cast<detail::ScheduledIo*>(event.key);
    receive_from_poller(io);  // pairs with add(); a no-op outside TSan

    // An error or hang-up wakes both directions: the waiter's next syscall reports
    // it. (EPOLLRDHUP is read-side only, so a writer may be woken needlessly by it;
    // its retry just succeeds or sees EAGAIN again, which is harmless.)
    const bool readable = event.readable || event.error || event.hup;
    const bool writable = event.writable || event.error || event.hup;

    // The slot wakers plus listed waiters, woken outside the lock: wake() enqueues
    // into an executor and must not nest inside ScheduledIo's mutex. In batches, so
    // that any number of listed waiters is woken without allocating.
    std::array<detail::Rc<detail::Waker>, kWakeBatch> listed;
    detail::Rc<detail::Waker> reader;
    detail::Rc<detail::Waker> writer;
    std::size_t count = 0;
    bool more;
    {
        std::lock_guard lock(io->m_mutex);
        if (io->m_level) {
            // Level-triggered: record what was seen for take_ready(), and wake the
            // one waker without taking it out of its slot. It runs inline below,
            // inside this turn.
            //
            // Race (handled): the registration may have been deregistered after
            // this turn fetched the event. deregister() cleared the waker under
            // this mutex, so nothing is woken.
            io->m_readable = io->m_readable || readable;
            io->m_writable = io->m_writable || writable;
            reader = io->m_reader.lock();
        } else {
            if (readable) {
                io->m_readable = true;
                reader = std::exchange(io->m_reader, {}).lock();
            }
            if (writable) {
                io->m_writable = true;
                writer = std::exchange(io->m_writer, {}).lock();
            }
        }
        ++io->m_tick;
        more = take_listed_wakers(*io, readable, writable, false,
                                  listed.data(), listed.size(), count);
    }

    // Race (benign): the waiter may already have re-polled between the unlock above
    // and these calls, seen the readiness, and moved on. The wake is then spurious,
    // which tasks already tolerate.
    if (reader) reader->wake();
    if (writer) writer->wake();
    for (;;) {
        for (std::size_t i = 0; i < count; ++i) {
            listed[i]->wake();
            listed[i] = nullptr;
        }
        if (!more) break;
        // Race (benign): waiters linked since the first batch match too, and are
        // woken by this event although they arrived after it. Each one's next poll
        // just runs its operation, which decides; at worst one spurious wake. Woken
        // waiters are unlinked, so restarting from the head never wakes one twice.
        std::lock_guard lock(io->m_mutex);
        more = take_listed_wakers(*io, readable, writable, false,
                                  listed.data(), listed.size(), count);
    }
}

detail::Rc<detail::ScheduledIo> IoDriver::add(detail::sys::RawFd fd, detail::sys::Interest interest,
                                              detail::sys::Trigger trigger) {
    auto io = detail::make_rc<detail::ScheduledIo>();
    if (trigger == detail::sys::Trigger::Level) {
        // Nothing has been reported yet; see ScheduledIo. Written before the poller
        // can name `io`, so no lock is needed.
        io->m_level    = true;
        io->m_readable = false;
        io->m_writable = false;
    }
    // Held across register_fd() so that shutdown() sees either no registration or a
    // linked one. Once per registration, so the syscall under the lock is no concern.
    std::lock_guard lock(m_registrations_mutex);
    if (m_closed)
        throw std::runtime_error(
            "coro::IoDriver: the driver has been shut down (its Runtime has shut down)");
    // Race (ordered by the kernel): a turn() blocked in poll() on another thread can
    // fetch an event naming `io` as soon as register_fd() adds it, and dispatch() then
    // touches it. epoll_ctl() happens before the epoll_wait() that returns it; this
    // annotation only makes that visible to TSan.
    publish_to_poller(io.get());
    m_poller.register_fd(fd, io.get(), interest, trigger);   // may throw: nothing linked yet
    io->m_next = m_registrations;
    if (m_registrations != nullptr) m_registrations->m_prev = io.get();
    m_registrations = io.get();
    return io;
}

void IoDriver::deregister(detail::sys::RawFd fd, detail::Rc<detail::ScheduledIo> io) noexcept {
    // Fails only if the fd was closed (or never registered) before this: the
    // IoRegistration contract was broken. With a dup of the fd still open, epoll keeps
    // the registration and its raw pointer to `io`, and an event after the deferred
    // release below is a use-after-free. Checked in debug builds only; nothing can be
    // recovered here.
    [[maybe_unused]] const bool removed = m_poller.deregister_fd(fd);
    assert(removed && "IoRegistration: fd closed before deregister()");
    {
        // Out of shutdown()'s reach before this function gives up its reference.
        std::lock_guard lock(m_registrations_mutex);
        if (io->m_prev != nullptr) io->m_prev->m_next = io->m_next;
        else                       m_registrations    = io->m_next;
        if (io->m_next != nullptr) io->m_next->m_prev = io->m_prev;
        io->m_prev = nullptr;
        io->m_next = nullptr;
    }
    {
        // Nobody can wait on this registration any more; drop the wakers now
        // rather than when the deferred release happens.
        std::lock_guard lock(io->m_mutex);
        io->m_reader.reset();
        io->m_writer.reset();
        // Every waiter's future holds a share of whatever owns this registration,
        // so none can still be linked. Unlinked anyway in release builds, so that
        // the list never points into a destroyed future.
        assert(io->m_waiters.empty() && "IoRegistration deregistered with linked IoWaiters");
        while (IoWaiter* waiter = io->m_waiters.pop_front()) {
            waiter->m_linked = false;
            waiter->m_waker.reset();
        }
    }
    // Race: a turn() on another thread may have fetched an event naming `io` just
    // before deregister_fd() above, and may be about to dispatch it. Keeping `io`
    // alive until the start of the next turn makes that dispatch harmless.
    //
    // push_back can throw only on allocation failure, which in a noexcept function
    // terminates — acceptable for a destructor path.
    std::size_t pending;
    {
        std::lock_guard lock(m_release_mutex);
        m_pending_release.push_back(std::move(io));
        pending = m_pending_release.size();
        m_pending_release_count.store(pending, std::memory_order_relaxed);
    }
    // A driver blocked in poll() would otherwise hold these until some other event
    // woke it. Only on reaching the threshold, not past it: one unpark per batch.
    //
    // Race (benign): a turn may swap the list out between the unlock and here; the
    // unpark then just makes the next turn return at once. If no thread is turning
    // (an executor with its own Parker), the list still grows until one does.
    if (pending == kReleaseUnparkThreshold) unpark();
}

// ---------------------------------------------------------------------------
// IoRegistration
// ---------------------------------------------------------------------------

IoRegistration::IoRegistration(IoDriver& driver, detail::sys::RawFd fd, detail::sys::Interest interest)
    : IoRegistration(driver, fd, interest, detail::sys::Trigger::Edge)
{}

IoRegistration::IoRegistration(IoDriver& driver, detail::sys::RawFd fd,
                               detail::sys::Interest interest, detail::sys::Trigger trigger)
    : m_driver(&driver)
    , m_fd(fd)
    , m_io(driver.add(fd, interest, trigger))
{}

IoRegistration::~IoRegistration() {
    deregister();
}

IoRegistration::IoRegistration(IoRegistration&& other) noexcept
    : m_driver(std::exchange(other.m_driver, nullptr))
    , m_fd(std::exchange(other.m_fd, detail::sys::kInvalidFd))
    , m_io(std::move(other.m_io))
{}

IoRegistration& IoRegistration::operator=(IoRegistration&& other) noexcept {
    if (this != &other) {
        deregister();
        m_driver = std::exchange(other.m_driver, nullptr);
        m_fd     = std::exchange(other.m_fd, detail::sys::kInvalidFd);
        m_io     = std::move(other.m_io);
    }
    return *this;
}

std::optional<IoReadyEvent> IoRegistration::poll_ready(IoDirection direction,
                                                       detail::Context& ctx) {
    assert(m_io && "poll_ready() on an empty IoRegistration");
    assert(!m_io->m_level && "poll_ready() on a level-triggered IoRegistration");
    std::lock_guard lock(m_io->m_mutex);
    if (m_io->m_shutdown)
        return IoReadyEvent{direction, m_io->m_tick, true};
    const bool ready = direction == IoDirection::Read ? m_io->m_readable
                                                      : m_io->m_writable;
    if (ready)
        return IoReadyEvent{direction, m_io->m_tick};

    // Stored under the same lock dispatch() uses to set readiness, so an event
    // either happened before this check (we returned ready above) or will see
    // this waker.
    auto& slot = direction == IoDirection::Read ? m_io->m_reader : m_io->m_writer;
    slot = ctx.get_weak_waker();
    return std::nullopt;
}

void IoRegistration::clear_ready(IoReadyEvent event) {
    assert(m_io && "clear_ready() on an empty IoRegistration");
    {
        std::lock_guard lock(m_io->m_mutex);
        // A changed tick means the driver reported new readiness after the caller's
        // poll_ready(); that readiness may postdate the caller's EAGAIN, so keep it.
        // (The tick is shared by both directions, so an event for the other direction
        // also keeps it; that costs the caller one extra EAGAIN, never a lost wake.)
        if (m_io->m_tick != event.tick) return;
        if (event.direction == IoDirection::Read)
            m_io->m_readable = false;
        else
            m_io->m_writable = false;
    }
    // The edge contract lets the caller wait from here on, so a poller that stops
    // reporting a direction once it has reported it (Windows) must poll for it again.
    // A no-op on epoll and lwIP. After the clear and outside the mutex: an event from
    // the re-armed poll then sets the flag again, and the AFD poll reports the
    // socket's state when it is submitted, so data that arrived between the EAGAIN
    // and here is not lost. See doc/design/windows_port.md, "Edge emulation".
    //
    // Race (benign): a turn on another thread may dispatch an event for this
    // direction between the unlock and this call; the re-arm then only makes the
    // poller report it once more.
    m_driver->m_poller.rearm(m_fd, event.direction == IoDirection::Read
                                       ? detail::sys::Interest::read()
                                       : detail::sys::Interest::write());
}

void IoRegistration::deregister() noexcept {
    if (!m_io) return;
    m_driver->deregister(m_fd, std::move(m_io));
    m_io     = nullptr;
    m_driver = nullptr;
    m_fd     = detail::sys::kInvalidFd;
}

void IoRegistration::set_interest(detail::sys::Interest interest) {
    assert(m_io && "set_interest() on an empty IoRegistration");
    assert(m_io->m_level && "set_interest() on an edge-triggered IoRegistration");
    // Race (benign): a turn on another thread may already hold an event for a
    // direction this removes, and dispatches it afterwards. The waker tolerates
    // that; see set_interest() in the header.
    m_driver->m_poller.reregister_fd(m_fd, m_io.get(), interest, detail::sys::Trigger::Level);
}

void IoRegistration::set_waker(detail::Weak<detail::Waker> waker) {
    assert(m_io && "set_waker() on an empty IoRegistration");
    assert(m_io->m_level && "set_waker() on an edge-triggered IoRegistration");
    // Under the mutex dispatch() reads the slot with: an event dispatched on
    // another thread wakes either the old waker or this one, never a torn value.
    std::lock_guard lock(m_io->m_mutex);
    m_io->m_reader = std::move(waker);
}

std::optional<IoReadyEvent> IoRegistration::poll_ready(IoDirection direction,
                                                       detail::Context& ctx,
                                                       IoWaiter& waiter) {
    assert(m_io && "poll_ready() on an empty IoRegistration");
    assert(!m_io->m_level && "poll_ready() on a level-triggered IoRegistration");
    std::lock_guard lock(m_io->m_mutex);
    if (m_io->m_shutdown) {
        if (waiter.m_linked) {
            m_io->m_waiters.remove(&waiter);
            waiter.m_linked = false;
            waiter.m_waker.reset();
        }
        waiter.m_ready = false;
        return IoReadyEvent{direction, m_io->m_tick, true};
    }
    // Woken by dispatch(), which unlinked it: ready once, whatever the shared
    // readiness says now. Another waiter may have cleared it since; this one's
    // operation decides. See doc/design/io_driver.md, "More than one waiter".
    if (std::exchange(waiter.m_ready, false))
        return IoReadyEvent{direction, m_io->m_tick};
    if (waiter.m_linked) {
        // Still waiting: the task may have been polled for another reason.
        waiter.m_waker = ctx.get_weak_waker();
        return std::nullopt;
    }
    const bool ready = direction == IoDirection::Read ? m_io->m_readable
                                                      : m_io->m_writable;
    if (ready)
        return IoReadyEvent{direction, m_io->m_tick};
    // Linked under the same lock dispatch() takes, so an event either happened
    // before this check (we returned ready above) or will find this waiter.
    waiter.m_direction = direction;
    waiter.m_waker     = ctx.get_weak_waker();
    waiter.m_linked    = true;
    m_io->m_waiters.push_back(&waiter);
    return std::nullopt;
}

void IoRegistration::cancel_wait(IoWaiter& waiter) noexcept {
    if (!m_io) {
        assert(!waiter.m_linked && "IoWaiter linked to a deregistered IoRegistration");
        return;
    }
    std::lock_guard lock(m_io->m_mutex);
    if (waiter.m_linked) {
        m_io->m_waiters.remove(&waiter);
        waiter.m_linked = false;
    }
    // Race (benign): dispatch() may have unlinked this waiter and taken its waker
    // just before; that wake then reaches a task whose future is gone, a spurious
    // wake it tolerates.
    waiter.m_ready = false;
    waiter.m_waker.reset();
}

IoLevelReady IoRegistration::take_ready() {
    assert(m_io && "take_ready() on an empty IoRegistration");
    assert(m_io->m_level && "take_ready() on an edge-triggered IoRegistration");
    std::lock_guard lock(m_io->m_mutex);
    IoLevelReady ready;
    ready.readable = std::exchange(m_io->m_readable, false);
    ready.writable = std::exchange(m_io->m_writable, false);
    ready.shutdown = m_io->m_shutdown;
    return ready;
}

} // namespace coro
