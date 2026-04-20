#pragma once

#include <coro/detail/context.h>
#include <coro/detail/poll_result.h>
#include <coro/detail/waker.h>
#include <coro/io/byte_buffer.h>
#include <coro/task/join_handle.h>
#include <any>
#include <cstddef>
#include <memory>
#include <mutex>
#include <ranges>
#include <string>
#include <system_error>
#include <utility>

namespace coro {

// ---------------------------------------------------------------------------
// PipeMode — flags for Pipe::open()
// ---------------------------------------------------------------------------

enum class PipeMode : unsigned {
    Read      = 0x01,  // O_RDONLY — blocks until a writer opens the other end
    Write     = 0x02,  // O_WRONLY — blocks until a reader opens the other end
    ReadWrite = 0x03,  // O_RDWR  — never blocks
};

// ---------------------------------------------------------------------------
// Internal request types
// ---------------------------------------------------------------------------

namespace detail {

// WriteRequest — owns the write buffer and tracks per-request completion.
struct WriteRequest {
    std::any               buf_owner;
    const char*            base      = nullptr;
    std::size_t            size      = 0;
    bool                   completed = false;  // GUARDED BY mutex
    std::error_code        error;              // GUARDED BY mutex
    std::mutex             mutex;
    std::shared_ptr<Waker> waker;              // GUARDED BY mutex
};

// ReadRequest — owns the read buffer and tracks per-request completion.
struct ReadRequest {
    std::any               buf_owner;
    char*                  base     = nullptr;
    std::size_t            capacity = 0;
    std::size_t            filled    = 0;      // GUARDED BY mutex
    bool                   completed = false;  // GUARDED BY mutex
    std::error_code        error;              // GUARDED BY mutex
    std::mutex             mutex;
    std::shared_ptr<Waker> waker;              // GUARDED BY mutex
};

// Request factories — move buf into std::any and extract a stable raw pointer.
// Requests are heap-allocated (shared_ptr), so the std::any address is stable
// regardless of SBO; the extracted char* remains valid for the I/O lifetime.
template<ByteBuffer Buf>
std::shared_ptr<WriteRequest> make_write_request(Buf buf) {
    auto req       = std::make_shared<WriteRequest>();
    req->buf_owner = std::move(buf);
    auto& stored   = std::any_cast<Buf&>(req->buf_owner);
    req->base      = reinterpret_cast<const char*>(std::ranges::data(stored));
    req->size      = std::ranges::size(stored);
    return req;
}

template<ByteBuffer Buf>
std::shared_ptr<ReadRequest> make_read_request(Buf buf) {
    auto req       = std::make_shared<ReadRequest>();
    req->buf_owner = std::move(buf);
    auto& stored   = std::any_cast<Buf&>(req->buf_owner);
    req->base      = reinterpret_cast<char*>(std::ranges::data(stored));
    req->capacity  = std::ranges::size(stored);
    return req;
}

struct PipeState;  // fully defined in pipe.cpp

// Push helpers called from the Pipe::write / Pipe::read templates in pipe.hpp.
// PipeState is incomplete here; the definitions in pipe.cpp see the full type.
void push_write_request(PipeState& state, std::shared_ptr<WriteRequest> req);
void push_read_request (PipeState& state, std::shared_ptr<ReadRequest>  req);

} // namespace detail

// ---------------------------------------------------------------------------
// WriteHandle — Future<void> that resolves when a queued write completes.
// ---------------------------------------------------------------------------

class [[nodiscard]] WriteHandle {
public:
    using OutputType = void;

    explicit WriteHandle(std::shared_ptr<detail::WriteRequest> req)
        : m_req(std::move(req)) {}
    WriteHandle(WriteHandle&&)            noexcept = default;
    WriteHandle& operator=(WriteHandle&&) noexcept = default;
    WriteHandle(const WriteHandle&)                = delete;
    WriteHandle& operator=(const WriteHandle&)     = delete;

    PollResult<void> poll(detail::Context& ctx);

private:
    std::shared_ptr<detail::WriteRequest> m_req;
};

// ---------------------------------------------------------------------------
// ReadHandle<Buf> — Future<pair<size_t,Buf>> that resolves when a read completes.
// ---------------------------------------------------------------------------

template<ByteBuffer Buf>
class [[nodiscard]] ReadHandle {
public:
    using OutputType = std::pair<std::size_t, Buf>;

    explicit ReadHandle(std::shared_ptr<detail::ReadRequest> req)
        : m_req(std::move(req)) {}
    ReadHandle(ReadHandle&&)            noexcept = default;
    ReadHandle& operator=(ReadHandle&&) noexcept = default;
    ReadHandle(const ReadHandle&)                = delete;
    ReadHandle& operator=(const ReadHandle&)     = delete;

    PollResult<std::pair<std::size_t, Buf>> poll(detail::Context& ctx) {
        std::lock_guard lock(m_req->mutex);
        if (!m_req->completed) {
            m_req->waker = ctx.getWaker();
            return PollPending;
        }
        if (m_req->error)
            return PollError(std::make_exception_ptr(
                std::system_error(m_req->error, "Pipe::read")));
        Buf buf = std::any_cast<Buf>(std::move(m_req->buf_owner));
        return std::pair<std::size_t, Buf>{m_req->filled, std::move(buf)};
    }

private:
    std::shared_ptr<detail::ReadRequest> m_req;
};

// ---------------------------------------------------------------------------
// Pipe
// ---------------------------------------------------------------------------

/**
 * @brief Async named-pipe (FIFO) handle backed by libuv stream I/O.
 *
 * Obtain a `Pipe` via `co_await Pipe::open(path, mode)`. Create the FIFO on
 * disk first with `co_await Pipe::create(path)`.
 *
 * `write(buf)` and `read(buf)` are regular (non-coroutine) functions that
 * queue the operation and return a handle immediately without suspending.
 * `co_await`-ing the handle waits for completion. Multiple requests can be
 * in-flight simultaneously; write requests are batched into a single
 * `uv_write` call per driver wake cycle, amortising cross-thread scheduling.
 *
 * **Concurrency:** a `Pipe` must not be shared across tasks without external
 * synchronisation.
 */
class Pipe {
public:
    using OpenFuture   = JoinHandle<Pipe>;
    using CreateFuture = JoinHandle<void>;

    Pipe(Pipe&&) noexcept;
    Pipe& operator=(Pipe&&) noexcept;
    Pipe(const Pipe&)            = delete;
    Pipe& operator=(const Pipe&) = delete;

    /// Signals background drivers to stop and closes the pipe asynchronously.
    ~Pipe();

    /**
     * @brief Opens an existing FIFO at `path`. Suspends until both ends are open.
     * @throws std::system_error on failure.
     */
    [[nodiscard]] static OpenFuture open(std::string path, PipeMode mode);

    /**
     * @brief Creates a FIFO at `path` via mkfifo(3). EEXIST is silently ignored.
     * @param permission Unix permission bits (default 0666, subject to umask).
     * @throws std::system_error on failure.
     */
    [[nodiscard]] static CreateFuture create(std::string path, int permission = 0666);

    /**
     * @brief Queues a write and returns a handle immediately (no suspension).
     *
     * The buffer is moved into the request. `co_await` the returned handle to
     * wait for completion. Multiple in-flight writes are batched into a single
     * `uv_write` call.
     */
    template<ByteBuffer Buf>
    [[nodiscard]] WriteHandle write(Buf buf);

    /**
     * @brief Queues a read and returns a handle immediately (no suspension).
     *
     * The buffer is moved into the request. `co_await` the returned handle to
     * get `{bytes_read, buf}`. Multiple reads can be pre-queued; the driver
     * fills them in order as data arrives.
     */
    template<ByteBuffer Buf>
    [[nodiscard]] ReadHandle<Buf> read(Buf buf);

private:
    explicit Pipe(std::shared_ptr<detail::PipeState> state);
    std::shared_ptr<detail::PipeState> m_state;
};

} // namespace coro

#include <coro/io/pipe.hpp>
