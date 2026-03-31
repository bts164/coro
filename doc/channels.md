# Channels

Async channels for inter-task communication. Three variants are planned; `broadcast` is
deferred.

| Variant | Producers | Consumers | Buffering |
|---|---|---|---|
| `oneshot` | 1 | 1 | none — single value |
| `mpsc` | N (cloneable sender) | 1 | bounded ring buffer |
| `watch` | 1 | N (cloneable receiver) | 1 — last value only |

All three live under `include/coro/sync/`. The interface and internal design should follow
Tokio's implementation as closely as C++ allows.

The library targets **C++23** for now. `std::expected<T, E>` is used for fallible
operations instead of exceptions or `std::optional`. A future compatibility shim
(`tl::expected` or a hand-rolled equivalent) will make this C++20-compatible — see the
roadmap.

---

## Cross-Cutting Design

These requirements apply to all three channel variants.

### Error handling

All fallible operations return `std::expected<T, ChannelError>` rather than throwing.
Channel closure is normal control flow in many programs, not an exceptional condition, so
forcing callers into try/catch is the wrong default. Callers that prefer exceptions can
call `.value()`, which throws on error:

```cpp
// Explicit — check the result:
auto r = co_await rx.recv();
if (!r) { /* channel closed */ }
T v = *r;

// Exception-style — let it throw:
T v = (co_await rx.recv()).value();
```

This is the library-wide error handling policy: `std::expected` as the default, `.value()`
as the escape hatch. `JoinHandle` and other futures that can fail will be updated to follow
the same pattern.

### RAII handles and channel closure

Sender and Receiver are RAII handles. The number of live handles on each end is reference
counted. When all senders are dropped the channel is closed from the sender side; when the
receiver is dropped it is closed from the receiver side. The appropriate error is delivered
to any task suspended on the other end.

```cpp
auto [tx, rx] = coro::oneshot::channel<T>();

// Receiver is waiting...
T x = co_await rx;  // suspended here

// ...but the sender is dropped without sending
{
    auto local_tx = std::move(tx);
    // local_tx goes out of scope without calling send()
}
// rx receives an error — channel closed with no value sent
```

### Thread safety

Channel handles follow the standard C++ object model: **concurrent operations on the
same handle instance are not safe** without external synchronization. This is the same
contract as `std::vector` or any other standard container — the object itself is not
protected against data races on its own members.

The shared channel state (ring buffer, waiter lists, ref counts) is **always
mutex-protected** and is safe for concurrent access from any number of threads. What
is not safe is two threads calling methods on the *same* `Sender` or `Receiver` object
simultaneously.

In practice:
- Each `Sender` clone is an independent object. Multiple threads sending concurrently
  is safe — give each thread its own clone.
- A `Receiver` is single-owner by design. Move it to whichever thread needs it.
- The synchronous `send()` on `OneshotSender` and `WatchSender` may be called from any
  thread, including non-async contexts, as long as only one thread calls it at a time.

```cpp
// Safe — each thread owns its own Sender clone:
auto tx2 = tx.clone();
std::thread([tx2 = std::move(tx2)]() mutable { tx2.trySend(1); }).detach();
tx.trySend(2);  // tx and tx2 are separate objects

// Not safe — two threads sharing the same Sender instance:
std::thread([&tx]() { tx.trySend(1); }).detach();
tx.trySend(2);  // data race on tx
```

Unlike Rust, C++ cannot enforce these rules at compile time. Violations produce
undefined behaviour rather than a compile error. This contract must be documented on
every public handle type.

### Synchronous try variants

Every async operation has a synchronous non-blocking counterpart. The async version
suspends when it cannot make progress; the try version returns immediately.

```cpp
// Async (suspends if buffer full / no value yet):
co_await tx.send(value);
T v = co_await rx.recv();

// Synchronous (returns immediately):
std::expected<void, TrySendError<T>> err = tx.trySend(std::move(value));
std::expected<T, ChannelError> v = rx.tryRecv();  // ChannelError::Empty if nothing ready
```

### Return-value-on-failure pattern

Following Tokio, try operations that fail return the caller's original value so it is not
silently dropped. `trySend` returns `std::expected<void, TrySendError<T>>` — truthy on
success; on failure `r.error()` carries both the reason (`Full` or `Disconnected`) and
the unsent value:

```cpp
if (auto r = tx.trySend(std::move(v)); !r)
    use(std::move(r.error().value));  // send failed — value returned to caller
```

`std::optional<T>` was considered but rejected: `has_value() == true` conventionally
signals success, yet in the return-value-on-failure pattern it would signal the error
case — an inversion that causes bugs. `std::expected` has the correct bool semantics and
maps directly to Tokio's `Result<(), TrySendError<T>>`.

This is especially important for move-only types: the caller must be able to recover the
value if the send cannot proceed.

### Sending errors through a channel

Channels transport values; they have no first-class mechanism for sending errors. Adding
one would create two distinct error paths at the receiver (`std::unexpected<ChannelError>`
from infrastructure failures vs. a rethrown exception from a sent error), with no clean
way to distinguish them at the call site.

The idiomatic solution — the same pattern Tokio uses — is to make the value type itself
a result type:

```cpp
auto [tx, rx] = coro::mpsc::channel<std::expected<int, MyError>>(16);
```

The outer `std::expected` (from `recv()`) represents the transport layer: did the channel
deliver anything? The inner `std::expected` represents the application layer: did the
operation the sender was performing succeed?

```cpp
// Sender — normal value:
co_await tx.send(42);

// Sender — propagating an error:
co_await tx.send(std::unexpected(MyError{"something went wrong"}));

// Sender — forwarding a caught exception:
try {
    co_await tx.send(compute());
} catch (...) {
    co_await tx.send(std::unexpected(MyError::from_exception(std::current_exception())));
}

// Receiver — two explicit layers, each with a clear meaning:
auto r = co_await rx.recv();
if (!r) {
    // Transport error — channel was closed (ChannelError)
    handle_channel_closed(r.error());
    return;
}
if (!*r) {
    // Application error — sender sent an error value (MyError)
    handle_app_error(r->error());
    return;
}
int v = **r;
```

When only application errors are possible (the channel will not be closed unexpectedly),
the outer layer can be collapsed with `.value()`:

```cpp
std::expected<int, MyError> result = (co_await rx.recv()).value();
```

**Documentation requirement:** this pattern is non-obvious to users coming from other
async frameworks. Both the Doxygen comments on each channel type and the user-facing
documentation must include a worked example of the `channel<std::expected<T, E>>`
pattern, explaining why first-class error sending was not added and how to achieve
equivalent behaviour.

---

## `oneshot`

Single value, one sender, one receiver. Send is synchronous (never suspends); receive is
async.

```cpp
auto [tx, rx] = coro::oneshot::channel<std::unique_ptr<int>>();

// Sender — synchronous; can be called from any thread
if (auto r = tx.send(std::make_unique<int>(42)); !r)
    // Receiver was already dropped — r.error() contains the unsent value

// Receiver — async
std::unique_ptr<int> v = co_await rx;  // suspends until send() or sender drop
```

- `OneshotSender<T>` — `send(T)` synchronous; returns `std::expected<void, T>` — error
  holds the unsent value if the receiver has already been dropped. Dropping the sender
  without calling `send()` closes the channel and causes `rx` to complete with an error.
- `OneshotReceiver<T>` — satisfies `Future<T>`.

### Internal state

```
OneshotShared<T>:
    mutex
    std::optional<T>  slot          // empty until send() is called
    bool              sender_alive
    bool              receiver_alive
    Waker             receiver_waker  // set when receiver suspends before send()
```

The receiver waker is stored directly in the shared state (not in an intrusive node)
because there is at most one receiver and it never needs O(1) mid-list removal —
cancellation either happens before the waker is set or after the value has arrived.

If the `OneshotReceiver` future is dropped while waiting (task cancelled), its destructor
must clear `receiver_waker` in the shared state. Without this, a subsequent `send()` call
would wake a dead task, wasting a scheduler cycle. Clearing the waker is the only cleanup
needed — no node to unlink.

---

## `mpsc`

Bounded, backpressured queue. Multiple producers, one consumer. Sender is cloneable.

```cpp
auto [tx, rx] = coro::mpsc::channel<int>(/*capacity=*/16);

// Producer (cloneable):
auto tx2 = tx.clone();
co_await tx.send(42);                   // suspends when buffer is full
auto unsent = tx.trySend(43);           // returns immediately

// Consumer:
while (auto v = co_await coro::next(rx))
    use(*v);
```

- `Sender<T>` — `send(T)` returns `Future<void>`, suspends when the buffer is full;
  `trySend(T)` returns immediately. Cloneable. Dropping all senders closes the channel.
- `Receiver<T>` — satisfies `Stream<T>`; yields items in send order; signals exhaustion
  when all senders are dropped. Dropping the receiver is a hard error for any suspended
  or future sender — `send()` returns `std::expected<void, T>` with the unsent value in
  the error slot.

### Internal state

```
MpscShared<T>:
    mutex
    T[]               ring_buffer     // fixed-capacity, allocated once at construction
    size_t            head, tail, count
    size_t            sender_count
    bool              receiver_alive
    IntrusiveList     sender_waiters  // SenderNode list — suspended senders
    std::optional     receiver_waiter // ReceiverNode — at most one suspended receiver
```

Intrusive nodes live in the coroutine frames of suspended futures — the channel allocates
no additional memory for waiters:

```
SenderNode:
    IntrusiveListNode  link         // prev/next pointers
    Waker              waker
    T                  value        // held in sender's coroutine frame until delivered
```

```
ReceiverNode:
    Waker              waker
    T*                 destination  // points into receiver's coroutine frame
```

`ReceiverNode` is not part of a list — there is at most one suspended receiver at any
time (single consumer), so `std::optional<ReceiverNode>` in the shared state is
sufficient. No intrusive list, no dynamic allocation.

### Send algorithm

1. **Lock.**
2. Receiver dropped → unlock, return `std::unexpected(std::move(value))`.
3. Receiver is waiting (`receiver_waiter` is set) → move value to
   `receiver_waiter->destination`, wake it, clear `receiver_waiter`, unlock. Sender
   never suspends; value goes directly to the receiver's coroutine frame.
4. Ring buffer has space → move value into ring buffer, unlock.
5. Buffer full, no waiting receiver → unlock, suspend: populate `SenderNode` with waker
   and value (both in the sender's coroutine frame), insert into `sender_waiters`, return
   `Pending`.

### Recv algorithm

1. **Lock.**
2. Ring buffer non-empty → pop value; if `sender_waiters` non-empty, move head sender's
   value into the now-free buffer slot and wake it; unlock, return value.
3. Ring buffer empty, `sender_waiters` non-empty → move head sender's value directly to
   the local result, wake sender, unlock, return value. (No buffer copy.)
4. Ring buffer empty, no suspended senders, all senders dropped → unlock, return
   `std::unexpected(ChannelError::SenderDropped)`.
5. Otherwise → unlock, suspend: set `receiver_waiter` with waker and `&destination`,
   return `Pending`.

### Zero-copy paths

Values skip the ring buffer in two situations:

- **Sender finds a waiting receiver (step 3 of send):** moved directly from the sender's
  argument to the receiver's coroutine frame.
- **Receiver finds suspended senders with an empty buffer (step 3 of recv):** moved
  directly from the sender's coroutine frame to the receiver.

The ring buffer's role is absorbing **bursts** — when producers run ahead of the consumer
for a stretch. In steady state with balanced rates, the buffer may never be touched.

### Cancellation safety

A `SendFuture` or `RecvFuture` that is dropped while suspended (task cancelled, losing
branch of a `select`, timeout) must remove its intrusive node from the channel's waiter
list before the coroutine frame is freed — the node lives inside that frame, so a
dangling pointer in the list would be UB.

The future's destructor is responsible for this: if the node is linked (i.e. the future
was suspended and not yet woken), the destructor re-acquires the channel mutex and
unlinks the node. This is always safe because the destructor holds a `shared_ptr` to the
channel state, keeping it alive for the duration of the unlink.

This requirement applies to `SenderNode` (mpsc) and `ReceiverNode` (mpsc and watch) and
must be implemented in their respective future destructors.

---

## `watch`

Single-value channel; new writes overwrite the previous value. Multiple receivers each
track their own "last seen" version. No backpressure — send never suspends.

```cpp
auto [tx, rx] = coro::watch::channel<int>(/*initial=*/0);

// Sender:
if (auto r = tx.send(42); !r)
    // all receivers dropped — r.error() contains the unsent value

// Receiver:
co_await rx.changed();        // suspends until a new value has been sent
int current = rx.borrow();    // read current value (under a read lock)
auto rx2 = rx.clone();        // independent cursor for another task
```

- `WatchSender<T>` — `send(T)` synchronous; returns `std::expected<void, T>` — error
  holds the unsent value if all receivers have been dropped. Last write wins among
  successful sends. Dropping the sender closes the channel; receivers see an error on
  the next `changed()` call.
- `WatchReceiver<T>` — `changed()` returns `Future<std::expected<void, ChannelError>>`;
  `borrow()` returns a `BorrowGuard<T>` that holds a shared read lock for its lifetime;
  `clone()` creates an independent receiver starting at version 0. Cloneable.

`BorrowGuard<T>` is a lightweight RAII handle — it holds the `value_mutex` shared lock
and exposes `operator*` and `operator->` for read-only access. The lock is released when
the guard is destroyed:

```cpp
{
    auto guard = rx.borrow();
    process(*guard);       // shared lock held here
}                          // lock released
```

`BorrowGuard` is not copyable (copying would require duplicating the lock) but is
movable. Callers must not hold a `BorrowGuard` across a `co_await` point — doing so
would hold the read lock while suspended, blocking all `send()` calls indefinitely.

### Internal state

```
WatchShared<T>:
    std::shared_mutex value_mutex   // shared for borrow(), exclusive for send()
    T                 value
    uint64_t          version       // incremented on every send()
    bool              sender_alive

    mutex             waker_mutex   // separate lock — guards waker list only
    IntrusiveList     receiver_waiters  // ReceiverNode list
```

```
ReceiverNode:
    IntrusiveListNode  link         // prev/next pointers
    Waker              waker
    uint64_t           last_seen    // version the receiver had when it suspended
```

`version` and the waker list use separate locks so `borrow()` (read-only, potentially
long-held) does not block `changed()` from registering its waker.

Each `WatchReceiver` stores its own `last_seen` version, initialised to `0`. Because
`version` starts at `0` and is incremented to `1` on the first `send()`, a freshly
created or cloned receiver will have `last_seen == 0`. If no `send()` has occurred yet,
`changed()` suspends; if at least one `send()` has occurred, `changed()` returns
immediately on the first call (the receiver sees it as a new value). Callers that want
only the current value without waiting should call `borrow()` directly.

On wake, the receiver updates `last_seen` to the version it observed.

Because multiple receivers may be suspended simultaneously, `receiver_waiters` is a full
intrusive list. On `send()`, all waiters are woken and removed from the list — each
receiver independently re-reads the value via `borrow()`.

---

## Error codes

```cpp
enum class ChannelError {
    Closed,           // generic — used by oneshot when sender drops without sending
    SenderDropped,    // all senders were dropped (mpsc, watch)
    ReceiverDropped,  // receiver was dropped (mpsc sender gets its value back)
    Empty,            // tryRecv only — channel open but no value available yet
};
```

No message string. Richer error context belongs in the value type — see
"Sending errors through a channel".

### `TrySendError<T>`

`trySend` can fail for two distinct reasons that callers need to distinguish:

- **`Full`** — buffer is full; the caller may retry later.
- **`Disconnected`** — receiver was dropped; retrying will never succeed.

Both cases return the unsent value to the caller. A plain `std::expected<void, T>`
cannot carry the reason alongside the value, so `trySend` returns
`std::expected<void, TrySendError<T>>`:

```cpp
template<typename T>
struct TrySendError {
    enum class Kind { Full, Disconnected } kind;
    T value;  // the unsent value, always present on failure
};
```

```cpp
if (auto r = tx.trySend(std::move(v)); !r) {
    if (r.error().kind == TrySendError<T>::Kind::Disconnected)
        return;  // receiver gone — give up
    // else: Full — buffer full, retry or drop
    buffer_for_retry(std::move(r.error().value));
}
```
