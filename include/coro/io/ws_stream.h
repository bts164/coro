#pragma once

#include <coro/detail/context.h>
#include <coro/detail/poll_result.h>
#include <coro/detail/waker.h>
#include <coro/runtime/io_service.h>
#include <libwebsockets.h>
#include <atomic>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Forward-declare the shared connection state so WsStream's nested Future classes
// can hold shared_ptr<> to it without requiring the full definition here.
namespace coro::detail::ws {
    struct ConnectionState;
    struct SendSubState;
}

namespace coro {

/**
 * @brief Async WebSocket client stream.
 *
 * Built on libwebsockets using the shared libuv event loop owned by @ref IoService.
 * Obtain a `WsStream` via `co_await WsStream::connect(url)`.
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
 * @ref IoService has no knowledge of WebSocket internals.
 *
 * See doc/websocket_stream.md for the full design.
 */
class WsStream {
public:
    // -----------------------------------------------------------------------
    // Public API types
    // -----------------------------------------------------------------------

    enum class OpCode { Text, Binary };
    enum class FrameMode { Full, Partial };

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
     * @brief Future<WsStream> returned by @ref WsStream::connect().
     *
     * On first poll, submits a `WsConnectRequest` to @ref IoService, which calls
     * `lws_client_connect_via_info()` on the I/O thread. When lws fires
     * `LWS_CALLBACK_CLIENT_ESTABLISHED` (or `_CONNECTION_ERROR`), the future is woken.
     *
     * Dropping this future before it completes sets `cancelled` on the connect sub-state;
     * `LWS_CALLBACK_CLIENT_ESTABLISHED` will then submit a close rather than waking nobody.
     */
    class ConnectFuture {
    public:
        using OutputType = WsStream;

        ConnectFuture(std::string url, FrameMode frame_mode,
                      std::vector<std::string> subprotocols, IoService* io_service);
        ~ConnectFuture();

        ConnectFuture(ConnectFuture&&) noexcept            = default;
        ConnectFuture& operator=(ConnectFuture&&) noexcept = default;
        ConnectFuture(const ConnectFuture&)                = delete;
        ConnectFuture& operator=(const ConnectFuture&)     = delete;

        PollResult<WsStream> poll(detail::Context& ctx);

    private:
        std::string                                  m_url;
        FrameMode                                    m_frame_mode;
        std::vector<std::string>                     m_subprotocols;
        IoService*                                   m_io_service;
        std::shared_ptr<detail::ws::ConnectionState> m_state;  // null until first poll
    };

    /**
     * @brief Future<Message> returned by @ref WsStream::receive().
     *
     * On first poll, stores the waker in `ReceiveSubState`. When
     * `LWS_CALLBACK_CLIENT_RECEIVE` assembles a complete message (Full mode) or any
     * fragment (Partial mode), it wakes this future.
     *
     * Dropping this future sets `cancelled`; buffered data is discarded on the I/O thread.
     */
    class ReceiveFuture {
    public:
        using OutputType = Message;

        ReceiveFuture(std::shared_ptr<detail::ws::ConnectionState> state,
                      IoService*                                    io_service);
        ~ReceiveFuture();

        ReceiveFuture(ReceiveFuture&&) noexcept            = default;
        ReceiveFuture& operator=(ReceiveFuture&&) noexcept = default;
        ReceiveFuture(const ReceiveFuture&)                = delete;
        ReceiveFuture& operator=(const ReceiveFuture&)     = delete;

        PollResult<Message> poll(detail::Context& ctx);

    private:
        std::shared_ptr<detail::ws::ConnectionState> m_state;
        IoService*                                   m_io_service;
        // Set to true when poll() returns PollReady so the destructor knows the
        // future was already consumed and must not set cancelled on the shared state.
        bool                                         m_done = false;
    };

    /**
     * @brief Future<void> returned by @ref WsStream::send().
     *
     * Owns a heap-allocated `SendSubState`. On first poll, pushes it onto the connection's
     * send queue and submits a `WsWritableRequest` to call `lws_callback_on_writable()`.
     * `LWS_CALLBACK_CLIENT_WRITEABLE` pops the entry, calls `lws_write()`, and wakes the
     * future.
     *
     * Dropping this future sets `cancelled` on the `SendSubState`; the I/O thread skips
     * the write if the flag is set when `WRITEABLE` fires.
     *
     * @note The caller's data span must remain valid until the future resolves.
     */
    class SendFuture {
    public:
        using OutputType = void;

        SendFuture(std::shared_ptr<detail::ws::ConnectionState> state,
                   std::span<const std::byte>                    data,
                   OpCode                                        opcode,
                   IoService*                                    io_service);
        ~SendFuture();

        SendFuture(SendFuture&&) noexcept            = default;
        SendFuture& operator=(SendFuture&&) noexcept = default;
        SendFuture(const SendFuture&)                = delete;
        SendFuture& operator=(const SendFuture&)     = delete;

        PollResult<void> poll(detail::Context& ctx);

    private:
        std::shared_ptr<detail::ws::ConnectionState> m_state;
        std::shared_ptr<detail::ws::SendSubState>    m_sub_state;
        IoService*                                   m_io_service;
        bool                                         m_started = false;
    };

    // -----------------------------------------------------------------------
    // WsStream public API
    // -----------------------------------------------------------------------

    WsStream(WsStream&&) noexcept;
    WsStream& operator=(WsStream&&) noexcept;
    WsStream(const WsStream&)            = delete;
    WsStream& operator=(const WsStream&) = delete;

    /// Submits a graceful close (Close frame + echo) via IoService. Does not block.
    ~WsStream();

    /**
     * @brief Connects to a WebSocket server and performs the opening handshake.
     * @param url Full URL: `ws://host[:port]/path` or `wss://host[:port]/path`.
     * @param frame_mode Controls whether `receive()` delivers full messages or fragments.
     * @param subprotocols Optional list of application-level subprotocols to advertise in the
     *        `Sec-WebSocket-Protocol` request header. When empty (the default), no subprotocol
     *        header is sent, which accepts any server regardless of what it speaks.
     * @return A `ConnectFuture` that resolves to a connected `WsStream`.
     * @throws std::invalid_argument if the URL cannot be parsed.
     * @throws std::system_error on connection failure.
     */
    [[nodiscard]] static ConnectFuture connect(std::string url,
                                               FrameMode   frame_mode = FrameMode::Full,
                                               std::vector<std::string> subprotocols = {});

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
    explicit WsStream(std::shared_ptr<detail::ws::ConnectionState> state,
                      IoService*                                    io_service);

    // WsListener::AcceptFuture constructs WsStream from server-accepted connections.
    friend class WsListener;

    std::shared_ptr<detail::ws::ConnectionState> m_state;
    IoService*                                   m_io_service = nullptr;
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
// RACE notes: fields written by the I/O thread and read by a worker thread
// (or vice versa) are std::atomic. Plain fields are written by the I/O thread
// before setting the atomic `complete` flag (release), and read by the worker
// after seeing `complete` (acquire) — no additional synchronisation needed.
// ---------------------------------------------------------------------------

struct ConnectSubState {
    // mutex guards waker, complete, and error.
    // Both protocol_cb (I/O thread) and ConnectFuture::poll (worker thread) must hold
    // it when reading or writing any of these fields to avoid race conditions.
    std::mutex                                         mutex;
    std::atomic<std::shared_ptr<coro::detail::Waker>> waker;
    bool                                               complete{false};
    std::atomic<bool>                                  cancelled{false};  // set by ConnectFuture dtor
    int                                                error = 0;         // set before complete=true
};

struct ReceiveSubState {
    // mutex guards buffer, is_text, is_final, complete, and cancelled.
    // Both protocol_cb (I/O thread) and ReceiveFuture::poll (worker thread) must hold
    // it when reading or writing any of these fields.
    std::mutex                                         mutex;
    std::atomic<std::shared_ptr<coro::detail::Waker>> waker;
    bool                                               complete{false};
    bool                                               cancelled{false};  // set by ReceiveFuture dtor
    std::vector<std::byte>                             buffer;    // assembled on I/O thread
    bool                                               is_text  = false;
    bool                                               is_final = false;
};

struct SendSubState {
    // mutex guards data, opcode, error, complete, and cancelled.
    // The I/O thread (WRITEABLE callback) checks cancelled then reads data under the lock.
    // SendFuture::~SendFuture sets cancelled under the lock, preventing a window where
    // the I/O thread passes the cancelled check and then reads a dangling data span after
    // the caller's buffer has been freed.
    std::mutex                                         mutex;
    std::atomic<std::shared_ptr<coro::detail::Waker>> waker;
    bool                                               complete{false};
    bool                                               cancelled{false};  // set by SendFuture dtor
    std::span<const std::byte>                         data;     // non-owning; caller keeps alive
    coro::WsStream::OpCode                             opcode = coro::WsStream::OpCode::Text;
    int                                                error  = 0;
};

// ---------------------------------------------------------------------------
// ConnectionState — owns all per-connection state shared across futures and
// the I/O thread. Heap-allocated and reference-counted so any future can
// outlive WsStream itself without dangling references.
// ---------------------------------------------------------------------------

struct ConnectionState {
    lws*                         wsi        = nullptr;  // owned by lws context — never freed here
    coro::WsStream::FrameMode    frame_mode = coro::WsStream::FrameMode::Full;
    ConnectSubState              connect;
    ReceiveSubState              receive;
    std::mutex                   send_queue_mutex;
    std::deque<std::shared_ptr<SendSubState>> send_queue;  // shared_ptr keeps sub-state alive
    std::atomic<bool>            closed{false};
    // Set by WsCloseRequest; checked in the WRITEABLE callback, which calls
    // lws_close_reason() then returns -1 to trigger the lws close handshake.
    // lws_close_reason() outside a callback is a no-op; the close must be
    // initiated by returning -1 from within a protocol callback.
    std::atomic<bool>            closing{false};
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
// protocol_cb — registered with lws at context creation; dispatches all
// WebSocket events to the appropriate ConnectionState sub-state.
// Runs exclusively on the I/O thread.
// ---------------------------------------------------------------------------
int protocol_cb(lws* wsi, lws_callback_reasons reason,
                void* user, void* in, std::size_t len);

} // namespace coro::detail::ws
