# Pipe — Zero-Copy Pipelined I/O

## Motivation

The current `Pipe` implementation issues a `uv_fs_read` or `uv_fs_write` per call, each
requiring a cross-thread dispatch to the uv thread and a full round-trip wait. For a
single sequential caller this overhead dominates regardless of how much data is transferred.

The goal is zero-copy I/O (caller's buffer passed directly to libuv) with amortised
scheduling overhead. These two goals are reconciled by **separating queueing from
awaiting**: `pipe.write` and `pipe.read` are regular functions that move the buffer into a
request queue and return a handle immediately — no suspension. The caller co_awaits the
handle when it needs the result. Multiple requests can be in flight simultaneously, and
the background driver on the uv thread processes all queued requests in one wake cycle.

---

## API

`pipe.write` and `pipe.read` are regular (non-coroutine) functions. They queue the
request and return a handle synchronously. The handle is a `[[nodiscard]]` awaitable
that suspends when `co_await`ed until the driver signals completion.

```cpp
// Queues the write and returns a handle immediately. Does not suspend.
template <ByteBuffer Buf>
[[nodiscard]] WriteHandle Pipe::write(Buf buf);

// Queues the read and returns a handle immediately. Does not suspend.
template <ByteBuffer Buf>
[[nodiscard]] ReadHandle<Buf> Pipe::read(Buf buf);

// Suspends until all previously issued writes have completed.
[[nodiscard]] Coro<void> Pipe::flush();
```

---

## Usage examples

### Sequential (common case)

For a single caller that issues one operation at a time, the API looks identical to
before. The scheduling overhead is the same as the current implementation.

```cpp
// Write one packet, wait for delivery.
auto wh = pipe.write(std::move(packet_buf));
co_await wh;

// Read into a buffer, get back how many bytes arrived.
auto rh = pipe.read(std::move(recv_buf));
auto [n, buf] = co_await rh;
```

### Pipelined writes (amortised scheduling overhead)

Queue multiple writes before awaiting any. The driver wakes once and issues a single
`uv_write` with all buffers — one scheduling event services all of them.

```cpp
auto h1 = pipe.write(std::move(header_buf));
auto h2 = pipe.write(std::move(payload_buf));
auto h3 = pipe.write(std::move(trailer_buf));
// All three are in the write queue. Driver wakes once, issues one uv_write for all.

co_await h1;   // suspend until all writes complete
co_await h2;   // likely already done — just checks the flag
co_await h3;
```

### Pipelined reads (pre-queued receive buffers)

Pre-queue multiple read buffers. The driver fills them as data arrives without needing
to wake the consumer between each one.

```cpp
auto h1 = pipe.read(std::move(buf1));
auto h2 = pipe.read(std::move(buf2));
// Both are in the read queue. Driver fills them in order as data arrives.

auto [n1, b1] = co_await h1;
auto [n2, b2] = co_await h2;
```

### Managed with JoinSet

For continuous streaming, managing individual handles gets tedious. Pass them to a
`JoinSet` instead and let it track completion and surface errors:

```cpp
// Write side — fire and forget into a JoinSet<void>
coro::JoinSet<void> writes;
writes.add(pipe.write(std::move(buf1)));
writes.add(pipe.write(std::move(buf2)));
writes.add(pipe.write(std::move(buf3)));
co_await writes.drain();   // wait for all; rethrows first error if any

// Read side — collect results via next()
coro::JoinSet<std::pair<std::size_t, Buf>> reads;
reads.add(pipe.read(std::move(buf1)));
reads.add(pipe.read(std::move(buf2)));
while (auto result = co_await coro::next(reads)) {
    auto [n, buf] = std::move(*result);
    process(buf.data(), n);
}
```

### Producer loop (gendata style)

```cpp
for (size_t i = 0; ; ++i) {
    fill_packet(buffer, i);
    // Queue immediately — no suspension unless driver falls behind.
    auto wh = pipe.write(std::move(buffer));
    co_await coro::sleep_for(50ms);
    // Await write after sleep — driver likely already done.
    buffer = co_await std::move(wh);   // get buffer back after write completes
    // or: co_await std::move(wh); buffer = allocate_buffer();
}
```

Actually `WriteHandle` returns `void` — the buffer is consumed. Allocate a fresh one or
reuse from a pool. See [Return values](#return-values).

### Flush when delivery must be confirmed

```cpp
co_await pipe.write(std::move(command_buf));
co_await pipe.flush();   // suspends until OS has the data
// Now safe to expect a response.
```

---

## Return values

`WriteHandle` completes with `void`. The buffer is moved into the request and consumed by
libuv — there is nothing to return. To reuse a buffer, allocate a new one after `co_await`.

`ReadHandle<Buf>` completes with `std::pair<std::size_t, Buf>` — the byte count and the
buffer back. The buffer was moved in, filled by libuv, and returned to the caller. This
is zero-copy end-to-end.

---

## Architecture

```
┌──────────────────────────────────────────────────────────┐
│ PipeState (shared between uv thread and callers)         │
│                                                          │
│  read_queue:  std::queue<shared_ptr<ReadRequest>>        │
│  write_queue: std::queue<shared_ptr<WriteRequest>>       │
│  read_driver_waker   write_driver_waker                  │
│  mutex                                                   │
└──────────────────────────────────────────────────────────┘
       ▲ enqueue ReadRequest          enqueue WriteRequest ▲
       │                                                   │
  pipe.read(buf)                               pipe.write(buf)
  returns ReadHandle                           returns WriteHandle
  (no suspension)                              (no suspension)
       │                                                   │
       ▼ fill buf via libuv           drain buf via libuv  ▼
  [uv thread — read_driver]        [uv thread — write_driver]
  alloc_cb / read_cb                uv_write(all queued bufs)
  signals ReadRequest waker         signals WriteRequest wakers
```

---

## Request types

Both request types own the buffer via `std::any`, so the buffer stays alive even if the
caller cancels before the driver completes. The `char*` is extracted after moving into
`std::any` by casting back to a reference. Since requests are heap-allocated via
`make_shared`, the `std::any` is at a stable address and the extracted pointer is valid
regardless of SBO.

```cpp
struct WriteRequest {
    std::any                       buf_owner;
    const char*                    base;
    std::size_t                    size;
    bool                           completed = false;  // written by driver
    std::error_code                error;
    std::mutex                     mutex;
    std::shared_ptr<detail::Waker> waker;              // GUARDED BY mutex
};

struct ReadRequest {
    std::any                       buf_owner;
    char*                          base;
    std::size_t                    capacity;
    std::size_t                    filled    = 0;
    bool                           completed = false;  // written by driver
    std::error_code                error;
    std::mutex                     mutex;
    std::shared_ptr<detail::Waker> waker;              // GUARDED BY mutex
};

template <ByteBuffer Buf>
std::shared_ptr<WriteRequest> make_write_request(Buf buf) {
    auto req       = std::make_shared<WriteRequest>();
    req->buf_owner = std::move(buf);
    auto& stored   = std::any_cast<Buf&>(req->buf_owner);
    req->base      = reinterpret_cast<const char*>(std::ranges::data(stored));
    req->size      = std::ranges::size(stored);
    return req;
}
// make_read_request analogous
```

---

## Shared State

```cpp
struct PipeState {
    uv_pipe_t          handle;
    std::mutex         mutex;

    std::queue<std::shared_ptr<ReadRequest>>  read_queue;   // GUARDED BY mutex
    std::shared_ptr<detail::Waker>            read_driver_waker;  // GUARDED BY mutex

    std::queue<std::shared_ptr<WriteRequest>> write_queue;  // GUARDED BY mutex
    std::shared_ptr<detail::Waker>            write_driver_waker; // GUARDED BY mutex

    bool                           closing = false;
    std::shared_ptr<detail::Waker> close_waker;             // GUARDED BY mutex
};
```

---

## Write Driver

Wakes when the write queue is non-empty. Drains as many queued requests as possible into
a single `uv_write` call using a `uv_buf_t[]` array — one libuv operation services the
entire batch. Signals each request's waker after the callback fires.

```
write_driver loop:
    if write_queue empty: park on write_driver_waker

    snapshot all queued WriteRequests into local vector (drain the queue under mutex)
    build uv_buf_t[] from {req.base, req.size} for each
    issue uv_write(buf_array, n_bufs)
    co_await write_cb
    for each request in snapshot: set completed=true, wake waker
```

Batching multiple requests into one `uv_write` is the key scheduling amortisation: all
pipelined writes queued before the driver wakes are delivered in a single syscall.

---

## Read Driver

Arms `uv_read_start` once and services queued `ReadRequest`s as data arrives. Stops when
the queue drains and re-arms when new requests are enqueued.

```
alloc_cb:
    peek front ReadRequest → return {base, capacity}
    if queue empty: return zero-length buf (triggers UV_ENOBUFS in read_cb)

read_cb(nread):
    if nread == UV_ENOBUFS: uv_read_stop; park on read_driver_waker; return
    if nread == UV_EOF or nread < 0: signal error to front request, stop
    pop front ReadRequest; set filled=nread, completed=true; wake waker
    if queue now empty: uv_read_stop; park on read_driver_waker
```

---

## Sequence diagram

```mermaid
sequenceDiagram
    participant C as Caller (worker thread)
    participant S as PipeState queues
    participant D as driver (uv thread)
    participant L as libuv

    Note over C,L: Pipe::open — one cross-thread hop
    C->>D: with_context → spawn read_driver + write_driver
    D->>L: uv_pipe_init, uv_pipe_open

    Note over C,L: Pipelined writes — driver batches into one uv_write
    C->>S: push WriteRequest{buf1} → wake write_driver_waker
    C->>S: push WriteRequest{buf2}
    C->>S: push WriteRequest{buf3}
    Note over C: caller continues without suspending
    D->>S: drain write_queue → [req1, req2, req3]
    D->>L: uv_write([buf1, buf2, buf3])
    L->>D: write_cb → signal req1, req2, req3 wakers
    C-->>C: co_await h1/h2/h3 resumes (all already done)

    Note over C,L: Zero-copy read
    C->>S: push ReadRequest{empty_buf}
    D->>L: uv_read_start; alloc_cb → empty_buf.base/capacity
    L->>D: read_cb(nread) → set filled, signal ReadRequest waker
    C-->>C: co_await rh → resume with {nread, filled_buf}
```

---

## What changes vs current implementation

| | Current | After |
|---|---|---|
| `write` / `read` return type | `JoinHandle<pair<size_t,Buf>>` | `WriteHandle` / `ReadHandle<Buf>` |
| Caller suspends at | Call site (always) | `co_await handle` (caller chooses when) |
| Multiple in-flight ops | Not possible | Yes — queue N, await later |
| Write batching | No | Yes — driver coalesces into one `uv_write` |
| Zero-copy | Yes (direct to libuv) | Yes (direct to libuv) |
| Buffer ownership during I/O | Caller frame (must stay alive) | Moved into request (`std::any`) |
| Cancellation safety | Caller must not cancel | Safe — request owns buffer |
| Cross-thread cost | Per operation | Per driver wake (amortised over batch) |

---

## Implementation steps

1. Define `WriteRequest` and `ReadRequest` with `std::any` ownership and per-request mutex.
2. Define `WriteHandle` and `ReadHandle<Buf>` as awaitables holding `shared_ptr<Request>`.
3. Define `PipeState` with `shared_ptr` queues, wakers, mutex, `uv_pipe_t`, closing flag.
4. Update `Pipe` to hold `shared_ptr<PipeState>`; remove `uv_file m_fd` and stray `std::queue<Event>`.
5. Update `Pipe::open`: after `uv_fs_open`, use `with_context` once to `uv_pipe_init` + `uv_pipe_open`, spawn both drivers.
6. Implement `write_driver`: drain queue, batch into `uv_write`, signal wakers on callback.
7. Implement `read_driver`: `uv_read_start` with alloc/read callbacks, stop when queue empty.
8. Implement `Pipe::write` and `Pipe::read` as regular functions returning handles.
9. Add `Pipe::flush()` returning `Coro<void>` that suspends until write queue is empty and driver has signalled all completions.
10. Update `Pipe::close` / destructor: signal drivers, drain queues with errors, `uv_close` teardown.
