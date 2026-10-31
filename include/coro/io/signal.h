#pragma once

// Desktop only: signals as futures and streams, on the IoDriver. See
// doc/design/signal_handling.md.
//
//   Every platform: ctrl_c(), ctrl_c_stream(), shutdown_requested()
//   POSIX:          signal(signum), signal_stream({signums...})
//   Windows:        windows::console_event(event), windows::console_event_stream({...})

#include <coro/detail/context.h>
#include <coro/detail/poll_result.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>

#ifndef _WIN32
#include <csignal>  // SIGINT etc.: callers name the signals they pass
#endif

namespace coro {

/**
 * @brief One coalesced batch of deliveries for a single signal number.
 *
 * Yielded by @ref SignalStream. `signum` is the POSIX signal number, or on Windows the
 * `windows::ConsoleEvent` value. `count` is a guaranteed **lower bound** on how many
 * times `signum` actually fired since the previous item for that signum was yielded:
 * never an overcount, but possibly an undercount for standard (non-realtime) signals
 * the kernel coalesced before the handler ran. See doc/design/signal_handling.md
 * ("Delivery counting").
 */
struct SignalEvent {
    int      signum;
    uint64_t count;
};

namespace detail {
struct SignalState;   // defined in signal.cpp
} // namespace detail

/**
 * @brief Stream of coalesced signal-delivery events. Obtain via @ref ctrl_c_stream,
 * @ref signal_stream or @ref windows::console_event_stream.
 *
 * Satisfies the @ref Stream concept (`ItemType = SignalEvent`). Never exhausts.
 * Watching starts when the stream is created, not at its first poll, and stops
 * synchronously when it is destroyed.
 *
 * Only one task may poll a given stream at a time.
 */
class [[nodiscard]] SignalStream {
public:
    using ItemType = SignalEvent;

    explicit SignalStream(std::unique_ptr<detail::SignalState> state) noexcept;

    SignalStream(SignalStream&&) noexcept;
    SignalStream& operator=(SignalStream&&) noexcept;
    SignalStream(const SignalStream&)            = delete;
    SignalStream& operator=(const SignalStream&) = delete;

    /// Stops watching. The last watcher of a signal restores its previous action.
    ~SignalStream();

    PollResult<std::optional<SignalEvent>> poll_next(detail::Context& ctx);

private:
    std::unique_ptr<detail::SignalState> m_state;
};

/**
 * @brief Future resolving on the first delivery of `signum` after it was created.
 * Obtain via @ref ctrl_c, @ref signal or @ref windows::console_event.
 *
 * Satisfies the @ref Future concept (`OutputType = void`). A leaf future: dropping it
 * at any time just stops watching.
 */
class [[nodiscard]] SignalFuture {
public:
    using OutputType = void;

    explicit SignalFuture(std::unique_ptr<detail::SignalState> state) noexcept;

    SignalFuture(SignalFuture&&) noexcept;
    SignalFuture& operator=(SignalFuture&&) noexcept;
    SignalFuture(const SignalFuture&)            = delete;
    SignalFuture& operator=(const SignalFuture&) = delete;

    /// Stops watching. The last watcher of a signal restores its previous action.
    ~SignalFuture();

    PollResult<void> poll(detail::Context& ctx);

private:
    std::unique_ptr<detail::SignalState> m_state;
};

/**
 * @brief Resolves once, on the next Ctrl-C, on every platform: `SIGINT` on POSIX,
 * `CTRL_C_EVENT` on Windows. The portable way to stop on Ctrl-C.
 *
 * Watching starts immediately, so a Ctrl-C between this call and the first `co_await`
 * is not missed. While any watcher exists, Ctrl-C no longer ends the process; once the
 * last is dropped, it does again. Otherwise as @ref signal on POSIX and
 * @ref windows::console_event on Windows, including their exceptions.
 *
 * On Windows a process with no console (a GUI application, or one started detached)
 * receives no console events, so this never resolves there.
 */
[[nodiscard]] SignalFuture ctrl_c();

/// @brief Yields a coalesced SignalEvent for the Ctrl-Cs since the last item: the
/// stream form of @ref ctrl_c, for "first Ctrl-C stops gracefully, second forces it".
/// `signum` is `SIGINT` on POSIX and `windows::ConsoleEvent::CtrlC` on Windows.
[[nodiscard]] SignalStream ctrl_c_stream();

/// Which requests @ref shutdown_requested watches.
struct ShutdownOptions {
    /// POSIX: also watch SIGHUP, sent when the controlling terminal closes (the
    /// counterpart of Windows' Close). Turn it off in a daemon that uses SIGHUP to mean
    /// "reload". Windows: ignored; Close is always watched, as nothing else can keep the
    /// process alive through it.
    bool hangup = true;
};

class ShutdownFuture;

/**
 * @brief Held while the application cleans up after a shutdown request. The output
 * of @ref shutdown_requested.
 *
 * While it exists, the signals or console events the future watched stay watched:
 *
 *  - Windows: Close, Logoff and Shutdown end the process as soon as the console handler
 *    returns. coro's handler doesn't return while a guard exists, so the process ends
 *    when the guard is dropped, or when Windows' own deadline expires (about 5 s for
 *    Close, about 20 s for Logoff and Shutdown), whichever is first.
 *  - POSIX: the process isn't ended by the request, so the guard holds nothing open; it
 *    only absorbs a repeated SIGTERM or SIGHUP during the cleanup. Once it is dropped
 *    (and nothing else watches them) they get their previous action back, by default
 *    ending the process.
 *
 * So the portable pattern is: keep the guard until the cleanup is done, then return
 * from main.
 *
 * Move-only. A moved-from guard holds nothing.
 */
class [[nodiscard]] ShutdownGuard {
public:
    ShutdownGuard(ShutdownGuard&& other) noexcept;
    ShutdownGuard& operator=(ShutdownGuard&& other) noexcept;
    ShutdownGuard(const ShutdownGuard&)            = delete;
    ShutdownGuard& operator=(const ShutdownGuard&) = delete;

    /// Stops watching. On Windows this lets the handler return, and so the process end.
    ~ShutdownGuard();

private:
    friend class ShutdownFuture;
    explicit ShutdownGuard(const detail::SignalState& state);
    void release() noexcept;

    std::array<int, 3> m_signums{};   // at most three: Close, Logoff, Shutdown
    std::size_t        m_count = 0;   // 0: holds nothing
};

/**
 * @brief Future resolving to a @ref ShutdownGuard on the first shutdown request after
 * it was created. Obtain via @ref shutdown_requested.
 *
 * Satisfies the @ref Future concept (`OutputType = ShutdownGuard`). Until it resolves,
 * the future itself watches; the guard starts watching before the future stops, so
 * there is no gap in which Windows can end the process.
 */
class [[nodiscard]] ShutdownFuture {
public:
    using OutputType = ShutdownGuard;

    explicit ShutdownFuture(std::unique_ptr<detail::SignalState> state) noexcept;

    ShutdownFuture(ShutdownFuture&&) noexcept;
    ShutdownFuture& operator=(ShutdownFuture&&) noexcept;
    ShutdownFuture(const ShutdownFuture&)            = delete;
    ShutdownFuture& operator=(const ShutdownFuture&) = delete;
    ~ShutdownFuture();

    PollResult<ShutdownGuard> poll(detail::Context& ctx);

private:
    std::unique_ptr<detail::SignalState> m_state;
};

/**
 * @brief Resolves when the process is asked to go away, on every platform, with a guard
 * to hold while cleaning up.
 *
 *  - POSIX: SIGTERM (`kill`, `systemctl stop`, `docker stop`), and SIGHUP (the terminal
 *    closed) unless `options.hangup` is false. A signal that is ignored when this is
 *    called (SIGHUP under `nohup`) is left alone.
 *  - Windows: the console events Close (the console window closed), Logoff and Shutdown.
 *    A console process receives Close for logoff and shutdown too; Logoff and Shutdown
 *    reach services only. A process with no console receives none of them.
 *
 * Ctrl-C isn't included; watch it with @ref ctrl_c, and `select` on both to stop on
 * either.
 *
 * @code
 * auto guard = co_await coro::shutdown_requested();
 * co_await flush_and_disconnect();
 * co_return;   // `guard` is dropped here
 * @endcode
 *
 * Hold the guard until the cleanup is done: on Windows the process ends as soon as it
 * is dropped. Discarding it (`co_await coro::shutdown_requested();` alone) lets Windows
 * end the process straight away. The time allowed differs: Windows ends the process
 * about 5 s after Close whatever the guard does, while e.g. systemd waits 90 s after
 * SIGTERM by default before sending SIGKILL.
 *
 * Watching starts immediately, so a request between this call and the first `co_await`
 * is not missed.
 *
 * @throws std::system_error if the handler can't be installed.
 * @throws std::logic_error if the current Runtime's executor doesn't turn the IoDriver.
 */
[[nodiscard]] ShutdownFuture shutdown_requested(ShutdownOptions options = {});

#ifndef _WIN32

/**
 * @brief Resolves once, on the next delivery of `signum`. POSIX only.
 *
 * Watching starts immediately, so a signal delivered between this call and the first
 * `co_await` is not missed. While any watcher of `signum` exists, its previous action
 * (e.g. SIGINT's default, terminate) is replaced by coro's handler; the previous action
 * is restored when the last watcher is dropped. A handler the application installed
 * itself is replaced too, not chained.
 *
 * @throws std::system_error EINVAL if `signum` can't be caught (SIGKILL, SIGSTOP, or out
 *         of range).
 * @throws std::logic_error if the current Runtime's executor doesn't turn the IoDriver.
 */
[[nodiscard]] SignalFuture signal(int signum);

/// @brief Yields a coalesced SignalEvent for every distinct watched signal that has
/// fired at least once since the last item. Same watching rules and exceptions as
/// @ref signal. POSIX only.
[[nodiscard]] SignalStream signal_stream(std::initializer_list<int> signums);

#else  // _WIN32

namespace windows {

/**
 * @brief A console control event, Windows' counterpart of a signal. The values are
 * Windows' own `CTRL_*_EVENT` constants.
 */
enum class ConsoleEvent : int {
    CtrlC     = 0,  ///< CTRL_C_EVENT: Ctrl-C
    CtrlBreak = 1,  ///< CTRL_BREAK_EVENT: Ctrl-Break
    /// CTRL_CLOSE_EVENT: the console window is being closed. Ends the process; watch
    /// it with @ref coro::shutdown_requested.
    Close     = 2,
    /// CTRL_LOGOFF_EVENT: the user is logging off. Received by services only (a
    /// console process gets Close instead). Ends the process; see
    /// @ref coro::shutdown_requested.
    Logoff    = 5,
    /// CTRL_SHUTDOWN_EVENT: the system is shutting down. Received by services only (a
    /// console process gets Close instead). Ends the process; see
    /// @ref coro::shutdown_requested.
    Shutdown  = 6,
};

/**
 * @brief Resolves once, on the next `event`: CtrlC or CtrlBreak. Windows only.
 *
 * Watching starts immediately, so an event between this call and the first `co_await`
 * is not missed. While any watcher of `event` exists it is handled, so e.g. Ctrl-C no
 * longer ends the process; once the last is dropped, Windows' default (ending the
 * process) applies again. Handlers the application installed with
 * SetConsoleCtrlHandler() still run: coro's passes an event nobody watches on to them.
 *
 * A process with no console (a GUI application, or one started detached) receives no
 * console events.
 *
 * @throws std::invalid_argument for Close, Logoff or Shutdown, which end the process
 *         and are watched with @ref coro::shutdown_requested instead.
 * @throws std::system_error EINVAL for a value that isn't a console event, or if the
 *         handler can't be installed.
 * @throws std::logic_error if the current Runtime's executor doesn't turn the IoDriver.
 */
[[nodiscard]] SignalFuture console_event(ConsoleEvent event);

/// @brief Yields a coalesced SignalEvent (`signum` = the ConsoleEvent value) for every
/// distinct watched event that has fired at least once since the last item. Same
/// events, watching rules and exceptions as @ref console_event.
[[nodiscard]] SignalStream console_event_stream(std::initializer_list<ConsoleEvent> events);

} // namespace windows

#endif  // _WIN32

} // namespace coro
