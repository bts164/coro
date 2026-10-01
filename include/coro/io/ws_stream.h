#pragma once

#include <coro/detail/context.h>
#include <coro/detail/poll_result.h>
#include <coro/detail/waker.h>
#include <coro/runtime/single_threaded_uv_executor.h>
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
 * Built on libwebsockets using the shared libuv event loop owned by the uv executor.
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
     * @brief Future<WsStream> returned by @ref WsStream::connect().
     *
     * On first poll, submits a `WsConnectRequest` to the uv executor, which calls
     * `lws_client_connect_via_info()` on the uv thread. When lws fires
     * `LWS_CALLBACK_CLIENT_ESTABLISHED` (or `_CONNECTION_ERROR`), the future is woken.
     *
     * Dropping this future before it completes sets `cancelled` on the connect sub-state;
     * `LWS_CALLBACK_CLIENT_ESTABLISHED` will then submit a close rather than waking nobody.
     */
    class ConnectFuture {
    public:
        using OutputType = WsStream;

        ConnectFuture(std::string url, Options options, SingleThreadedUvExecutor* uv_exec);
        ~ConnectFuture();

        ConnectFuture(ConnectFuture&&) noexcept            = default;
        ConnectFuture& operator=(ConnectFuture&&) noexcept = default;
        ConnectFuture(const ConnectFuture&)                = delete;
        ConnectFuture& operator=(const ConnectFuture&)     = delete;

        PollResult<WsStream> poll(detail::Context& ctx);

    private:
        std::string                                  m_url;
        Options                                      m_options;
        SingleThreadedUvExecutor*                    m_uv_exec;
        std::shared_ptr<detail::ws::ConnectionState> m_state;  // null until first poll
    };

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
                      SingleThreadedUvExecutor*                                    uv_exec);

        ReceiveFuture(ReceiveFuture&&) noexcept            = default;
        ReceiveFuture& operator=(ReceiveFuture&&) noexcept = default;
        ReceiveFuture(const ReceiveFuture&)                = delete;
        ReceiveFuture& operator=(const ReceiveFuture&)     = delete;

        PollResult<Message> poll(detail::Context& ctx);

    private:
        std::shared_ptr<detail::ws::ConnectionState> m_state;
        SingleThreadedUvExecutor*                                   m_uv_exec;
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
                   SingleThreadedUvExecutor*                                    uv_exec);
        ~SendFuture();

        SendFuture(SendFuture&&) noexcept            = default;
        SendFuture& operator=(SendFuture&&) noexcept = default;
        SendFuture(const SendFuture&)                = delete;
        SendFuture& operator=(const SendFuture&)     = delete;

        PollResult<void> poll(detail::Context& ctx);

    private:
        std::shared_ptr<detail::ws::ConnectionState> m_state;
        std::shared_ptr<detail::ws::SendSubState>    m_sub_state;
        SingleThreadedUvExecutor*                                   m_uv_exec;
        bool                                         m_started = false;
    };

    // -----------------------------------------------------------------------
    // WsStream public API
    // -----------------------------------------------------------------------

    WsStream(WsStream&&) noexcept;
    WsStream& operator=(WsStream&&) noexcept;
    WsStream(const WsStream&)            = delete;
    WsStream& operator=(const WsStream&) = delete;

    /// Submits a graceful close (Close frame + echo) on the uv executor. Does not block.
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
    [[nodiscard]] static ConnectFuture connect(std::string url);
    [[nodiscard]] static ConnectFuture connect(std::string url, Options options);

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
                      SingleThreadedUvExecutor*                                    uv_exec);

    // WsListener::AcceptFuture constructs WsStream from server-accepted connections.
    friend class WsListener;

    std::shared_ptr<detail::ws::ConnectionState> m_state;
    SingleThreadedUvExecutor*                                   m_uv_exec = nullptr;
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

// One entry of ReceiveSubState::ready: a complete message (Full mode), a fragment
// (Partial mode), or an error (e.g. EMSGSIZE) that receive() throws in its place.
struct ReceivedMessage {
    std::vector<std::byte> data;
    bool                   is_text  = false;
    bool                   is_final = false;
    int                    error    = 0;
};

struct ReceiveSubState {
    // mutex guards everything below except waker. Both the receive callbacks (I/O thread)
    // and ReceiveFuture::poll (worker thread) must hold it.
    std::mutex                                         mutex;
    std::atomic<std::shared_ptr<coro::detail::Waker>> waker;
    std::vector<std::byte>                             buffer;            // message being assembled
    std::size_t                                        message_size = 0;  // bytes of it so far, for max_message_size
    bool                                               discarding = false;  // dropping the rest of an oversized message
    // Filled by the I/O thread, drained in order by receive().
    // FIXME: unbounded -- rx flow control isn't applied, so a peer sending faster than the
    // application receives grows this without limit.
    std::deque<ReceivedMessage>                        ready;
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
    lws*                         wsi              = nullptr;  // owned by lws context — never freed here
    coro::WsStream::FrameMode    frame_mode       = coro::WsStream::FrameMode::Full;
    std::size_t                  max_message_size = 0;  // 0 = unlimited; enforced in receive callbacks
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

// ---------------------------------------------------------------------------
// on_receive -- the RECEIVE logic shared by the client (protocol_cb) and server
// (WsListener) callbacks: appends one fragment of an incoming message and, once
// the message is complete (or per fragment in Partial mode), queues it on
// state.receive.ready and wakes the receiver. Runs on the I/O thread.
// ---------------------------------------------------------------------------
void on_receive(ConnectionState& state, std::span<const std::byte> fragment,
                bool is_text, bool is_final_fragment);

} // namespace coro::detail::ws
