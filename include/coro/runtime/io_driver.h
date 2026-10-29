#pragma once

// Readiness-based I/O driver, turned by the executor threads. The readiness source
// is the sys layer's Poller: epoll on Linux, lwIP on the Pico SDK. See
// doc/design/io_driver.md.
//
// IoDriver has no thread of its own. Executor threads call turn() — blocking when
// they are idle, non-blocking when busy — and I/O futures use IoRegistration to
// wait for readiness on any thread, with no cross-thread hop. It also owns the
// runtime's timer queue: the earliest deadline bounds turn()'s wait.

#include <coro/detail/context.h>
#include <coro/detail/intrusive_list.h>
#include <coro/detail/mutex.h>
#include <coro/detail/rc.h>
#include <coro/detail/relaxed_state.h>
#include <coro/detail/waker.h>
#include <coro/detail/sys/poller.h>
#include <coro/detail/timer_queue.h>
#include <coro/runtime/clock.h>
#include <coro/runtime/parker.h>

#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <mutex>
#include <optional>
#include <type_traits>
#include <vector>

namespace coro {

class IoDriver;
class IoRegistration;
class IoWaiter;

/// A readiness direction. One waiter slot exists per direction.
enum class IoDirection { Read, Write };

/**
 * @brief Readiness observed by IoRegistration::poll_ready().
 *
 * Pass it back to clear_ready() after the syscall returned EAGAIN. `tick` identifies
 * the readiness generation the caller acted on, so a readiness event that arrived
 * after the failed syscall is not cleared by mistake.
 *
 * `shutdown` is set once the driver has been shut down. The caller must then fail the
 * operation instead of waiting: no readiness will ever be reported again.
 * IoRegistration::poll_io() does this.
 */
struct IoReadyEvent {
    IoDirection direction;
    uint64_t    tick;
    bool        shutdown = false;
};

/**
 * @brief What IoRegistration::take_ready() reports for a level-triggered
 * registration: the readiness the driver has seen since the previous call.
 *
 * An error or hang-up on the fd sets both directions, as it wakes both waiters of an
 * edge-triggered registration.
 */
struct IoLevelReady {
    bool readable = false;
    bool writable = false;
    /// The driver has been shut down: this registration will never report again.
    bool shutdown = false;
};

/**
 * @brief One waiter on an IoRegistration's waiter list, for primitives that let any
 * number of tasks wait on one object at once (the ISR primitives).
 *
 * Lives inside the waiting future, so a wait allocates nothing. Linked into the
 * registration while waiting; the driver unlinks it when it wakes it, and marks it
 * ready for that wake, so each woken waiter returns ready exactly once even if
 * another waiter clears the shared readiness first. See doc/design/io_driver.md,
 * "More than one waiter".
 *
 * WARNING: must not move while linked: the list holds its address. A future is
 * pinned from its first poll (doc/design/future_and_stream.md), and only a poll
 * links it, so a waiter inside a future never moves while linked. The move
 * operations assert it.
 */
class IoWaiter {
public:
    IoWaiter() = default;
    IoWaiter([[maybe_unused]] IoWaiter&& other) noexcept {
        assert(!other.m_linked && "IoWaiter moved while linked (a future moved after its first poll)");
    }
    IoWaiter& operator=([[maybe_unused]] IoWaiter&& other) noexcept {
        assert(!m_linked && !other.m_linked && "IoWaiter moved while linked");
        return *this;
    }
    IoWaiter(const IoWaiter&)            = delete;
    IoWaiter& operator=(const IoWaiter&) = delete;
    ~IoWaiter() {
        assert(!m_linked && "IoWaiter destroyed while linked: call IoRegistration::cancel_wait()");
    }

    /// True while linked into a registration's waiter list.
    bool linked() const noexcept { return m_linked; }

    // detail::IntrusiveList links. GUARDED BY the registration's ScheduledIo mutex.
    IoWaiter* prev = nullptr;
    IoWaiter* next = nullptr;

private:
    friend class IoDriver;
    friend class IoRegistration;

    // GUARDED BY the registration's ScheduledIo mutex, like everything below.
    detail::Weak<detail::Waker> m_waker;
    IoDirection                 m_direction = IoDirection::Read;
    bool                        m_linked    = false;
    // Set by dispatch() or shutdown() when it wakes (and unlinks) this waiter; the
    // next poll_ready() returns ready and clears it.
    bool                        m_ready     = false;
};

namespace detail {

/**
 * @brief Per-registration readiness state shared by the driver and the registration.
 *
 * Internal; only IoDriver and IoRegistration touch it. Its address is the key
 * handed to the Poller, so it must stay alive until no poll() batch can still
 * name it — see IoDriver::deregister().
 */
class ScheduledIo {
private:
    friend class coro::IoDriver;
    friend class coro::IoRegistration;

    // std::mutex; a no-op on Pico (see detail/mutex.h).
    Mutex        m_mutex;
    // Set once by IoDriver::add(), before the poller can report this registration.
    // True for a level-triggered registration, which uses the fields below
    // differently: see each one.
    bool         m_level = false;
    // Edge-triggered: readiness starts true, so the first operation is a plain
    // syscall attempt; the "wait" only happens after a real EAGAIN.
    // Level-triggered: starts false, and holds what the driver has reported since
    // the last take_ready(). GUARDED BY m_mutex.
    bool         m_readable = true;
    bool         m_writable = true;
    // Bumped by every driver event for this registration. GUARDED BY m_mutex.
    uint64_t     m_tick = 0;
    // Edge-triggered: one waiter per direction (one recv and one send in flight at
    // a time), taken out of its slot when woken.
    // Level-triggered: m_reader is the one waker set by set_waker(). It is woken
    // for either direction and stays in the slot; m_writer is unused.
    // Weak: the task owns the future that owns the registration, so a strong
    // reference here would form a cycle. GUARDED BY m_mutex.
    Weak<Waker>  m_reader;
    Weak<Waker>  m_writer;
    // Set by IoDriver::shutdown(): every poll_ready() from then on reports it.
    // GUARDED BY m_mutex.
    bool         m_shutdown = false;
    // Waiters that wait through an IoWaiter rather than the per-direction slots:
    // any number of them, in either direction. Edge-triggered only. See
    // doc/design/io_driver.md, "More than one waiter". GUARDED BY m_mutex.
    IntrusiveList<IoWaiter*> m_waiters;

    // Links in the driver's list of live registrations, which shutdown() walks.
    // GUARDED BY IoDriver::m_registrations_mutex.
    ScheduledIo* m_prev = nullptr;
    ScheduledIo* m_next = nullptr;
};

} // namespace detail

/**
 * @brief Owns the OS readiness queue and dispatches events to waiting futures.
 *
 * Thread safety:
 *  - turn() may run on only one thread at a time (serialized by an internal mutex).
 *  - unpark(), shutdown(), and registering/deregistering via IoRegistration, are safe
 *    from any thread, including while another thread is blocked in turn().
 *
 * @warning The driver must outlive every IoRegistration created from it.
 */
class IoDriver {
public:
    /// `options` configure the platform's Poller (the lwIP backend's async_context;
    /// nothing on POSIX).
    explicit IoDriver(detail::sys::PollerOptions options = {});
    ~IoDriver();

    IoDriver(const IoDriver&)            = delete;
    IoDriver& operator=(const IoDriver&) = delete;
    IoDriver(IoDriver&&)                 = delete;
    IoDriver& operator=(IoDriver&&)      = delete;

    /**
     * @brief Waits up to `timeout` for I/O events and dispatches them.
     *
     * Releases registrations dropped since the previous turn, polls, then for each
     * event marks the registration ready, bumps its tick, and wakes its waiters.
     * Finally fires every timer whose deadline has passed. Wakers fire on the
     * calling thread, so a woken task that belongs to this thread's executor is a
     * plain local enqueue.
     *
     * The wait is bounded by the earliest timer deadline as well as `timeout`.
     *
     * @param timeout `std::nullopt` blocks until an event, a timer or unpark(); zero
     *        never blocks.
     * @return The number of I/O readiness events dispatched plus timers fired
     *         (unpark() not counted).
     */
    std::size_t turn(std::optional<std::chrono::nanoseconds> timeout);

    /**
     * @brief Like turn(), but returns `std::nullopt` at once if another thread is
     * turning.
     *
     * Used by multi-threaded executors, whose workers share one driver: a worker that
     * can't get the driver parks somewhere else rather than queueing behind it.
     */
    std::optional<std::size_t> try_turn(std::optional<std::chrono::nanoseconds> timeout);

    /**
     * @brief Like try_turn(), but once this thread holds the driver it calls
     * `before_poll()`; if that returns false, it releases the driver without polling
     * and returns 0.
     *
     * Lets a parking worker check for a wake token and publish "blocked in the driver"
     * while it already holds the turn. Only the holder ever consumes an unpark(), so
     * an unpark() made after `before_poll()` returns true can't be eaten by another
     * thread's turn: it reaches this thread's poll(). See
     * WorkStealingExecutor::park_worker().
     */
    template<typename BeforePoll>
    std::optional<std::size_t> try_turn(std::optional<std::chrono::nanoseconds> timeout,
                                        BeforePoll&& before_poll) {
        std::unique_lock turn_lock(m_turn_mutex, std::try_to_lock);
        if (!turn_lock.owns_lock()) return std::nullopt;
        if (!before_poll()) return 0;
        return turn_locked(timeout);
    }

    /**
     * @brief Makes a blocked turn() return, or the next turn() return immediately.
     *
     * Thread-safe. Callers that want the turning thread to observe new work must
     * publish the work (e.g. push onto the injection queue) BEFORE calling unpark().
     */
    void unpark() noexcept;

    /**
     * @brief Tells every waiter that this driver will not be turned again.
     *
     * Called by `Runtime::shutdown()` once the executor's threads have stopped. A
     * task of that runtime never sees it: they have all finished by then. It is for
     * a waiter the runtime does not own: a thread outside it, or a task of another
     * runtime, waiting on one of this driver's timers or registrations. Without this
     * it would wait for good.
     *
     *  - Every registration is marked shut down and its waiters are woken. From then
     *    on IoRegistration::poll_io() fails with `sys::kDriverShutDown`, without
     *    running the operation, and take_ready() reports `shutdown`.
     *  - Every timer is removed and its waker woken. is_shut_down() tells the woken
     *    waiter why; add_timer() throws from then on.
     *  - Registering a new fd throws.
     *
     * It holds the turn throughout, so a waker it wakes runs under the same mutual
     * exclusion as one woken by turn(). A level-triggered registration's waker, which
     * does its work inline, relies on that. It first makes a thread blocked in turn()
     * return; from then on turn() never blocks, since nothing could end the wait.
     *
     * turn(), unpark(), cancel_timer() and deregistering keep working, so futures
     * and sockets that outlive the shutdown are destroyed as usual. Thread-safe and
     * idempotent. Must not be called from inside a turn (from a waker that turn() or
     * shutdown() is waking): it would wait for itself.
     */
    void shutdown() noexcept;

    /**
     * @brief True once shutdown() has begun. Thread-safe; takes no lock.
     *
     * For a timer's owner that was polled again before its deadline: the wake may be
     * shutdown()'s, and the timer is then gone.
     */
    bool is_shut_down() const noexcept { return m_shut_down.load(std::memory_order_acquire); }

    /// The platform's Poller. For the backend's own objects (the lwIP backend's
    /// sockets reach their Poller through it); not for primitives.
    detail::sys::Poller& poller() noexcept { return m_poller; }

    /**
     * @brief Adds a timer that wakes `waker` once `deadline` has passed.
     *
     * Thread-safe. If a thread is blocked in turn() for a later deadline, unparks it
     * so it recomputes its timeout. Used by `Runtime::add_timer()`.
     *
     * @return The id to pass to cancel_timer().
     * @throws std::runtime_error if the driver has been shut down: the timer could
     *         never fire.
     */
    detail::TimerId add_timer(Instant deadline, detail::Weak<detail::Waker> waker);

    /**
     * @brief Cancels a timer, so that it fires nothing. Thread-safe.
     *
     * Does nothing if the timer has already fired or been cancelled, so the caller
     * need not know which. Used by `Runtime::cancel_timer()`.
     */
    void cancel_timer(detail::TimerId id) noexcept;

private:
    friend class IoRegistration;

    /// Registers `fd`; returns the state its IoRegistration will share.
    /// @throws std::runtime_error if the driver has been shut down.
    detail::Rc<detail::ScheduledIo> add(detail::sys::RawFd fd, detail::sys::Interest interest,
                                        detail::sys::Trigger trigger);

    /// Deregisters `fd` and defers releasing `io` until the start of the next turn().
    void deregister(detail::sys::RawFd fd, detail::Rc<detail::ScheduledIo> io) noexcept;

    /// The body of turn(); the caller holds m_turn_mutex.
    std::size_t turn_locked(std::optional<std::chrono::nanoseconds> timeout);

    static void dispatch(const detail::sys::Event& event);

    // Unlinks every waiter on `io`'s list whose direction `readable`/`writable`
    // (or `all`) covers, marking each ready, and appends its live waker to `out`,
    // at most `limit` of them. Returns true if it stopped at the limit with
    // matching waiters left. Caller holds io->m_mutex.
    static bool take_listed_wakers(detail::ScheduledIo& io, bool readable, bool writable,
                                   bool all, detail::Rc<detail::Waker>* out,
                                   std::size_t limit, std::size_t& count);

    detail::sys::Poller    m_poller;
    // The PollWaker's events carry this object's address as their key. It is a
    // distinct object from every ScheduledIo, so the address can't collide.
    char           m_unpark_key = 0;
    detail::sys::PollWaker m_unpark;

    // Serializes turn() callers, and shutdown() against them. Held for the whole
    // turn, including while blocked in poll(); other threads never take it except
    // to turn or to shut down.
    //
    // Lock order: m_turn_mutex, then m_registrations_mutex.
    detail::Mutex  m_turn_mutex;
    // Reused event buffer. GUARDED BY m_turn_mutex.
    std::vector<detail::sys::Event> m_events;

    // Registrations dropped since the last turn. An event batch fetched by a turn
    // that is still dispatching may name one of these, so they are only released at
    // the start of the NEXT turn. GUARDED BY m_release_mutex.
    detail::Mutex  m_release_mutex;
    std::vector<detail::Rc<detail::ScheduledIo>> m_pending_release;
    // m_pending_release.size(), readable without the lock. Written only under
    // m_release_mutex; read without it at the start of every turn so the common
    // "nothing to release" case doesn't take the mutex (whose cache line would then
    // move to every worker that turns). A hint only: a stale zero just delays a
    // release to a later turn, which is always safe. Mirrors tokio's
    // num_pending_release.
    detail::RelaxedState<std::size_t> m_pending_release_count{0};

    // Listed waiters woken per batch: dispatch() collects at most this many wakers
    // under a registration's mutex, wakes them outside it, and repeats. Bounds its
    // stack use and keeps it from allocating, however many tasks wait.
    static constexpr std::size_t kWakeBatch = 32;

    // A deregister() that brings the pending list to this size unparks the driver,
    // so the releases happen soon even if nothing else wakes it. Tokio uses 16 too.
    static constexpr std::size_t kReleaseUnparkThreshold = 16;

    // Fired at the end of every turn; its earliest deadline bounds the poll.
    // Internally synchronized.
    detail::TimerQueue m_timers;

    // Every registration between add() and deregister(), so that shutdown() can
    // reach them: the poller knows them only as opaque keys. An intrusive list
    // through ScheduledIo::m_prev/m_next. Taken once per registration's lifetime at
    // each end, never per operation.
    //
    // Lock order: m_registrations_mutex, then a ScheduledIo's m_mutex.
    detail::Mutex        m_registrations_mutex;
    detail::ScheduledIo* m_registrations = nullptr;   // GUARDED BY m_registrations_mutex
    // Set by shutdown(); add() refuses from then on. GUARDED BY m_registrations_mutex.
    bool                 m_closed = false;

    // m_closed for readers that must not take a lock: SleepFuture checks it on a
    // re-poll that would otherwise touch nothing shared, and every turn checks it
    // before blocking. Written once, by shutdown(), before it takes the turn and so
    // before it wakes anyone: a waiter woken by shutdown() sees it. An atomic rather
    // than a mutex because both readers are on a hot path.
    detail::RelaxedState<bool> m_shut_down{false};
};

/**
 * @brief An fd's registration with an IoDriver, plus the readiness handshake.
 *
 * Owned by an I/O primitive. It does NOT own the fd: the primitive must call
 * deregister() (or destroy the registration) BEFORE closing the fd.
 *
 * The handshake, for one direction (read shown):
 * @code
 * for (;;) {
 *     auto ready = reg.poll_ready(IoDirection::Read, ctx);
 *     if (!ready) return PollPending;           // waker stored; the driver will wake us
 *     ssize_t n = ::recv(fd, ..., MSG_DONTWAIT);
 *     if (n >= 0) return done(n);
 *     if (errno != EAGAIN) return error(errno);
 *     reg.clear_ready(*ready);                  // only clears if no newer event arrived
 * }
 * @endcode
 *
 * Dropping a future mid-wait is always safe: the only state it left behind is a
 * weak waker in this registration, which a later poll_ready() overwrites.
 *
 * **Level-triggered mode** is for an fd that someone else reads and writes: a
 * library that asks to be called whenever its socket is ready, and cannot promise
 * to drain it. Such a registration has no handshake. Instead:
 *
 *  - set_waker() stores one waker, which stays stored. The driver wakes it, inside
 *    the turn, every time the fd is ready for something in the interest set.
 *  - The waker does the work inline: it calls take_ready() to learn which
 *    directions, then calls into the library. Because it runs inside the turn, it
 *    must not block, turn the driver, or resume a coroutine.
 *  - set_interest() keeps the interest set exact. An fd left registered for writing
 *    while nothing is waiting to write makes every turn return at once.
 *
 * poll_ready(), clear_ready() and poll_io() are for edge-triggered registrations
 * only; set_interest(), set_waker() and take_ready() for level-triggered ones.
 * See doc/design/io_driver.md, "Level-triggered registrations".
 */
class IoRegistration {
public:
    /// An empty registration (no driver, no fd).
    IoRegistration() = default;

    /// Registers `fd` with `driver` for edge-triggered readiness in `interest`.
    /// @throws std::system_error if the OS rejects the registration.
    /// @throws std::runtime_error if `driver` has been shut down.
    IoRegistration(IoDriver& driver, detail::sys::RawFd fd,
                   detail::sys::Interest interest = detail::sys::Interest::read_write());

    /// Registers `fd` with `driver` with the given trigger. With `Trigger::Level`,
    /// nothing is woken until set_waker() has been called.
    /// @throws std::system_error if the OS rejects the registration.
    /// @throws std::runtime_error if `driver` has been shut down.
    IoRegistration(IoDriver& driver, detail::sys::RawFd fd,
                   detail::sys::Interest interest, detail::sys::Trigger trigger);

    ~IoRegistration();

    IoRegistration(IoRegistration&& other) noexcept;
    IoRegistration& operator=(IoRegistration&& other) noexcept;
    IoRegistration(const IoRegistration&)            = delete;
    IoRegistration& operator=(const IoRegistration&) = delete;

    /// True while registered.
    explicit operator bool() const { return m_io != nullptr; }

    /**
     * @brief Returns the current readiness for `direction`, or stores the context's
     * waker and returns `std::nullopt` if that direction isn't ready.
     *
     * Replaces any previously stored waker for that direction.
     *
     * Once the driver has been shut down it always returns an event, with `shutdown`
     * set, and stores nothing.
     */
    std::optional<IoReadyEvent> poll_ready(IoDirection direction, detail::Context& ctx);

    /**
     * @brief Marks `event.direction` not ready, unless an event has arrived since
     * `event` was observed. Call only after the syscall returned EAGAIN.
     */
    void clear_ready(IoReadyEvent event);

    /// Deregisters the fd; afterwards the fd may be closed. Idempotent.
    ///
    /// A level-triggered registration's waker may call this on its own
    /// registration, or on another one, while the driver is waking it.
    void deregister() noexcept;

    /**
     * @brief Replaces the interest set. Level-triggered registrations only.
     *
     * Takes effect for the next poll. A turn that has already fetched its events may
     * still report a direction that was just removed, so the waker must tolerate a
     * wake for a direction it no longer wants. An error or hang-up is reported
     * whatever the interest set is.
     *
     * @throws std::system_error if the OS rejects the change.
     */
    void set_interest(detail::sys::Interest interest);

    /**
     * @brief Sets the waker the driver wakes each time the fd is ready.
     * Level-triggered registrations only.
     *
     * One waker serves both directions, and it stays set until replaced or until the
     * fd is deregistered. It is woken inside the turn, and once more by
     * IoDriver::shutdown(), after which take_ready() reports `shutdown`.
     *
     * Race (for the caller to handle): a shutdown that began between the
     * constructor and this call has already collected the wakers it will wake, and
     * does not wake this one. A caller that is not itself inside a turn must check
     * `take_ready().shutdown` after setting the waker.
     */
    void set_waker(detail::Weak<detail::Waker> waker);

    /**
     * @brief Returns the readiness reported since the previous call, and clears it.
     * Level-triggered registrations only.
     *
     * Nothing is lost by the clearing: while the fd stays ready, the next turn
     * reports it again.
     */
    IoLevelReady take_ready();

    /**
     * @brief The waiter-list form of poll_ready(), for a primitive that allows any
     * number of concurrent waiters on one direction. Edge-triggered only.
     *
     * Returns an event if `waiter` was woken since it was linked (once per wake),
     * or if the direction is ready; otherwise links `waiter` (or, if it is already
     * linked, refreshes its waker) and returns `std::nullopt`. Once the driver has
     * been shut down it returns an event with `shutdown` set. Executor-safe on any
     * thread, like poll_ready().
     *
     * Precondition: `waiter` is not linked to another registration.
     */
    std::optional<IoReadyEvent> poll_ready(IoDirection direction, detail::Context& ctx,
                                           IoWaiter& waiter);

    /**
     * @brief Unlinks `waiter` if it is linked, and forgets a wake it has not
     * consumed. The owner of a waiter calls it before the waiter is destroyed, for
     * example from a dropped future's destructor. Idempotent.
     */
    void cancel_wait(IoWaiter& waiter) noexcept;

    /**
     * @brief Runs the readiness handshake around one non-blocking operation.
     *
     * `op()` performs the syscall and returns `std::expected<T, int>`, the error being
     * an errno. Loops `poll_ready()` → `op()` → `clear_ready()` while `op()` would
     * block. tokio's `Registration::poll_io`.
     *
     * Once the driver has been shut down, `op()` is not run and the result is the
     * error `sys::kDriverShutDown`: waiting could never end. A future that was already
     * waiting is woken by IoDriver::shutdown() and gets that error from its next poll.
     *
     * @return `std::nullopt` if the direction isn't ready (the context's waker is
     *         stored, and the driver will wake it); otherwise `op()`'s result, either
     *         a value or an error other than would-block.
     */
    template<typename Op>
    auto poll_io(IoDirection direction, detail::Context& ctx, Op&& op)
        -> std::optional<std::invoke_result_t<Op&>>
    {
        for (;;) {
            auto ready = poll_ready(direction, ctx);
            if (!ready) return std::nullopt;
            if (ready->shutdown)
                return std::invoke_result_t<Op&>(std::unexpect, detail::sys::kDriverShutDown);
            auto result = op();
            if (result || !detail::sys::would_block(result.error())) return result;
            // Race (handled): readiness that arrived after op() saw EAGAIN bumped the
            // tick, so clear_ready() leaves it set and the loop retries at once.
            clear_ready(*ready);
        }
    }

    /**
     * @brief The waiter-list form of poll_io(): the same loop, waiting through
     * `waiter` (see poll_ready(direction, ctx, waiter)).
     */
    template<typename Op>
    auto poll_io(IoDirection direction, detail::Context& ctx, IoWaiter& waiter, Op&& op)
        -> std::optional<std::invoke_result_t<Op&>>
    {
        for (;;) {
            auto ready = poll_ready(direction, ctx, waiter);
            if (!ready) return std::nullopt;
            if (ready->shutdown)
                return std::invoke_result_t<Op&>(std::unexpect, detail::sys::kDriverShutDown);
            auto result = op();
            if (result || !detail::sys::would_block(result.error())) return result;
            // As in the slot form: readiness that arrived after op() saw EAGAIN
            // bumped the tick, so clear_ready() leaves it set and the loop retries.
            clear_ready(*ready);
        }
    }

private:
    IoDriver*               m_driver = nullptr;
    detail::sys::RawFd              m_fd = detail::sys::kInvalidFd;
    detail::Rc<detail::ScheduledIo> m_io;
};

/**
 * @brief Parks an executor thread by turning an IoDriver.
 *
 * park(max_wait) is `driver.turn(max_wait)`, every time. How often a busy executor
 * gets here is the executor's business: CurrentThreadExecutor parks once per batch
 * of `event_interval` task polls.
 *
 * Not thread-safe except unpark(): one executor thread owns park().
 */
class IoDriverParker final : public Parker {
public:
    explicit IoDriverParker(IoDriver& driver) : m_driver(driver) {}

    void park(std::optional<std::chrono::nanoseconds> max_wait) override {
        m_driver.turn(max_wait);
    }
    void unpark() noexcept override { m_driver.unpark(); }

private:
    IoDriver& m_driver;
};

} // namespace coro
