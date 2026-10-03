#pragma once

#include <coro/coro.h>
#include <coro/detail/context.h>
#include <coro/detail/poll_result.h>
#include <coro/detail/rc.h>
#include <coro/detail/waker.h>
#include <libwebsockets.h>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// Forward-declare the shared connection state so WsStream's nested Future classes
// can hold shared_ptr<> to it without requiring the full definition here.
namespace coro::detail::ws {
    struct ConnectionState;
    struct SendSubState;
    class LwsService;
}

namespace coro {

/**
 * @brief Async WebSocket client stream.
 *
 * Built on libwebsockets. Every lws call runs on a dedicated lws service thread (see
 * doc/design/websocket_stream.md, "Service threads"), so WsStream needs neither the
 * IoDriver nor any particular executor. Obtain one via `co_await WsStream::connect(url)`
 * or `co_await listener.accept()`.
 *
 * **Frame modes:**
 * - `FrameMode::Full` (default) — `receive()` returns after the complete message is assembled.
 * - `FrameMode::Partial` — `receive()` returns for each fragment; check `Message::is_final`.
 *
 * **Concurrency:** only one `receive()` and one `send()` may be in flight at a time unless
 * the send queue is enabled (see design doc § Send Queue). `WsStream` must not be shared
 * across tasks.
 *
 * All lws handles, protocol callbacks, and request types are private implementation details.
 */
class WsStream {
public:
    // -----------------------------------------------------------------------
    // Public API types
    // -----------------------------------------------------------------------

    enum class OpCode { Text, Binary };
    enum class FrameMode { Full, Partial };

    /**
     * @brief Per-connection options for WsStream::connect().
     *
     * All fields have sensible defaults; use designated initializers to set
     * only what you need:
     * @code
     *   co_await WsStream::connect(url, { .max_message_size = 1 << 20 });
     * @endcode
     */
    struct Options {
        FrameMode                frame_mode       = FrameMode::Full;
        std::vector<std::string> subprotocols     = {};   ///< Sec-WebSocket-Protocol to advertise.
        std::size_t              max_message_size = 0;    ///< 0 = unlimited.
    };

    /**
     * @brief A received WebSocket message (or fragment in Partial mode).
     */
    struct Message {
        std::string_view as_text() const {
            if (!is_text) throw std::logic_error("Message::as_text: not a text frame");
            return std::string_view(reinterpret_cast<const char*>(data.data()), data.size());
        }
        std::vector<std::byte> data;      ///< Payload bytes.
        bool                   is_text;   ///< true = text frame, false = binary.
        bool                   is_final;  ///< Always true in Full mode; may be false in Partial mode.
    };

    // -----------------------------------------------------------------------
    // Nested Future types — public because they appear in return types.
    // As nested classes they have full access to WsStream's private members.
    // -----------------------------------------------------------------------

    /**
     * @brief Future<Message> returned by @ref WsStream::receive().
     *
     * Resolves to the oldest message in `ReceiveSubState::ready`. The receive callback
     * queues each complete message (Full mode) or fragment (Partial mode) there as it
     * arrives, whether or not a receive is pending, and wakes this future.
     *
     * Cancel-safe: dropping this future before it resolves loses nothing. Messages that
     * arrive meanwhile stay queued for the next `receive()`.
     */
    class ReceiveFuture {
    public:
        using OutputType = Message;

        ReceiveFuture(std::shared_ptr<detail::ws::ConnectionState> state,
                      std::shared_ptr<detail::ws::LwsService>      service);

        ReceiveFuture(ReceiveFuture&&) noexcept            = default;
        ReceiveFuture& operator=(ReceiveFuture&&) noexcept = default;
        ReceiveFuture(const ReceiveFuture&)                = delete;
        ReceiveFuture& operator=(const ReceiveFuture&)     = delete;

        PollResult<Message> poll(detail::Context& ctx);

    private:
        std::shared_ptr<detail::ws::ConnectionState> m_state;
        // Unused by poll(); held so the service, and the thread that wakes this
        // future, outlive it.
        std::shared_ptr<detail::ws::LwsService>      m_service;
    };

    /**
     * @brief Future<void> returned by @ref WsStream::send().
     *
     * Owns a heap-allocated `SendSubState`. On first poll, pushes it onto the connection's
     * send queue and posts `lws_callback_on_writable()` to the lws service thread. The
     * WRITEABLE callback pops the entry, calls `lws_write()`, and wakes the future.
     *
     * Dropping this future sets `cancelled` on the `SendSubState`; the service thread
     * skips the write if the flag is set when WRITEABLE fires.
     *
     * @note The caller's data span must remain valid until the future resolves.
     */
    class SendFuture {
    public:
        using OutputType = void;

        SendFuture(std::shared_ptr<detail::ws::ConnectionState> state,
                   std::span<const std::byte>                    data,
                   OpCode                                        opcode,
                   std::shared_ptr<detail::ws::LwsService>       service);
        ~SendFuture();

        SendFuture(SendFuture&&) noexcept            = default;
        SendFuture& operator=(SendFuture&&) noexcept = default;
        SendFuture(const SendFuture&)                = delete;
        SendFuture& operator=(const SendFuture&)     = delete;

        PollResult<void> poll(detail::Context& ctx);

    private:
        std::shared_ptr<detail::ws::ConnectionState> m_state;
        std::shared_ptr<detail::ws::SendSubState>    m_sub_state;
        std::shared_ptr<detail::ws::LwsService>      m_service;
        bool                                         m_started = false;
    };

    // -----------------------------------------------------------------------
    // WsStream public API
    // -----------------------------------------------------------------------

    WsStream(WsStream&&) noexcept;
    WsStream& operator=(WsStream&&) noexcept;
    WsStream(const WsStream&)            = delete;
    WsStream& operator=(const WsStream&) = delete;

    /// Posts a graceful close (Close frame + echo) to the lws service thread. Does not
    /// block, unless this drops the last reference to the service: then it waits for
    /// the service thread to exit, which closes the remaining connections abruptly.
    ~WsStream();

    /**
     * @brief Connects to a WebSocket server and performs the opening handshake.
     *
     * A host name is resolved with `lookup_host()` (on the blocking pool), then each
     * address is tried in turn until one completes the handshake, as
     * `TcpStream::connect()` does. The Host header (and TLS SNI) carry the name from the
     * URL, not the address.
     *
     * @param url Full URL: `ws://host[:port]/path` or `wss://host[:port]/path`.
     * @param options Frame mode, advertised subprotocols (none by default, which
     *        accepts any server) and the largest message accepted.
     * @throws std::invalid_argument if the URL cannot be parsed.
     * @throws std::system_error with `dns_error_category()` if the host doesn't
     *         resolve, or the last address's failure (e.g. `ECONNREFUSED`) if none
     *         connects.
     */
    [[nodiscard]] static Coro<WsStream> connect(std::string url);
    [[nodiscard]] static Coro<WsStream> connect(std::string url, Options options);

    /**
     * @brief Receives the next message (or fragment in Partial mode).
     * @return A `ReceiveFuture` resolving to a @ref Message.
     */
    [[nodiscard]] ReceiveFuture receive();

    /**
     * @brief Sends a binary or text frame.
     * @note The caller's data span must remain valid until the future resolves.
     */
    [[nodiscard]] SendFuture send(std::span<const std::byte> data,
                                  OpCode                     opcode = OpCode::Binary);

    /// Convenience overload — sends a UTF-8 text frame.
    [[nodiscard]] SendFuture send(std::string_view text);

private:
    class ConnectAttempt;

    WsStream(std::shared_ptr<detail::ws::ConnectionState> state,
             std::shared_ptr<detail::ws::LwsService>      service);

    // WsListener::AcceptFuture constructs WsStream from server-accepted connections.
    friend class WsListener;

    std::shared_ptr<detail::ws::ConnectionState> m_state;
    std::shared_ptr<detail::ws::LwsService>      m_service;
};

} // namespace coro

// =============================================================================
// coro::detail::ws — canonical shared-state definitions
//
// Defined after WsStream so these structs can reference WsStream::OpCode and
// WsStream::FrameMode without a circular dependency. Nothing outside this header
// and ws_stream.cpp should include or name these types directly.
// =============================================================================

namespace coro::detail::ws {

// ---------------------------------------------------------------------------
// Sub-states — one per operation type, embedded in ConnectionState.
//
// Each is shared between a future (on an executor thread) and the lws callbacks (on
// the lws service thread), under its own mutex. Wakers are stored weak, as the
// IoDriver stores them: the executor's task list owns a waiting task, and a task
// that has gone away is simply not woken. They are fired outside the mutex.
// ---------------------------------------------------------------------------

struct ConnectSubState {
    // mutex guards everything here. Both the client callback (service thread) and
    // ConnectAttempt (executor thread) hold it.
    std::mutex                       mutex;
    detail::Weak<detail::Waker>      waker;
    bool                             complete  = false;
    bool                             cancelled = false;  // the ConnectAttempt was dropped
    int                              error     = 0;      // errno; set before complete
    std::string                      reason;             // lws's text for a failure
};

// One entry of ReceiveSubState::ready: a complete message (Full mode), a fragment
// (Partial mode), or an error (e.g. EMSGSIZE) that receive() throws in its place.
struct ReceivedMessage {
    std::vector<std::byte> data;
    bool                   is_text  = false;
    bool                   is_final = false;
    int                    error    = 0;
};

struct ReceiveSubState {
    // mutex guards everything here. Both the receive callbacks (service thread) and
    // ReceiveFuture::poll (executor thread) hold it.
    std::mutex                       mutex;
    detail::Weak<detail::Waker>      waker;
    std::vector<std::byte>           buffer;              // message being assembled
    std::size_t                      message_size = 0;    // bytes of it so far, for max_message_size
    bool                             discarding   = false;  // dropping the rest of an oversized message
    // Filled by the service thread, drained in order by receive().
    // FIXME: unbounded -- rx flow control isn't applied, so a peer sending faster than the
    // application receives grows this without limit.
    std::deque<ReceivedMessage>      ready;
};

struct SendSubState {
    // mutex guards everything here. The WRITEABLE callback checks cancelled, then
    // reads data, under the lock; ~SendFuture sets cancelled under it. So the
    // callback can't pass the check and then read a span whose buffer the caller
    // has just freed.
    std::mutex                       mutex;
    detail::Weak<detail::Waker>      waker;
    bool                             complete  = false;
    bool                             cancelled = false;  // set by ~SendFuture
    std::span<const std::byte>       data;               // non-owning; caller keeps alive
    coro::WsStream::OpCode           opcode = coro::WsStream::OpCode::Text;
    int                              error  = 0;
};

// ---------------------------------------------------------------------------
// ConnectionState — all per-connection state shared by the futures and the lws
// callbacks. Reference-counted so any future can outlive the WsStream.
//
// While lws has the connection, `self` keeps the state alive and lws holds only a
// raw pointer to it (client: the wsi user pointer; server: the per-session slot).
// close_connection() drops `self` when lws reports the connection gone.
// ---------------------------------------------------------------------------

struct ConnectionState {
    // Service thread only: the callbacks and posted commands read and write these,
    // and nothing else does.
    lws*                             wsi     = nullptr;  // owned by the lws context
    std::shared_ptr<ConnectionState> self;
    // Set by a posted close; checked in the WRITEABLE callback, which calls
    // lws_close_reason() then returns -1 to start the close handshake. (lws only
    // starts a close from inside a protocol callback.)
    bool                             closing = false;

    coro::WsStream::FrameMode    frame_mode       = coro::WsStream::FrameMode::Full;
    std::size_t                  max_message_size = 0;  // 0 = unlimited; enforced in on_receive
    ConnectSubState              connect;
    ReceiveSubState              receive;
    std::mutex                   send_queue_mutex;
    std::deque<std::shared_ptr<SendSubState>> send_queue;  // shared_ptr keeps sub-state alive
    // Written by the service thread, read by the futures. Atomic rather than under a
    // mutex: it only ever goes false -> true, and each reader re-checks it under the
    // sub-state mutex that close_connection() takes before waking (see there).
    std::atomic<bool>            closed{false};
};

// ---------------------------------------------------------------------------
// URL parsing helper
// ---------------------------------------------------------------------------

struct ParsedUrl {
    std::string host;
    std::string path;
    uint16_t    port;
    bool        tls;
};

/// Parses a ws:// or wss:// URL into its components.
/// @throws std::invalid_argument on malformed input.
ParsedUrl parse_ws_url(std::string_view url);

// ---------------------------------------------------------------------------
// The lws callback logic. All of it runs on the lws service thread.
// ---------------------------------------------------------------------------

/// The client protocol callback, registered by LwsService::client().
int protocol_cb(lws* wsi, lws_callback_reasons reason,
                void* user, void* in, std::size_t len);

/// RECEIVE, shared by the client and server callbacks: appends one fragment of an
/// incoming message and, once the message is complete (or per fragment in Partial
/// mode), queues it on state.receive.ready and wakes the receiver.
void on_receive(ConnectionState& state, std::span<const std::byte> fragment,
                bool is_text, bool is_final_fragment);

/// WRITEABLE, shared by the client and server callbacks: starts a requested close,
/// or writes the front of the send queue. Returns the callback's result.
int on_writeable(ConnectionState& state, lws* wsi);

/// The connection is gone (CLOSED, a connect error, or the wsi being destroyed):
/// fails a pending connect with `connect_error`, marks the state closed, wakes the
/// receiver, fails the queued sends, and drops `self`. Idempotent. `state` may be
/// freed on return.
void close_connection(ConnectionState& state, int connect_error, std::string reason = {});

/// Posts a graceful close of `state`'s connection, if it's still open.
void post_close(LwsService& service, std::shared_ptr<ConnectionState> state);

} // namespace coro::detail::ws
