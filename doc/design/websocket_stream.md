# WebSocket Stream

`WsStream` and `WsListener` — async WebSocket client and server built on
[libwebsockets](https://libwebsockets.org/) using the shared libuv event loop as the backend.

---

## Overview

`WsStream` exposes the same coroutine-friendly API surface as `TcpStream`:

```cpp
// Connect and perform the WebSocket handshake.
WsStream ws = co_await WsStream::connect("ws://example.com/chat");

// Send a text frame.
co_await ws.send("hello", WsStream::OpCode::Text);

// Receive the next complete message.
WsStream::Message msg = co_await ws.receive();
std::string_view text(reinterpret_cast<const char*>(msg.data.data()), msg.data.size());

// Destructor sends a close frame and tears down gracefully.
```

---

## Event Loop Integration

libwebsockets (lws) has first-class support for an external libuv event loop. Passing
`LWS_SERVER_OPTION_LIBUV` and a pointer to the runtime's `uv_loop_t` causes lws to
register all its internal handles on that loop:

```cpp
lws_context_creation_info info{};
info.options        |= LWS_SERVER_OPTION_LIBUV;
info.foreign_loops[0] = loop;  // the uv_loop_t* owned by IoService
lws_context* ctx = lws_create_context(&info);
```

lws is then driven entirely by the existing I/O thread — no new threads, no polling, no
separate event loop.

### `lws_context` ownership

The `lws_context` is owned by `IoService` directly — it is a second I/O backend alongside
the libuv loop, not a separate service. `IoService` gains a `lws_context*` member
initialised at the **start of `io_thread_loop()`** (not in the constructor), because
`lws_context` creation must happen on the same thread that will drive the loop. It is
destroyed in `stop()` before `uv_loop_close()`.

```
IoService members:
  uv_loop_t    m_uv_loop;      // libuv event loop
  lws_context* m_lws_ctx;      // lws context; created on I/O thread, destroyed in stop()
  uv_async_t   m_async;        // cross-thread doorbell (unchanged)
```

WebSocket operation types (`WsConnectRequest`, `WsSendRequest`, etc.) remain entirely
separate from `IoService`, following the same pattern as `SleepFuture`'s timer requests —
`IoService` submits and dispatches `IoRequest`s without knowing their concrete types.

---

## Key Differences from `TcpStream`

### 1. lws owns the connection, not the caller

With `TcpStream` the library allocates the `uv_tcp_t`, controls its lifetime, and closes
it when the stream is destroyed. With lws, the `lws_wsi*` (WebSocket instance) is owned
by the lws context. Callers receive a raw pointer in callbacks and must never free it
themselves. `WsStream` must hold a raw `lws_wsi*` and null it out when lws delivers a
`LWS_CALLBACK_CLIENT_CLOSED` or `LWS_CALLBACK_CLIENT_CONNECTION_ERROR` event.

The `shared_ptr<Handle>` ownership pattern from `TcpStream` does not apply here. Lifetime
is managed by lws.

### 2. A single callback dispatches all events

lws fires one protocol callback (a C function pointer registered at context creation) for
every event on every connection. The `reason` argument is an enum with ~80 values covering
connection establishment, data receipt, write-readiness, pings, closes, and more. The
relevant subset for a client stream:

| Reason | Meaning |
|---|---|
| `LWS_CALLBACK_CLIENT_ESTABLISHED` | Handshake complete — connection is ready |
| `LWS_CALLBACK_CLIENT_RECEIVE` | A complete (or partial) message frame arrived |
| `LWS_CALLBACK_CLIENT_WRITEABLE` | Safe to call `lws_write()` now |
| `LWS_CALLBACK_CLIENT_CLOSED` | Connection closed by remote or after our close |
| `LWS_CALLBACK_CLIENT_CONNECTION_ERROR` | Handshake or connection failed |

This means a single `protocol_cb` function must dispatch to whichever sub-state is
relevant for each event, rather than having separate `connect_cb`, `read_cb`, `write_cb`
as in `TcpStream`.

### 3. Writing is write-readiness based, not direct

`uv_write()` can be called at any time from the I/O thread — lws forbids this. The correct
pattern is:

1. Caller's `SendFuture` stores the data and submits a `WsWritableRequest` which calls
   `lws_callback_on_writable(wsi)` on the I/O thread.
2. lws fires `LWS_CALLBACK_CLIENT_WRITEABLE` when the connection is ready.
3. `protocol_cb` calls `lws_write()` from within that callback, records the result,
   and wakes the `SendFuture`.

This adds one extra suspension point compared to `TcpStream::write()` but is required by
the lws API contract.

---

## Public API Types

### `WsStream::OpCode`

```cpp
enum class OpCode { Text, Binary };
```

### `WsStream::FrameMode`

```cpp
enum class FrameMode { Full, Partial };
```

`Full` (default): `receive()` returns only after the final fragment — the complete message
is in `Message::data`.

`Partial`: `receive()` returns for each fragment as it arrives; `Message::is_final`
indicates whether this is the last one. Useful for large binary transfers.

Selected at connect time:

```cpp
WsStream ws = co_await WsStream::connect(url);                        // Full
WsStream ws = co_await WsStream::connect(url, FrameMode::Partial);   // Partial
```

### `WsStream::Message`

```cpp
struct Message {
    std::vector<std::byte> data;      // payload bytes
    bool                   is_text;   // true = UTF-8 text frame, false = binary
    bool                   is_final;  // always true in Full mode; may be false in Partial mode
};
```

---

## State and Ownership Model

Because multiple futures (`ConnectFuture`, `ReceiveFuture`, `SendFuture`) share the same
lws connection, all shared state lives in a single `coro::detail::ws::ConnectionState`
struct. This follows the detail-namespace rule: when sibling futures share state, a
dedicated `coro::detail::<op>` namespace is used rather than nesting in one sibling
arbitrarily.

`WsStream` holds a `shared_ptr<ConnectionState>`. Each future also holds one, so the
state remains valid even if `WsStream` is destroyed while an operation is in flight.

### Canonical struct definitions

```cpp
namespace coro::detail::ws {

struct ConnectSubState {
    std::atomic<std::shared_ptr<Waker>> waker;
    std::atomic<bool>                   complete{false};
    int                                 error = 0;   // 0 = success; set before complete=true
};

struct ReceiveSubState {
    std::atomic<std::shared_ptr<Waker>> waker;
    std::atomic<bool>                   complete{false};
    std::atomic<bool>                   cancelled{false};  // set by ReceiveFuture destructor
    std::vector<std::byte>              buffer;    // assembled across partial frames on I/O thread
    bool                                is_text  = false;  // set before complete=true
    bool                                is_final = false;  // set before complete=true
};

struct SendSubState {
    std::atomic<std::shared_ptr<Waker>> waker;
    std::atomic<bool>                   complete{false};
    std::atomic<bool>                   cancelled{false};  // set by SendFuture destructor
    std::span<const std::byte>          data;     // non-owning; caller's buffer must stay alive
    WsStream::OpCode                    opcode  = WsStream::OpCode::Text;
    int                                 error   = 0;   // set before complete=true
};

struct ConnectionState {
    lws*                  wsi        = nullptr;  // owned by lws context — never freed by us
    WsStream::FrameMode   frame_mode = WsStream::FrameMode::Full;  // read-only after connect
    ConnectSubState       connect;
    ReceiveSubState       receive;
    SendSubState          send;
    std::mutex            send_queue_mutex;
    std::deque<SendSubState*> send_queue;        // see Send Queue section
    std::atomic<bool>     closed{false};
};

int protocol_cb(lws* wsi, lws_callback_reasons reason,
                void* user, void* in, std::size_t len);

} // namespace coro::detail::ws
```

---

## `WsConnectRequest` — Bootstrapping per-session user data

`lws_client_connect_via_info` requires the `ConnectionState*` to be available in
`protocol_cb` from the very first callback. lws passes per-session user data via the
`userdata` field of `lws_client_connect_info`. `WsConnectRequest::execute()` stores a
heap-allocated `shared_ptr<ConnectionState>` there before calling connect, so
`protocol_cb` can recover it from the first event onward:

```cpp
struct WsConnectRequest : IoRequest {
    std::shared_ptr<ConnectionState> state;
    std::string                      host;
    std::string                      path;
    uint16_t                         port;
    bool                             tls;

    void execute(uv_loop_t* /*loop*/) override {
        // Heap-allocate a shared_ptr wrapper — protocol_cb recovers it via
        // lws_get_opaque_user_data() and deletes it in LWS_CALLBACK_CLIENT_CLOSED.
        auto* sp = new std::shared_ptr<ConnectionState>(state);

        lws_client_connect_info ci{};
        ci.context    = /* IoService::m_lws_ctx, passed in at construction */;
        ci.address    = host.c_str();
        ci.port       = port;
        ci.path       = path.c_str();
        ci.ssl_connection = tls ? LCCSCF_USE_SSL : 0;
        ci.userdata   = sp;   // recovered by protocol_cb as shared_ptr<ConnectionState>*

        state->wsi = lws_client_connect_via_info(&ci);
        if (!state->wsi) {
            delete sp;
            state->connect.error = -1;
            state->connect.complete.store(true, std::memory_order_release);
            if (auto w = state->connect.waker.load()) w->wake();
        }
    }
};
```

`WsConnectRequest` needs access to `IoService::m_lws_ctx`. The simplest approach is to
pass the `lws_context*` into the request at construction time (obtained via a new
`IoService::lws_context()` accessor).

---

## `protocol_cb` Dispatch

```cpp
int protocol_cb(lws* wsi, lws_callback_reasons reason, void* /*user*/,
                void* in, std::size_t len) {

    auto* sp = static_cast<std::shared_ptr<ConnectionState>*>(
                   lws_wsi_user(wsi));
    if (!sp) return 0;
    auto& state = **sp;

    switch (reason) {

    case LWS_CALLBACK_CLIENT_ESTABLISHED:
        state.connect.complete.store(true, std::memory_order_release);
        if (auto w = state.connect.waker.load()) w->wake();
        break;

    case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
        state.connect.error = -1;  // TODO: surface lws error string
        state.connect.complete.store(true, std::memory_order_release);
        if (auto w = state.connect.waker.load()) w->wake();
        break;

    case LWS_CALLBACK_CLIENT_RECEIVE: {
        // Discard if ReceiveFuture was dropped.
        if (state.receive.cancelled.load(std::memory_order_acquire)) {
            state.receive.buffer.clear();
            break;
        }
        auto* bytes = static_cast<const std::byte*>(in);
        state.receive.buffer.insert(state.receive.buffer.end(), bytes, bytes + len);
        bool final_fragment = lws_is_final_fragment(wsi);

        if (state.frame_mode == WsStream::FrameMode::Partial || final_fragment) {
            state.receive.is_text    = (lws_frame_is_binary(wsi) == 0);
            state.receive.is_final   = final_fragment;
            state.receive.complete.store(true, std::memory_order_release);
            if (auto w = state.receive.waker.load()) w->wake();
        }
        break;
    }

    case LWS_CALLBACK_CLIENT_WRITEABLE: {
        std::lock_guard lk(state.send_queue_mutex);
        if (state.send_queue.empty()) break;
        auto* sub = state.send_queue.front();
        state.send_queue.pop_front();

        if (!sub->cancelled.load(std::memory_order_acquire)) {
            // lws_write requires LWS_PRE bytes of padding before the payload.
            std::vector<std::byte> buf(LWS_PRE + sub->data.size());
            std::copy(sub->data.begin(), sub->data.end(), buf.begin() + LWS_PRE);
            int flags = (sub->opcode == WsStream::OpCode::Binary)
                            ? LWS_WRITE_BINARY : LWS_WRITE_TEXT;
            int r = lws_write(wsi,
                              reinterpret_cast<unsigned char*>(buf.data() + LWS_PRE),
                              sub->data.size(),
                              static_cast<lws_write_protocol>(flags));
            sub->error = (r < 0) ? r : 0;
            sub->data  = {};
            sub->complete.store(true, std::memory_order_release);
            if (auto w = sub->waker.load()) w->wake();
        }

        // If more sends are queued, request another WRITEABLE callback.
        if (!state.send_queue.empty())
            lws_callback_on_writable(wsi);
        break;
    }

    case LWS_CALLBACK_CLIENT_CLOSED:
        state.wsi = nullptr;
        state.closed.store(true, std::memory_order_release);
        // Wake any futures blocked on receive or send so they can return an error.
        if (auto w = state.receive.waker.load()) w->wake();
        {
            std::lock_guard lk(state.send_queue_mutex);
            for (auto* sub : state.send_queue) {
                sub->error = -1;
                sub->complete.store(true, std::memory_order_release);
                if (auto w = sub->waker.load()) w->wake();
            }
            state.send_queue.clear();
        }
        // Delete the heap-allocated shared_ptr wrapper — last ref may free ConnectionState.
        delete sp;
        break;

    default:
        break;
    }
    return 0;
}
```

---

## Send Future — Extra Suspension Point

Unlike `TcpStream::WriteFuture`, `SendFuture::poll()` cannot issue the write directly.
It must enqueue the send and request write-readiness:

```
SendFuture::poll() #1:
    push SendSubState* onto state.send_queue (under mutex)
    submit WsWritableRequest → calls lws_callback_on_writable(wsi) on I/O thread
    store waker, return PollPending

LWS_CALLBACK_CLIENT_WRITEABLE fires:
    protocol_cb pops SendSubState* from queue
    calls lws_write(), sets complete=true, wakes future

SendFuture::poll() #2:
    complete == true → check error → return PollReady or PollError
```

---

## Send Cancellation

Dropping a `SendFuture` before it completes sets `cancelled = true` on its `SendSubState`.
`protocol_cb` checks this flag in `LWS_CALLBACK_CLIENT_WRITEABLE` before calling
`lws_write()`, skipping the write if the future is gone. The data span is never read after
`cancelled` is set, so there is no dangling-pointer hazard.

```cpp
// SendFuture destructor — worker thread.
~SendFuture() {
    if (!m_sub_state->complete.load(std::memory_order_acquire))
        m_sub_state->cancelled.store(true, std::memory_order_release);
}
```

Because `cancelled` and `complete` are both atomics, the destructor and `protocol_cb`
cannot both act on the data simultaneously.

---

## ConnectFuture Cancellation

Dropping a `ConnectFuture` mid-handshake is handled the same way: a `cancelled` flag on
`ConnectSubState`. `LWS_CALLBACK_CLIENT_ESTABLISHED` checks it and, if set, immediately
submits a `WsCloseRequest` rather than waking a future that no longer exists. The `wsi`
remains valid until `LWS_CALLBACK_CLIENT_CLOSED` fires and nulls it.

```cpp
struct ConnectSubState {
    std::atomic<std::shared_ptr<Waker>> waker;
    std::atomic<bool>                   complete{false};
    std::atomic<bool>                   cancelled{false};  // set by ConnectFuture destructor
    int                                 error = 0;
};
```

---

## Receive Cancellation

Dropping a `ReceiveFuture` sets `cancelled` on `ReceiveSubState`. In
`LWS_CALLBACK_CLIENT_RECEIVE`, if `cancelled` is set, any buffered data is discarded and
no wake is issued. In `Partial` mode this prevents unbounded buffer growth when the caller
abandons a mid-stream receive.

---

## Frame Mode — Full vs. Partial

`receive()` behaviour is controlled by `FrameMode` stored in `ConnectionState`
(set at connect time, read-only thereafter). `protocol_cb` reads it from `ConnectionState`
— not from a member variable, since `protocol_cb` is a plain C function with no `this`.

In `Partial` mode, after the caller consumes a fragment, `WsStream::receive()` resets
`ReceiveSubState` (clears buffer, sets `complete = false`) before returning, so the next
`receive()` call sees a clean slate.

---

## Send Queue (Backpressure)

`ConnectionState::send_queue` holds raw pointers to `SendSubState` objects owned by their
respective `SendFuture`s. Each `SendFuture` pushes its `SendSubState*` onto the queue
(under `send_queue_mutex`) and submits a `WsWritableRequest`. `LWS_CALLBACK_CLIENT_WRITEABLE`
pops and processes one entry per callback, then calls `lws_callback_on_writable()` again
if more remain.

This allows multiple concurrent `send()` calls without waiting for each to complete.
If the send queue interaction with cancellation proves complex during implementation,
fall back to a single-in-flight constraint enforced by an assertion and defer the queue
to a future version.

---

## LWS_PRE Buffer Padding

lws requires `LWS_PRE` bytes of free space before the payload pointer passed to
`lws_write()`. `protocol_cb` copies the payload into a `LWS_PRE`-padded local buffer for
each write. A future optimisation could expose a `send_with_padding()` API letting callers
pre-allocate padded buffers to avoid the copy.

---

## TLS (`wss://`)

lws handles TLS internally when the `tls` flag is set in `WsConnectRequest`. The caller
may pass a CA certificate path in `lws_context_creation_info` or use the system store. No
changes to the callback model or future types are needed — TLS is transparent at the API
level.

---

## URL Parsing

`connect(url)` accepts a full URL string. A small internal helper in `coro::detail::ws`
parses it into the fields `lws_client_connect_via_info` requires:

```cpp
struct ParsedUrl {
    std::string host;
    std::string path;
    uint16_t    port;
    bool        tls;
};
ParsedUrl parse_ws_url(std::string_view url);  // throws std::invalid_argument on bad input
```

Handles four forms: `ws://host/path`, `ws://host:port/path`, and their `wss://`
equivalents. Default ports: 80 for `ws://`, 443 for `wss://`.

---

## Shutdown

`WsStream`'s destructor submits a `WsCloseRequest` to the I/O thread, which calls
`lws_close_reason()` to initiate a clean WebSocket close (sends a Close frame and waits
for the echo). `LWS_CALLBACK_CLIENT_CLOSED` then nulls `state->wsi`, wakes any pending
futures with an error, and deletes the `shared_ptr<ConnectionState>` wrapper.

Forceful teardown (e.g. runtime shutdown before the close handshake completes) is handled
by `lws_context_destroy()`, called from `IoService::stop()` on the I/O thread before
`uv_loop_close()`. This closes all open connections synchronously.

---

## Protocol Negotiation (`Sec-WebSocket-Protocol`)

The WebSocket opening handshake optionally includes a `Sec-WebSocket-Protocol` header that
lets clients advertise one or more application-level subprotocols they understand, and
servers echo back the one they selected.

### Default behaviour — no subprotocol

By default, `WsStream` and `WsListener` omit `Sec-WebSocket-Protocol` entirely:

```cpp
// Client — no Sec-WebSocket-Protocol header sent.
WsStream ws = co_await WsStream::connect("ws://localhost:9001/");

// Server — accepts connections regardless of any subprotocol the client requests.
WsListener listener = co_await WsListener::bind("127.0.0.1", 9001);
```

This makes both classes behave like a general-purpose WebSocket transport (similar to
Python's `websockets` library), compatible with any peer without additional configuration.

On the lws side, `nullptr` is passed as the protocol name in `lws_protocols`:

```cpp
const lws_protocols protocols[] = {
    { nullptr, server_protocol_cb, sizeof(void*), 4096, 0, nullptr, 0 },
    { nullptr, nullptr, 0, 0, 0, nullptr, 0 }  // terminator
};
```

For the client, `ci.protocol = nullptr` omits the header from the HTTP upgrade request.

### Opting in to a specific subprotocol

Pass an optional `subprotocols` vector to advertise or restrict to named protocols:

```cpp
// Client advertises support for "chat.v1" and "chat.v2".
WsStream ws = co_await WsStream::connect(
    "wss://example.com/chat", WsStream::FrameMode::Full, {"chat.v1", "chat.v2"});

// Server only accepts clients requesting "chat.v1".
WsListener listener = co_await WsListener::bind(
    "0.0.0.0", 9001, {"chat.v1"});
```

Multiple names are joined with commas and passed directly to lws:

```cpp
// "chat.v1,chat.v2" → Sec-WebSocket-Protocol: chat.v1, chat.v2
ci.protocol = subprotocol_str.empty() ? nullptr : subprotocol_str.c_str();
```

The subprotocol string only needs to be alive for the duration of
`lws_client_connect_via_info()` / `lws_create_context()`, so it is stored in the
`WsConnectRequest` / `WsBindRequest` struct and freed after `execute()` returns.

---

## Future Enhancements (Out of Scope for v1)

- **DNS resolution timeout:** lws resolves DNS internally via `getaddrinfo`. Exposing a
  configurable timeout requires hooking into lws's async DNS path.
- **Ping / Pong:** lws responds to unsolicited pings automatically. Exposing an explicit
  `ping()` future for application-level heartbeats is straightforward but not required for v1.
- **Zero-copy sends:** expose `send_with_padding()` so callers can pre-allocate
  `LWS_PRE`-padded buffers and avoid the copy in `protocol_cb`.
