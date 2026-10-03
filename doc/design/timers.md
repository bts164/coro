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
  Dropping it mid-wait leaves an empty slot in the heap, which is later popped without a
  wake.

---

## API

```cpp
namespace coro {

using Instant = Clock::time_point;   // see clock.h

class SleepFuture {
public:
    using OutputType = void;
    explicit SleepFuture(Instant deadline);
    ~SleepFuture();                         // empties the slot's waker, if registered
    PollResult<void> poll(detail::Context& cx);
    Instant deadline() const noexcept;
};

[[nodiscard]] SleepFuture sleep_until(Instant deadline);
[[nodiscard]] SleepFuture sleep_for(std::chrono::nanoseconds duration);  // sleep_until(Clock::now() + d)

template<Future F> [[nodiscard]] auto timeout(std::chrono::nanoseconds duration, F future);
template<Future F> [[nodiscard]] auto timeout_at(Instant deadline, F future);

class IntervalTimer {
public:
    explicit IntervalTimer(std::chrono::nanoseconds period);
    [[nodiscard]] Coro<void> tick();
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

`poll()` and the release are out of line, in `src/sync/sleep.cpp`, so `sleep.h` does not
pull in `runtime.h`.

---

## `SleepFuture`

```mermaid
sequenceDiagram
    participant S as SleepFuture
    participant R as Runtime
    participant Q as TimerQueue
    S->>S: poll(): now < deadline
    S->>S: create TimerSlot, store weak waker
    S->>R: add_timer(deadline, slot)
    R->>Q: insert (driver on desktop, executor on Pico)
    Note over Q: deadline passes; the thread turning the driver fires it
    Q-->>S: wake (task re-polled)
    S->>S: poll(): now >= deadline → Ready
```

`poll()`:

1. `Clock::now() >= deadline` → release the slot, `PollReady`.
2. First pending poll: create the `TimerSlot`, store the waker,
   `current_runtime().add_timer(deadline, slot)`. The slot is not shared until
   `add_timer()` publishes it, so no lock is needed yet.
3. Later pending polls: replace the slot's waker under its mutex, then check the clock
   again (see the re-poll race below). Under `select` the waker can change between polls,
   so the latest context is the one woken.

The slot holds a `Weak` waker. The task owns the future, which owns the slot, so a strong
waker in the slot would be a reference cycle through the queue.

`Runtime::add_timer()` sends the timer to the driver on desktop and to the executor on
Pico. On a desktop `Runtime` whose executor never turns the driver (a
`CurrentThreadExecutor` given its own `Parker`), it throws `std::logic_error`, because
the timer could never fire. That exception propagates from the first pending `poll()`.

## `timeout` and `timeout_at`

`timeout(d, f)` is `select(f, sleep_for(d))`, and `timeout_at(t, f)` is
`select(f, sleep_until(t))`. The result is `SelectBranch<0, T>` if `f` finished first and
`SelectBranch<1, void>` if the deadline did. When `f` wins, the `SleepFuture` is dropped
and its heap entry is popped without a wake at its deadline.

!!! tip "PERF: cancelled timers stay in the heap until their deadline"
    A loop that races a short operation against a long `timeout()` leaves one dead entry
    per iteration until each deadline passes. See the matching note in
    [I/O Driver](io_driver.md), "`detail::TimerQueue`", for the fix if profiling ever
    shows the heap growing.

## `IntervalTimer`

`IntervalTimer` keeps one `Instant m_next`, set to one period after construction. `tick()`
awaits `sleep_until(m_next)` and then advances it by one period, so time spent working
between ticks is absorbed instead of added. If the loop has fallen more than a period
behind, it resets `m_next` to `now + period` instead of firing a burst of immediate ticks.

---

## Races

- **Re-poll on another worker while firing.** The waker swap and the fire both hold the
  slot mutex, so the fire uses either the old waker or the new one. If the queue took the
  old one between `poll()`'s first clock check and the swap, the new waker would never
  fire. `poll()` therefore checks the clock again after storing it: the queue pops an
  entry only after its deadline, so that check sees the deadline passed and returns
  ready. The old waker's wake is then spurious and harmless.
- **Future dropped while its timer is firing.** `fire_expired()` takes the waker out of
  the slot under the slot mutex and wakes it after unlocking. A destructor that runs in
  between finds the slot already empty. The wake then reaches a task whose future is
  gone, which is a spurious wake the task tolerates. The `Weak` waker stops it from
  touching a freed task.
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
| `SleepTest.ManyConcurrentSleepers` | 10,000 spawned sleeps of 1–50 ms on `Runtime(4)` all complete. |
| `SleepTest.WorksWithWorkSharingRuntime` | The WorkSharing driver handoff fires timers. |
| `SleepTest.ThrowsWithoutDriver` | On a `CurrentThreadExecutor` with a `PollingParker`, awaiting `sleep_for()` throws `std::logic_error`. |
| `TimeoutTest.*` | The future or the deadline wins as expected; `timeout_at()` returns the timeout branch at its deadline. |
| `SleepPicoTest.*` (`test_pico_suite`) | Sleep, ordering, `timeout` and `IntervalTimer` on the Pico `Runtime` over a stubbed `time_us_64()`. |

The queue itself is covered by `test/detail/test_timer_queue.cpp` and the driver's timer
tests in `test/runtime/test_io_driver.cpp`.
