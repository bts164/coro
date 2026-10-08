# Timers

The user-facing time API: `sleep_for`, `sleep_until`, `timeout`, `timeout_at` and
`IntervalTimer`. All of them are built on one leaf future, `SleepFuture`, which registers
a deadline with the runtime's timer queue. The queue itself, the `Clock`, and who fires
timers are part of the driver's design: see [I/O Driver](io_driver.md), "Timers".

---

## Goals

- **One implementation for every platform.** No `#ifdef`: on desktop the timer goes to
  the `IoDriver`, whose earliest deadline bounds `epoll_pwait2`; on Pico it goes to the
  `CurrentThreadExecutor`'s own queue.
- **Nanosecond resolution, no thread hop.** A sleep is a heap entry on the thread that
  already blocks in the driver. It is not rounded to milliseconds and costs no
  cross-thread round trip.
- **Never early.** The deadline is the truth. `SleepFuture` checks the clock itself, so an
  early or spurious wake just leaves it pending.
- **Safe to drop at any time.** `SleepFuture` is a leaf future with no `cancel()`.
  Dropping it mid-wait cancels its timer, whose heap entry is later removed without a
  wake.
- **No allocation per sleep.** A timer is an entry in vectors the queue already owns. The
  future holds only an integer id for it.

---

## API

```cpp
namespace coro {

using Instant = Clock::time_point;   // see clock.h

class SleepFuture {
public:
    using OutputType = void;
    explicit SleepFuture(Instant deadline);
    ~SleepFuture();                         // cancels the timer, if registered
    PollResult<void> poll(detail::Context& cx);
    Instant deadline() const noexcept;
};

[[nodiscard]] SleepFuture sleep_until(Instant deadline);
[[nodiscard]] SleepFuture sleep_for(std::chrono::nanoseconds duration);  // sleep_until(Clock::now() + d)

template<Future F> [[nodiscard]] auto timeout(std::chrono::nanoseconds duration, F future);
template<Future F> [[nodiscard]] auto timeout_at(Instant deadline, F future);

class IntervalTimer {
public:
    class TickFuture;                       // Future<void>
    explicit IntervalTimer(std::chrono::nanoseconds period);
    [[nodiscard]] TickFuture tick();
};

}
```

```cpp
co_await coro::sleep_for(200us);

auto r = co_await coro::timeout(5s, fetch_data());
if (r.index() == 1) handle_timeout();

coro::IntervalTimer frame(20ms);
while (running) {
    do_work();
    co_await frame.tick();   // waits out the rest of the 20 ms window
}
```

`poll()` and the cancel are out of line, in `src/sync/sleep.cpp`, so `sleep.h` does not
pull in `runtime.h`.

---

## `SleepFuture`

```mermaid
sequenceDiagram
    participant S as SleepFuture
    participant R as Runtime
    participant Q as TimerQueue
    S->>S: poll(): now < deadline
    S->>R: add_timer(deadline, weak waker)
    R->>Q: insert (driver on desktop, executor on Pico)
    Q-->>S: TimerId
    Note over Q: deadline passes; the thread turning the driver fires it
    Q-->>S: wake (task re-polled)
    S->>S: poll(): now >= deadline → Ready
    Note over S,Q: dropped before the deadline instead: cancel_timer(id)
```

The future's whole state is the deadline, the `TimerId` and `Runtime*` of its
registration (none until the first pending poll), and a copy of the weak waker it
registered. Since the timer is named by an id and not by the future's address, moving
the future stays safe even after a poll.

`poll()`:

1. `Clock::now() >= deadline` → `PollReady`. The timer, if registered, is cancelled then
   or by the destructor.
2. First pending poll: `id = current_runtime().add_timer(deadline, waker)`, and remember
   the runtime and the waker.
3. Later pending polls: nothing, if the context's waker is the one registered. This is
   the usual case and takes no lock. If the waker is a different one, cancel the timer and
   register again with the new waker.

The destructor calls `cancel_timer(id)` on the remembered runtime if a timer is
registered. It does so whether or not the timer has fired: an id whose timer has fired is
stale, and the queue ignores it.

!!! note "NOTE: why a re-poll can bring a different waker"
    A context's waker is the task being polled (or `blocking_wait`'s own waker), and
    combinators such as `select` pass the context through unchanged. A future is also
    never moved once polled. So a `SleepFuture` normally sees one waker for its whole
    life.

    The exception is a future that lives in a coroutine frame whose owner changes. A
    `CoroStream` suspended in `co_await sleep_for(...)` can be polled by one task with
    `next()`, then handed to another task, or to `blocking_next()`, while still suspended
    there. The frame doesn't move, but the next poll arrives with the new task's waker.
    Without step 3 the timer would wake the old task, and the new one would sleep
    forever.

The queue holds a `Weak` waker: a strong one would keep a finished task alive until its
deadline. It also means cancelling is about avoiding a spurious wake, not about memory
safety.

The runtime must outlive the future, as it already must for any future that holds an
`IoRegistration`. `Runtime` destroys its executor, and with it every task and its
futures, before the driver that owns the queue.

`Runtime::add_timer()` sends the timer to the driver on desktop and to the executor on
Pico. On a desktop `Runtime` whose executor never turns the driver (a
`CurrentThreadExecutor` given its own `Parker`), it throws `std::logic_error`, because
the timer could never fire. That exception propagates from the first pending `poll()`.

## `timeout` and `timeout_at`

`timeout(d, f)` is `select(f, sleep_for(d))`, and `timeout_at(t, f)` is
`select(f, sleep_until(t))`. The result is `SelectBranch<0, T>` if `f` finished first and
`SelectBranch<1, void>` if the deadline did. When `f` wins, the `SleepFuture` is dropped
and its timer cancelled. A loop that races a short operation against a long `timeout()`
therefore cancels one timer per iteration, long before any of their deadlines. The queue
sweeps cancelled entries out once they outnumber the live ones, so they don't pile up:
see [I/O Driver](io_driver.md), "`detail::TimerQueue`".

## `IntervalTimer`

`IntervalTimer` keeps one `Instant m_next`, set to one period after construction. `tick()`
returns a `TickFuture`: a `SleepFuture` for `m_next` and a pointer back to the timer. When
the sleep is ready, the same `poll()` advances `m_next` by one period, so time spent
working between ticks is absorbed instead of added. If the loop has fallen more than a
period behind, it resets `m_next` to `now + period` instead of firing a burst of immediate
ticks.

`TickFuture` is a hand-written future and not a coroutine, so a tick has no coroutine
frame and allocates nothing, like the sleep inside it. A tick dropped mid-wait leaves
`m_next` unchanged, and the next `tick()` waits for the same deadline.

!!! danger "WARNING: the timer must outlive its tick and stay put"
    `TickFuture` holds a raw pointer to its `IntervalTimer`. The timer must not be
    destroyed or moved while a tick is pending. A timer that is a local of the loop's
    coroutine, as in the example above, meets this without any care.

---

## Races

- **Re-poll on another worker while firing.** A re-poll with the same waker touches
  nothing shared, so there is nothing to race. The queue pops an entry only after its
  deadline, and the clock is monotonic, so a poll that comes after the fire sees the
  deadline passed and returns ready.
- **Re-poll with a different waker while firing.** `poll()` saw the deadline not yet
  passed, then cancels and registers again. If the queue fired the old entry in between,
  the cancel finds a stale id and does nothing, and the old waker's wake is spurious. The
  new entry is already past its deadline and fires on the next turn. A blocked driver
  holder doesn't delay that: either `insert()` reports the entry as the earliest and the
  holder is unparked, or an earlier entry is also due and the holder is waking for it.
- **Future dropped while its timer is firing.** `fire_expired()` takes the waker out of
  the queue under the queue mutex and wakes it after unlocking. A destructor that runs in
  between cancels a stale id, which does nothing. The wake then reaches a task whose
  future is gone, which is a spurious wake the task tolerates. The `Weak` waker stops it
  from touching a freed task.
- **Insert while the driver holder is blocked.** Handled by the queue's waiter record:
  see [I/O Driver](io_driver.md), "Timer races".

!!! note "NOTE: timer latency is I/O latency"
    Timers fire only when some thread turns the driver, so they have the same latency as
    I/O readiness. With all workers busy, an expired timer waits up to 61 task polls on
    whichever worker reaches its `try_turn(0)` first; with the driver holder running a
    long task and every other worker asleep, it waits until a worker parks. See
    [Work-Stealing Scheduler](work_stealing_executor.md), "Parking and its races".

---

## Tests

| Test | Checks |
|---|---|
| `SleepTest.SubMillisecondPrecision*` | 1000 × `sleep_for(200us)` takes under 1 s on `Runtime(1)` and `Runtime(4)`, which a millisecond timer can't do. |
| `SleepTest.SleepUntil`, `PassedDeadlineIsReadyWithoutATimer` | `sleep_until` doesn't return early; a passed deadline is ready on the first poll. |
| `SleepTest.DroppedSleepDoesNotWake` | A sleep polled once and dropped never wakes its counting waker. |
| `SleepTest.RepollUpdatesWaker` | A sleep polled with waker A, then B, wakes only B. |
| `SleepTest.RepollWithSameWakerKeepsTimer` | A sleep polled repeatedly with one waker wakes it once. |
| `SleepTest.MovedAfterPollKeepsTimer` | A sleep moved after its first poll still wakes; dropping the moved-from one cancels nothing. |
| `SleepTest.ManyCancelledTimeouts` | 10,000 timeouts that never expire, each cancelling a far-off timer, then a sleep that still fires. |
| `SleepTest.StreamHandedToAnotherTaskStillWakes` | A `CoroStream` suspended in a sleep, polled by one task and then awaited by another, wakes the second. |
| `SleepTest.ManyConcurrentSleepers` | 10,000 spawned sleeps of 1–50 ms on `Runtime(4)` all complete. |
| `SleepTest.WorksWithWorkSharingRuntime` | The WorkSharing driver handoff fires timers. |
| `SleepTest.ThrowsWithoutDriver` | On a `CurrentThreadExecutor` with a `PollingParker`, awaiting `sleep_for()` throws `std::logic_error`. |
| `TimeoutTest.*` | The future or the deadline wins as expected; `timeout_at()` returns the timeout branch at its deadline. |
| `IntervalTimerTest.*` | Ticks come once per period; work between ticks is absorbed; a tick dropped mid-wait leaves the schedule alone; missed ticks are skipped, not delivered in a burst. |
| `SleepPicoTest.*` (`test_pico_suite`) | Sleep, ordering, `timeout` and `IntervalTimer` on the Pico `Runtime` over a stubbed `time_us_64()`. |

The queue itself is covered by `test/detail/test_timer_queue.cpp` and the driver's timer
tests in `test/runtime/test_io_driver.cpp`.
