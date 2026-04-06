#pragma once

#include <coro/detail/context.h>
#include <coro/detail/poll_result.h>
#include <coro/detail/waker.h>
#include <coro/runtime/io_service.h>
#include <uv.h>
#include <atomic>
#include <cstddef>
#include <memory>
#include <span>
#include <string>

namespace coro {

/**
 * @brief Async TCP connection. Satisfies move-only ownership of a connected socket.
 *
 * Obtain a `TcpStream` via `co_await TcpStream::connect(host, port)`.
 * Once connected, use `read()` and `write()` to transfer data. The destructor
 * closes the socket asynchronously via @ref IoService.
 *
 * **Concurrency:** a `TcpStream` must not be shared across tasks. Only one
 * `read()` or `write()` future may be in flight at a time.
 *
 * **Cancellation:** mid-operation cancellation (dropping a `ReadFuture` or
 * `WriteFuture` before it completes) is not safe in this initial implementation.
 * The caller's buffer must remain valid until the future resolves.
 * TODO: implement safe cancellation via a cancel flag + uv_read_stop/uv_cancel.
 *
 * All libuv handles, request structs, and callbacks are private implementation
 * details of this class — @ref IoService has no knowledge of TCP internals.
 */
class TcpStream {
private:
    // -----------------------------------------------------------------------
    // Handle — heap-allocated uv_tcp_t wrapper. Address must remain stable
    // for the lifetime of the handle, hence the shared_ptr indirection.
    // -----------------------------------------------------------------------
    struct Handle {
        uv_tcp_t handle;  // must be first field
    };

    // -----------------------------------------------------------------------
    // ConnectState — shared between ConnectFuture and the I/O thread.
    //
    // `req` is passed to uv_tcp_connect(); its address must be stable until
    // connect_cb fires, hence ConnectState is heap-allocated via shared_ptr.
    //
    // RACE: `waker` written by worker thread, read by I/O thread (connect_cb).
    //       `status` written by I/O thread (connect_cb), read by worker (poll).
    //       Use acquire/release on `status` to synchronise the waker store.
    // -----------------------------------------------------------------------
    struct ConnectState {
        std::shared_ptr<Handle>                     tcp;
        uv_connect_t                                req;    // passed to uv_tcp_connect; must be stable
        std::atomic<std::shared_ptr<detail::Waker>> waker;
        std::atomic<int>                            status{INT_MIN};  // INT_MIN = pending
    };

    // -----------------------------------------------------------------------
    // ReadState — shared between ReadFuture and the I/O thread.
    //
    // `buf` is a non-owning view into the caller's buffer. The caller must
    // keep the buffer alive until the future resolves (see class-level note).
    //
    // RACE: `waker` written by worker, read by I/O thread (read_cb).
    //       `nread`/`error` written by I/O thread before `complete` is set
    //       with release; worker reads them after seeing `complete` with acquire.
    // -----------------------------------------------------------------------
    struct ReadState {
        std::shared_ptr<Handle>                     tcp;
        std::span<std::byte>                        buf;
        std::atomic<std::shared_ptr<detail::Waker>> waker;
        std::atomic<bool>                           complete{false};
        bool                                        started = false; // set on first poll, worker thread only
        ssize_t                                     nread = 0;   // set before complete=true
        int                                         error = 0;   // set before complete=true
    };

    // -----------------------------------------------------------------------
    // WriteState — shared between WriteFuture and the I/O thread.
    //
    // `req` is passed to uv_write(); its address must be stable until
    // write_cb fires. `buf_desc` points into the caller's data span.
    //
    // RACE: same acquire/release pattern as ReadState via `complete`.
    // -----------------------------------------------------------------------
    struct WriteState {
        std::shared_ptr<Handle>                     tcp;
        uv_write_t                                  req;      // passed to uv_write; must be stable
        uv_buf_t                                    buf_desc; // points into caller's data — caller keeps alive
        std::atomic<std::shared_ptr<detail::Waker>> waker;
        std::atomic<bool>                           complete{false};
        bool                                        started = false; // set on first poll, worker thread only
        int                                         error = 0;  // set before complete=true
    };

    // -----------------------------------------------------------------------
    // IoRequest subtypes — submitted via IoService::submit(), executed on
    // the I/O thread. Each holds a shared_ptr to its operation's state so
    // the state outlives the submitting future if the future is destroyed
    // before the request is processed.
    // -----------------------------------------------------------------------

    struct ConnectRequest : IoRequest {
        std::shared_ptr<ConnectState> state;
        std::string                   host;
        uint16_t                      port;

        ConnectRequest(std::shared_ptr<ConnectState> s, std::string h, uint16_t p)
            : state(std::move(s)), host(std::move(h)), port(p) {}

        void execute(uv_loop_t* loop) override;
    };

    struct ReadRequest : IoRequest {
        std::shared_ptr<ReadState> state;
        explicit ReadRequest(std::shared_ptr<ReadState> s) : state(std::move(s)) {}
        void execute(uv_loop_t* loop) override;
    };

    struct WriteRequest : IoRequest {
        std::shared_ptr<WriteState> state;
        explicit WriteRequest(std::shared_ptr<WriteState> s) : state(std::move(s)) {}
        void execute(uv_loop_t* loop) override;
    };

    struct CloseRequest : IoRequest {
        // Holds the Handle alive until close_cb fires, at which point the
        // shared_ptr<Handle>* wrapper stored in handle.data is deleted, and
        // this shared_ptr is released — freeing the Handle if the last ref.
        std::shared_ptr<Handle> handle;
        explicit CloseRequest(std::shared_ptr<Handle> h) : handle(std::move(h)) {}
        void execute(uv_loop_t* loop) override;
    };

    // -----------------------------------------------------------------------
    // libuv callbacks — all run exclusively on the I/O thread.
    // -----------------------------------------------------------------------

    /// Fired by libuv when the TCP handshake completes (success or failure).
    static void connect_cb(uv_connect_t* req, int status);

    /// Called by libuv to request a read buffer before data arrives.
    static void alloc_cb(uv_handle_t* handle, std::size_t suggested_size, uv_buf_t* buf);

    /// Fired by libuv when data has been read into the buffer provided by alloc_cb.
    static void read_cb(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf);

    /// Fired by libuv when a uv_write() request completes.
    static void write_cb(uv_write_t* req, int status);

    /// Fired by libuv after uv_close() completes. Frees the Handle.
    static void close_cb(uv_handle_t* handle);

public:
    // -----------------------------------------------------------------------
    // Public nested Future types — returned by connect(), read(), write().
    // As nested classes they have full access to TcpStream's private types.
    // -----------------------------------------------------------------------

    /**
     * @brief Future<TcpStream> returned by @ref TcpStream::connect().
     *
     * On first poll, submits a `ConnectRequest` to the @ref IoService which
     * calls `uv_tcp_connect()` on the I/O thread. When the OS completes the
     * handshake, `connect_cb` wakes this future. The next poll constructs and
     * returns the `TcpStream`.
     *
     * @throws std::system_error on connection failure (wraps the libuv error code).
     */
    class ConnectFuture {
    public:
        using OutputType = TcpStream;

        ConnectFuture(std::string host, uint16_t port, IoService* io_service);

        ConnectFuture(ConnectFuture&&) noexcept            = default;
        ConnectFuture& operator=(ConnectFuture&&) noexcept = default;
        ConnectFuture(const ConnectFuture&)                = delete;
        ConnectFuture& operator=(const ConnectFuture&)     = delete;

        PollResult<TcpStream> poll(detail::Context& ctx);

    private:
        std::string m_host;
        uint16_t    m_port;
        IoService*  m_io_service;
        std::shared_ptr<ConnectState> m_state;  // null until first poll
    };

    /**
     * @brief Future<std::size_t> returned by @ref TcpStream::read().
     *
     * On first poll, submits a `ReadRequest` which calls `uv_read_start()`.
     * When data arrives, `read_cb` stops reading, records the byte count, and
     * wakes the future. Returns 0 on clean EOF.
     *
     * @note The caller's buffer must remain valid until the future resolves.
     *       Do not drop this future while a read is in flight.
     *       TODO: implement safe cancellation.
     */
    class ReadFuture {
    public:
        using OutputType = std::size_t;

        ReadFuture(std::shared_ptr<Handle> handle,
                   std::span<std::byte>   buf,
                   IoService*             io_service);

        ReadFuture(ReadFuture&&) noexcept            = default;
        ReadFuture& operator=(ReadFuture&&) noexcept = default;
        ReadFuture(const ReadFuture&)                = delete;
        ReadFuture& operator=(const ReadFuture&)     = delete;

        PollResult<std::size_t> poll(detail::Context& ctx);

    private:
        IoService*                 m_io_service;
        std::shared_ptr<ReadState> m_state;
    };

    /**
     * @brief Future<void> returned by @ref TcpStream::write().
     *
     * On first poll, submits a `WriteRequest` which calls `uv_write()`.
     * Libuv guarantees the full buffer is written before calling `write_cb`.
     * `write_cb` records the status and wakes the future.
     *
     * @throws std::system_error on write failure (wraps the libuv error code).
     *
     * @note The caller's data span must remain valid until the future resolves.
     *       TODO: implement safe cancellation via uv_cancel.
     */
    class WriteFuture {
    public:
        using OutputType = void;

        WriteFuture(std::shared_ptr<Handle>    handle,
                    std::span<const std::byte> data,
                    IoService*                 io_service);

        WriteFuture(WriteFuture&&) noexcept            = default;
        WriteFuture& operator=(WriteFuture&&) noexcept = default;
        WriteFuture(const WriteFuture&)                = delete;
        WriteFuture& operator=(const WriteFuture&)     = delete;

        PollResult<void> poll(detail::Context& ctx);

    private:
        IoService*                  m_io_service;
        std::shared_ptr<WriteState> m_state;
    };

    // -----------------------------------------------------------------------
    // TcpStream public API
    // -----------------------------------------------------------------------

    TcpStream(TcpStream&&) noexcept;
    TcpStream& operator=(TcpStream&&) noexcept;
    TcpStream(const TcpStream&)            = delete;
    TcpStream& operator=(const TcpStream&) = delete;

    /// Closes the socket asynchronously via IoService. Does not block.
    ~TcpStream();

    /**
     * @brief Resolves `host` and performs an async TCP connect to `port`.
     * @return A `ConnectFuture` that resolves to a connected `TcpStream`.
     */
    [[nodiscard]] static ConnectFuture connect(std::string host, uint16_t port);

    /**
     * @brief Reads up to `buf.size()` bytes from the stream.
     * @return A `ReadFuture` that resolves to the number of bytes read, or 0 on EOF.
     */
    [[nodiscard]] ReadFuture read(std::span<std::byte> buf);

    /**
     * @brief Writes all of `data` to the stream.
     * @return A `WriteFuture` that resolves when the write is fully handed to the OS.
     */
    [[nodiscard]] WriteFuture write(std::span<const std::byte> data);

private:
    // -----------------------------------------------------------------------
    // TcpStream data members
    // -----------------------------------------------------------------------
    explicit TcpStream(std::shared_ptr<Handle> handle, IoService* io_service);

    std::shared_ptr<Handle> m_handle;
    IoService*              m_io_service = nullptr;
};

} // namespace coro
