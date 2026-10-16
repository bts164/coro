# Work-Stealing Executor

## Overview

The current `WorkSharingExecutor` gives each worker thread a local queue but has no
stealing path: when a worker's local queue is empty it blocks on a shared
condition variable waiting for the global injection queue. Under uneven load — one task
spawning many children, or tasks with varied run times — some workers sleep while
others are overloaded. Work-stealing fixes this by allowing idle workers to take tasks
from busy workers' local queues before going to sleep.

The design mirrors Tokio's multi-threaded scheduler at a high level, adapted to the
existing `Executor` interface and task state machine.

---

## Goals

- Replace (or upgrade) `WorkSharingExecutor` with a scheduler that steals work across
  threads before parking.
- Preserve the existing `Executor` interface (`schedule`, `enqueue`, `poll_ready_tasks`,
  `wait_for_completion`) so `Runtime` needs no changes.
- Preserve all invariants of the lock-free `SchedulingState` CAS machine.
- Keep the lock-free deque interface (`push` / `pop` / `steal`) so that implementing the
  Chase-Lev algorithm later is a drop-in change to `WorkStealingDeque`.

---

## Current State

`WorkSharingExecutor` already has most of the structure in place:

- Per-worker `WorkStealingDeque` with `push`, `pop`, `steal` — currently mutex-backed.
- A shared injection queue protected by `m_mutex` + `m_cv`.
- Thread-locals `t_owning_executor` and `t_worker_index` for local-path routing in
  `enqueue()`.

What is missing is the steal path in `worker_loop()`. Today, when a worker's local queue
is empty it goes directly to sleep on `m_cv`. It should first attempt to steal from
peer workers before sleeping.

---

## Proposed Design

### Worker Loop with Stealing

The core change is the task-search strategy in `worker_loop()`. Each worker follows this
priority order:

```
1. Pop from own local queue                   (no lock, LIFO)
2. Drain from injection queue                 (lock, FIFO — prevents starvation of remote wakes)
3. Enter "searching" state (if permitted)
   a. Steal from a peer's local queue         (one full sweep, round-robin)
   b. Re-check injection queue                (a task may have arrived during the sweep)
   c. Found a task: wake one parked worker    (see "Passing the wake on")
4. Exit searching, park                       (the last searcher looks at its peers again first)
```

Step 2 is checked before stealing so that tasks enqueued from outside the executor
(timer wakeups, channel wakeups from non-worker threads) are not starved behind
stealing. It does not protect them from a worker whose own queue never empties; the
periodic check below does.

### Periodic injection-queue check

A worker reaches Step 2 only when its local queue is empty. Tasks that keep each other
runnable, or that keep spending their budget and being re-queued, keep it non-empty. If
every worker is in that state, nothing takes from the injection queue: wakes from
threads outside the executor, tasks scheduled from outside it, and tasks spilled from a
full local queue all wait until some worker runs dry. The driver is not affected; busy
workers already turn it every `kEventInterval` polls.

The fix is Tokio's `global_queue_interval`. Each worker counts the passes of its loop,
and on every `kGlobalQueueInterval`-th one it looks at the injection queue before its
own:

```
tick = 0
loop:
    ++tick
    if tick % kGlobalQueueInterval == 0 and m_injection_len != 0:
        task = pop one from the injection queue         // under m_mutex
    if no task: Step 1 (own local queue)
    if no task: Step 2 (injection queue, in a batch)    // unchanged
    ...
```

- **`kGlobalQueueInterval` is 61,** the value Tokio's multi-thread scheduler starts
  from. It is a constant next to `kEventInterval`.
- **One task is taken,** not a batch. The worker has local work already; the other
  workers reach the same check on their own counts.
- **The emptiness test is the lock-free read of `m_injection_len`** that Step 2 uses. A
  stale zero costs nothing: the next interval, or Step 2 or Step 4, finds the task.
- **Parking is untouched.** The check only changes which queue a busy worker pops
  first. The lost-wakeup argument in "Parking and its races" depends on what a worker
  re-checks before it parks, and that is the same.

The [task budget](task_budget.md) adds two things to the same loop: the `Context` for
each poll is built with the runtime's budget, and after a poll that yielded the worker
does its non-blocking driver turn at once, without waiting for `kEventInterval`.

!!! tip "PERF: tune the interval from the mean poll time, later"
    Tokio's multi-thread scheduler does not keep 61. It tracks a moving average of how
    long a task poll takes and picks the count that makes the interval about 200 µs,
    between 2 and 127 polls. That costs two clock reads per interval, not per poll.
    Add it only if a workload shows remote wakes waiting too long behind slow polls,
    or the lock being taken too often behind fast ones.

Tests, in `test/runtime/test_work_stealing_executor.cpp`:

| Test | Proves |
|---|---|
| `InjectedTaskRunsWhileEveryWorkerIsBusy` | With several times more self-waking tasks than workers, so that no local queue empties, a task woken from a thread outside the executor still runs promptly. |
| `SpawnFromOutsideRunsWhileEveryWorkerIsBusy` | The same for a task scheduled by a blocking-pool thread. |
| `OverflowedTasksRunWhileEveryWorkerIsBusy` | Tasks spilled to the injection queue by a full local queue are polled although the local queues stay non-empty. |

### Victim Selection

Use a round-robin starting offset seeded by `worker_index` so that all workers do not
pile onto the same victim simultaneously:

```
for i in 0..N:
    victim = (worker_index + 1 + i) % N
    if victim == worker_index: skip
    if m_local_queues[victim].steal_half(my_queue) > 0:
        return my_queue.pop()   // run the first stolen task immediately
```

Each worker tries all peers once per search pass before concluding there is nothing to steal.

### Bounded Searching Workers

At most `num_workers / 2` workers are in the "searching" state at any time, tracked by
a shared atomic `m_searching`:

```
// Worker trying to enter searching:
int expected = m_searching.load();
while (expected < max_searching):
    if m_searching.compare_exchange_weak(expected, expected + 1):
        // entered searching — do the steal sweep
        ...
        last_searcher = m_searching.fetch_sub(1) == 1
        if found a task: notify_if_needed()   // see "Passing the wake on"
        break
// If CAS loop fails (already enough searchers), skip to park.
```

`max_searching = num_workers / 2` (minimum 1). This prevents thundering herd: if half
the pool is already searching, additional idle workers park immediately instead of all
piling onto the same victim queues.

The steal sweep itself acts as the latency buffer before parking — there is no separate
spin-yield loop.

### Which worker is woken

Every wake goes to the lowest-numbered idle worker (the lowest set bit of `m_idle_mask`),
never to a random or round-robin one. Work therefore concentrates on the low-numbered
workers, and the pool grows upward only as far as the load needs: a program with at
most four tasks runnable at once keeps roughly workers 0-3 busy, and the rest stay
parked for the whole run, unless one of them steals work during a sweep.

This is deliberate:

- **Warm workers stay warm.** The workers that keep getting woken still have the
  program's data in cache and are known to the OS scheduler as busy, instead of being
  pulled out of a long sleep.
- **Idle cores stay idle.** Parked workers are never woken just to take a turn, so their
  cores can drop into deep sleep states, and the parts of the program that are actually
  busy get more thermal and turbo headroom.

Spreading wakes evenly would cost both and gain no throughput. Tokio gets the same effect
differently, by keeping sleeping workers in a stack and waking the most recently parked
one.

What a profiler shows follows from this: a moderately parallel program uses the first few
workers and leaves the high-numbered ones idle, which is not a sign of a bottleneck.
Worker numbers are not CPU numbers, though. The workers are not pinned, and the OS
decides which core each one runs on.

### Passing the wake on

A worker that finds a task while searching, by stealing or in the injection queue
re-check, calls `notify_if_needed()` after it leaves the searching state and before it
runs the task. Tokio does the same in `transition_worker_from_searching()`.

`enqueue()` alone does not get a burst of work to every worker. It wakes at most one
worker per push, and it picks the lowest idle bit, which stays set until the chosen
worker has woken up and cleared it. A task that spawns ten tasks in a row does so
faster than a parked thread wakes, so all ten pushes pick the same worker:

```mermaid
sequenceDiagram
    participant W0 as Worker 0 (spawning)
    participant W1 as Worker 1 (parked)
    participant W2 as Worker 2 (parked)
    W0->>W1: push task 1, unpark
    Note over W0: pushes 2..10 find worker 1's idle bit<br/>still set and unpark it again: no effect
    W1->>W0: wakes, steals half
    W1->>W2: found work while searching: unpark
    W2->>W0: wakes, steals from worker 0 or 1
    Note over W2: wakes the next idle worker in turn
```

Without the last two steps worker 1 is the only help the burst gets. It runs one of the
stolen tasks and the rest wait in two local queues while every other worker sleeps,
until something else happens to be enqueued. Tasks that suspend or spend their
[budget](task_budget.md) often hide this, because every wake of a task is another
`enqueue()`. Tasks that run for a long time in one poll do not.

The chain stops by itself: each woken worker wakes one more only if it found a task, and
`notify_if_needed()` does nothing when a worker is already searching or none is idle.

### The last searcher looks again

`notify_if_needed()` wakes nobody while a worker is searching, on the grounds that the
searcher will find the task. That holds only if the searcher has not yet swept the queue
the task went to:

```mermaid
sequenceDiagram
    participant K as Worker k (running a task)
    participant S as Worker s (searching)
    S->>K: sweep: k's queue is empty
    K->>K: push a task to its own queue
    Note over K: m_searching is 1: wake nobody
    S->>S: sweep ends with nothing, m_searching becomes 0
    S->>K: last searcher: is any peer's queue non-empty?
    Note over S: yes: clear idle bit, search again, steal
```

The last step is the fix, and it is what Tokio does in `notify_if_work_pending()`. A
worker whose decrement takes `m_searching` to zero, and which then finds nothing in its
own queue or the injection queue, reads the length of every peer's local queue
(`peer_has_tasks()`) before it parks. If one is non-empty it clears its idle bit and
runs the loop again. Without it the task waits behind whatever worker k is running,
with idle workers asleep, until the next wake of any kind.

Only the last searcher looks. An earlier one leaves a searcher behind it, and that one
will be last in its turn.

!!! danger "WARNING: the read of `m_searching` in `notify_if_needed()` must be seq_cst"
    The pushing worker stores to its queue and then reads `m_searching`; the searcher
    stores to `m_searching` and then reads the queue. Acquire and release do not order
    a store against a later load of a different variable, so with them both sides could
    miss each other. `notify_if_needed()` therefore reads with
    `fetch_add(0, seq_cst)` and the worker decrements with `fetch_sub(1, seq_cst)`,
    as Tokio's `Idle` does. Do not weaken either to make `enqueue()` cheaper.

!!! note "NOTE: a worker that parks without having searched does not look"
    When `max_searching` workers are already searching, a further idle worker goes
    straight to parking and makes no check of its peers. A push that sees no searcher
    and no idle bit, just before that worker sets its bit, wakes nobody. Tokio has the
    same window. Nothing is lost: the task is in the queue of a worker that is running
    and will get to it. It only misses a chance to run in parallel.

There is no test for this alone. The window is a few instructions wide and cannot be
held open from outside the executor; the stress tests and the benchmarks run through it.

!!! note "NOTE: the last wake of a chain may find nothing"
    The task a worker steals may have been the only one left. The worker it then wakes
    sweeps the queues, finds nothing and parks again. That is one wasted wake per burst,
    the price of not counting what is left in the victim's queue.

### Batched wakes (`WakeBatch`)

The chain above gets a burst to every worker eventually, but one worker at a time: each
link costs a thread wake-up, a steal sweep and another unpark. When the burst is the
start of a short computation split across the workers, such as a
[barrier](barrier.md) releasing its waiters or [`parallel()`](parallel.md) spawning its
parts, the ramp-up can take longer than the work.

The code that makes the burst knows how big it is. `detail::WakeBatch` lets it say so:
while a batch is open on a thread, `enqueue()` calls made on that thread for this
executor collect the tasks instead of pushing and notifying one by one. Closing the batch
pushes them all at once and wakes as many workers as it has tasks:

```cpp
{
    detail::WakeBatch batch;
    for (auto& waker : to_wake)
        waker->wake();        // enqueue() appends to the batch; no push, no notify
}   // ~WakeBatch: one injection-queue push, up to to_wake.size() workers woken
```

#### Scope

The batch belongs to the thread that opened it and to the stretch of synchronous code
between its constructor and destructor:

- **A thread-local pointer.** The constructor saves the current
  `t_wse_wake_batch` and installs itself; the destructor flushes and restores the saved
  pointer. A nested batch therefore flushes its own tasks when it closes, and the
  outer one carries on.
- **Read only by `enqueue()`, only on that thread.** `worker_loop()` doesn't change, and
  no other thread ever sees the batch. The cost on the normal path is one more
  thread-local load and branch in `enqueue()`, next to the `t_wse_in_driver_turn` check
  it already makes.
- **One executor per batch.** The first diverted task binds the batch to its executor.
  A task for a different `WorkStealingExecutor` goes through the normal path. Other
  executors never look at the pointer, so a `WakeBatch` on `CurrentThreadExecutor`,
  `WorkSharingExecutor` or Pico costs nothing and does nothing; callers can open one
  unconditionally.
- **Any thread.** The thread need not be a worker. On a worker, the batch replaces the
  local push and its notify; elsewhere, it replaces the injection-queue push and its
  notify.

!!! danger "WARNING: never hold a `WakeBatch` across a suspension"
    The pointer is per thread, not per task. If a coroutine suspended with a batch open,
    the tasks other code woke on that worker would collect in the batch, unwoken, until
    the coroutine resumed; and it might resume on another worker, whose thread-local the
    destructor would then restore wrongly. Treat a batch like a held mutex: open it as a
    stack local inside a future's `poll()`, where a suspension is impossible, or, in a
    coroutine, close it before the next `co_await`. A debug build asserts after each
    task poll that no batch is open.

`WakeBatch` lives in `include/coro/detail/wake_batch.h` and is not public API. The
library uses it in `Barrier`'s release, in a [Team](team.md)'s wakes and in `parallel()`'s
spawn loop.

#### Flush

```mermaid
sequenceDiagram
    participant W as Worker 0 (closing the batch)
    participant Q as Injection queue
    participant W1 as Worker 1 (parked)
    participant W2 as Worker 2 (parked)
    participant W3 as Worker 3 (parked)
    W->>Q: lock, push k tasks, update m_injection_len
    W->>W1: unpark
    W->>W2: unpark
    W->>W3: unpark
    Note over W1,W3: each takes its share in Step 2
```

1. **One push.** All `k` tasks go to the back of the injection queue under one hold of
   `m_mutex`. A backlog already there runs first; the batch doesn't jump the queue.
2. **Wake `min(k, idle)` workers.** The flush reads `m_idle_mask` once and calls
   `unpark_worker()` on that many distinct set bits, lowest first.
3. **No searching short-circuit.** `notify_if_needed()` wakes nobody while a worker is
   searching; the flush ignores `m_searching`. One searcher would find one task, and the
   rest would ramp up by the chain again.

Woken workers find the tasks in Step 2, before the searching cap of Step 3 can turn them
away. Step 2 takes a `len / n + 1` share, so with `k` tasks and `k` woken workers some
take two and a later one finds the queue empty. That one goes on to steal from a worker
that took two, which is the chain again but only for the last task.

`k` wakes, not `k - 1`: the closing worker is usually still busy afterwards (a barrier
leader goes straight on to its next phase). If it isn't, it takes a task from the queue
in its own Step 2 and one woken worker finds nothing, which costs one wasted wake.

Races:

- **A bit read as set whose worker has just woken for another reason.** The unpark banks
  a token and that worker's next park returns at once: one extra loop iteration. Same as
  `notify_if_needed()`.
- **A worker that sets its idle bit just after the flush read the mask.** It re-checks the
  injection queue under `m_mutex` in Step 4 before parking, and finds the tasks.
- **Fewer idle workers than tasks.** The rest wait in the injection queue for a busy
  worker to reach Step 2 or its periodic check. Every worker is busy, so nothing is gained
  by waking more.

!!! danger "WARNING: the destructor must flush, also during unwinding"
    The collected tasks are `Notified`; nothing else will enqueue them again. A batch
    that dropped them would be a lost wake-up, and a hang. The destructor is `noexcept`
    and flushes whether the scope ends normally or by an exception.

The tasks are held as `TaskBase*`, as in the run queues: the owned-task list keeps them
alive. Up to 64 fit in the batch itself; more spill into a vector, so the usual batch
allocates nothing.

#### What it replaces and what it doesn't

- **The driver-turn deferral stays as it is.** Wakes during a driver turn are local
  pushes that skip `notify_if_needed()` (see [Parking in the I/O driver](#parking-in-the-io-driver)).
  No batch is ever open during a turn: the driver calls wakers, it doesn't poll futures.
- **The chain stays.** It still spreads bursts made without a batch: a task that spawns in
  a loop, or a driver turn that dispatches many tasks.
- **No LIFO slot.** A task diverted into a batch never goes to the
  [LIFO slot](#lifo-slot); a batch is for spreading work across workers.

### Parking and Wakeup

#### What park and unpark are

At the OS level, "parking" a thread means putting it to sleep until another thread
explicitly wakes it. On Linux this is a **futex** (`futex(FUTEX_WAIT)` to sleep,
`futex(FUTEX_WAKE)` to wake). On Windows it is `WaitOnAddress` / `WakeByAddressSingle`.
The key property shared by all implementations: the wake call is lock-free and targets
a specific thread rather than waking the entire pool.

A `std::condition_variable` is built on the same futex primitives but wraps them behind
a mutex. The mutex is necessary because `std::condition_variable` has no internal state
— it cannot remember that a notification was sent before a waiter arrived. Without the
lock, the following race is possible:

```
worker:   check predicate → false
notifier: predicate becomes true, notify_one() fires (nobody is waiting yet)
worker:   enters wait() — sleeps forever (notification was lost)
```

The mutex closes this window by ensuring the notifier cannot fire between the predicate
check and the `wait()` call.

#### How Tokio parks workers

Tokio avoids the global mutex entirely. Each worker owns a `Parker` struct containing:

- A small per-worker atomic (`AtomicUsize`) with three states: `EMPTY`, `PARKED`,
  `NOTIFIED`.
- The OS thread handle needed to call `unpark()` on that specific thread.

Rust's `std::thread::park()` / `unpark()` maps directly to futex on Linux. The atomic
state machine eliminates the lost-wakeup race without a mutex: if `unpark()` is called
before `park()`, the state transitions to `NOTIFIED` and the subsequent `park()` returns
immediately without ever touching the OS.

An **idle set** (a bitmask over worker indices, stored as an `AtomicU64` for pools up to
64 threads) tracks which workers are parked. When a task is enqueued, the enqueueing
thread reads the idle set, picks a parked worker, and calls `unpark()` on its handle —
all without acquiring any lock.

The comparison to our current approach:

| | `WorkSharingExecutor` condvar | Tokio-style per-worker park |
|---|---|---|
| Sleep | `m_cv.wait(lock)` — acquires `m_mutex` | `thread::park()` — futex only |
| Wake one | `m_cv.notify_one()` — acquires `m_mutex` | `handle.unpark()` — atomic + futex |
| Wake all (shutdown) | `m_cv.notify_all()` — acquires `m_mutex` | iterate idle set, unpark each |
| Lock contention | All workers share one mutex | None on the hot path |
| Which worker wakes | Unspecified (OS choice) | Caller chooses by index |

#### C++ equivalent: `std::binary_semaphore`

C++ has no direct `park()`/`unpark()` in the standard library. The two closest options
are:

- **`std::atomic<uint32_t>::wait()` / `notify_one()`** (C++20) — thin wrappers over
  futex. Require the caller to manage state and handle spurious wakeups manually.
- **`std::binary_semaphore`** (C++20) — a semaphore with a maximum count of 1.
  `acquire()` blocks if the count is 0; `release()` increments the count and wakes one
  waiter. On Linux, `libstdc++` and `libc++` both implement this with futex.

`std::binary_semaphore` is the right choice because it has the same "notify-before-wait"
semantics as Rust's `park()`/`unpark()`: if `release()` fires before `acquire()`, the
token is banked and `acquire()` returns immediately. The lost-wakeup race is closed by
the semaphore's internal count — no external mutex is required.

Per-worker parking with `std::binary_semaphore` would look like:

```cpp
struct WorkerSlot {
    std::thread             thread;
    std::binary_semaphore   parker{0};  // 0 = no pending wake
};
std::vector<WorkerSlot> m_workers;
std::atomic<uint64_t>   m_idle_mask{0}; // bit k set ↔ worker k is parked

static_assert(MAX_WORKERS <= 64, "m_idle_mask is a uint64_t; max 64 workers supported");

// Worker parks:
m_idle_mask.fetch_or(1ull << worker_index, std::memory_order_release);
m_workers[worker_index].parker.acquire();  // futex wait
m_idle_mask.fetch_and(~(1ull << worker_index), std::memory_order_relaxed);

// Enqueuer wakes one idle worker:
uint64_t idle = m_idle_mask.load(std::memory_order_acquire);
if (idle) {
    int idx = std::countr_zero(idle);  // pick lowest idle worker
    m_workers[idx].parker.release();   // futex wake — no mutex
}
```

#### 64-worker cap

`m_idle_mask` is a `uint64_t`, so the pool is capped at 64 workers. The constructor
clamps `num_threads` to `MAX_WORKERS` rather than throwing: with a throw, the default
`Runtime()` (sized by `hardware_concurrency()`) failed outright on >64-core machines.

!!! tip "TODO: Lift the 64-worker cap"
    Extra hardware threads beyond 64 go unused. See the roadmap entry
    "WorkStealingExecutor: more than 64 workers" for the two candidate designs
    (multi-word idle bitmap, or tokio-style counters plus a mutex-protected sleeper list).

#### Park protocol: avoiding lost wakeups

`binary_semaphore` closes the "notify arrives before `acquire()`" race (the token is
banked), but a different race remains between marking idle and parking:

```
worker:   finds all queues empty — not yet in m_idle_mask
enqueuer: pushes task, reads m_idle_mask → no idle workers, skips wake
worker:   sets bit in m_idle_mask, calls acquire() → sleeps with task pending ✗
```

The fix: after setting the idle bit, **re-check all queues once more** before calling
`acquire()`. If work is found, clear the bit and resume the worker loop. If not, park —
and if the enqueuer raced and called `release()` after the idle bit was set, the
semaphore token is banked and `acquire()` returns immediately without sleeping.

```
worker loop (park sequence):
  1. decrement m_searching (no longer searching)
  2. set idle bit in m_idle_mask  (release store)
  3. re-check local queue and injection queue; the last searcher also checks
     whether any peer's queue is non-empty (see "The last searcher looks again")
  4. if work found: clear idle bit, loop back to step 1 of worker loop
  5. else: parker.acquire()       (sleeps only if no banked token)
  6. clear idle bit (already cleared by enqueuer's race wins too — harmless double-clear)
  7. resume worker loop
```

`WorkStealingExecutor` first used per-worker `std::binary_semaphore` parking,
replacing the shared `m_mutex` + `m_cv` used by `WorkSharingExecutor`. Once workers
could also park in the I/O driver, the semaphore became a per-worker park slot (mutex,
condvar and state); see [Parking in the I/O driver](#parking-in-the-io-driver). The
per-worker approach was adopted because:

- It is the natural C++ equivalent of Tokio's `park()`/`unpark()`.
- It makes Q4 (notify on local enqueue) cheap enough to always do correctly: check
  `m_searching` and `m_idle_mask` atomically, then call `unpark_worker()` on one
  idle worker — no shared-mutex round-trip.
- Shutdown (`notify_all`) becomes: call `unpark_worker()` on each worker.

The shared `m_mutex` is retained only for protecting the injection queue (remote
enqueue path). It is no longer used for parking/wakeup.

### LIFO slot

When a task wakes another task on the same worker (a sender waking its receiver, the last
part of a join waking the task that waits on it), the woken task is put in a one-task slot
and polled as soon as the current poll returns, ahead of the local queue. The data the
waker just wrote is still in this core's cache, and the consumer doesn't wait behind
everything else that is queued. Tokio has the same slot.

The slot is part of `LocalRunQueue`'s owner handle (`push_lifo()`, `pop_lifo()`); see
[Not stealable](#not-stealable-as-in-tokio). It exists in the `CORO_USE_LOCAL_RUN_QUEUE`
build only; the mutex-backed deque has no slot.

#### Which wakes use it

A wake goes to the slot only when all of these hold:

- `enqueue()` is called on a worker of this executor (the local path);
- not inside a driver turn (those are run in order after the turn);
- not into a [`WakeBatch`](#batched-wakes-wakebatch);
- the task is not the one the worker is polling. A task that wakes itself, including
  every yield and every spent budget, is re-queued after its poll by the worker loop,
  which pushes it to the back of the ring directly, never to the slot. Otherwise a yield
  would poll the same task again at once (see
  [What a yield does](task_budget.md#what-a-yield-does)).

A task spawned on a worker takes the same path (`schedule()` calls `enqueue()`), so it
too lands in the slot and runs once the spawning poll returns, as in tokio. A burst of
spawns keeps only the last in the slot; each one it displaces goes to the ring with a
notify, so peers can steal them. To start several tasks across the workers at once, spawn
them inside a [`WakeBatch`](#batched-wakes-wakebatch), as `parallel()` does.

```mermaid
flowchart TD
    A["enqueue(task)"] --> B{"WakeBatch open<br/>for this executor?"}
    B -- yes --> C["append to batch"]
    B -- no --> D{"local?"}
    D -- no --> E["injection queue, notify_if_needed()"]
    D -- yes --> F{"in driver turn?"}
    F -- yes --> G["push to ring, no notify"]
    F -- no --> H["swap into LIFO slot"]
    H --> I{"slot held a task?"}
    I -- yes --> J["push it to ring, notify_if_needed()"]
    I -- no --> K["no notify"]
```

A task placed in an empty slot wakes nobody: it is meant to run next on this worker, and a
peer that woke to steal it would defeat that. A task it displaces goes to the back of the
ring and is notified for as any local push.

#### Running it

The worker takes the slot before anything else, at the top of each turn of its loop:
after a poll and the state transitions that follow it, and also after a task destroyed at
the end of the previous turn has woken another into the slot.

- **At most 3 in a row.** Two tasks that keep waking each other would otherwise hold the
  worker, with the periodic injection check and the local queue never reached. After three
  consecutive slot polls the slot's task is moved to the back of the ring and the loop
  carries on as normal. Tokio uses the same limit (`MAX_LIFO_POLLS_PER_TICK`).
- **The budget carries over.** A slot poll gets what was left of the
  [budget](task_budget.md) of the poll that woke it, as in tokio, so a ping-pong pair
  shares one budget rather than getting a fresh one per hop. With none left, the slot's
  task goes to the back of the ring instead, as for the three-poll limit.
- **Counted as a poll.** Slot polls count toward `kEventInterval`, so busy driver turns
  still happen every 61 polls.

#### Not stealable, as in tokio

Only the owning worker ever reads or writes the slot. The task in it was woken because
the task that just ran handed it something: a value through a channel, the last result
of a join. That data is in this core's cache now. A thief that took the task would run
it on a cold core, which is the cost the slot exists to avoid, so stealing it works
against the slot's purpose.

It also makes the slot free. With no thief, it is a plain `TaskBase*` in the owner's
`Local` handle: no atomic, no cache line that another core can touch. Changes to
`LocalRunQueue`:

- the slot moves out of the shared `Inner` into `Local`, as a plain pointer;
- `steal_into()` loses its fallback to the slot;
- `Steal::len()` and `is_empty()` stop counting it. Otherwise `peer_has_tasks()` would
  report a task no thief can take, and the last searcher would loop instead of parking.
  `Local::len()` still counts it, for `after_turn()`.

Parking can't strand a slot task: the worker takes the slot before anything else after
each poll, so it is always empty by the time the worker gets to Step 1.

!!! warning "Hazard: a task in the slot waits for the poll that woke it"
    If the task that woke it goes on working in the same poll, the woken task waits
    for that poll to end, with other workers idle, and no other worker can take it.
    The budget bounds the wait for code that awaits; code that doesn't is bounded
    only by the poll. Tokio has the same limit; its builder has an unstable option to
    turn the slot off.

    Long compute polls belong apart from latency-sensitive tasks: in
    `spawn_blocking()`, or on a runtime of their own. See
    [BL.5](../guidelines.md#bl5-keep-long-computations-off-a-runtime-that-serves-latency-sensitive-tasks).

#### Tests

In `test/runtime/test_work_stealing_executor.cpp`. Stealing makes the order in which
tasks run nondeterministic, so these test that everything still runs, not the order:

- A ping-pong pair through two `Event`s for 10,000 rounds completes.
- A ping-pong pair on one worker, with 20 tasks queued behind it and the other worker held
  by a blocking task so it can't steal them: the queued tasks all run while the pair is
  still going. Only the three-poll cap lets them in.

Not covered by an automated test: that the slot task runs before an earlier-queued one,
that `yield_now()` lets a queued task run first, that a searching peer leaves a slot task
alone during a long poll, and that batched and driver-turn wakes skip the slot. Each
would need a hook into the worker loop to observe deterministically.

---

## Parking in the I/O driver

I/O readiness and timers fire only where some thread turns the runtime's `IoDriver`
(see [I/O Driver](io_driver.md)). `WorkStealingExecutor` follows tokio's multi-thread
scheduler, and its workers share one driver:

- **Parking.** The first worker to run out of work takes the driver and parks in
  `try_turn(nullopt)`. Other idle workers park on their own condition variable.
- **Wakes during a turn.** Wakes fired inside the turn are local enqueues onto the
  turning worker's own queue.
- **Busy workers.** A busy worker does a non-blocking `try_turn(0)` every
  `kEventInterval` (61) task polls, so I/O and timers aren't starved when no worker is
  idle.

The idle-mask and searching protocol above is unchanged. What parks is a per-worker park
slot that knows whether the worker is blocked in the driver or on its condition variable.

### Driver API

A worker must not block on the driver's mutex while another worker holds it, so
`IoDriver` has a try-variant:

```cpp
/// Like turn(), but returns std::nullopt at once if another thread is turning.
std::optional<std::size_t> try_turn(std::optional<std::chrono::nanoseconds> timeout);

/// Like try_turn(), but calls before_poll() once it holds the driver; false releases
/// the driver without polling and returns 0.
template<typename BeforePoll>
std::optional<std::size_t> try_turn(std::optional<std::chrono::nanoseconds> timeout,
                                    BeforePoll&& before_poll);
```

### Per-worker park state

Each `WorkerSlot` holds `park_mutex`, `park_cv` and `park_state`, GUARDED BY
`park_mutex`:

```mermaid
stateDiagram-v2
    [*] --> Empty
    Empty --> Notified: unpark_worker()
    Empty --> ParkedDriver: park_worker() got the driver
    Empty --> ParkedCondvar: park_worker(), driver busy
    ParkedDriver --> Empty: try_turn() returned
    ParkedDriver --> Notified: unpark_worker() → driver.unpark()
    ParkedCondvar --> Notified: unpark_worker() → cv.notify_one()
    Notified --> Empty: park_worker() consumes the token
```

```cpp
// Returns the number of I/O events and timers dispatched (0 if this worker didn't turn).
std::size_t park_worker(i):
    std::optional<size_t> n;
    { InDriverTurn guard;                 // thread-local: wakes in the turn are local, no notify
      n = driver.try_turn(nullopt, [&] {  // runs only once this worker holds the driver
          lock(slot.m);
          if (slot.state == Notified) return false;   // token banked: don't poll
          slot.state = ParkedDriver;
          return true; }); }
    lock(slot.m);
    if (!n && slot.state != Notified) {   // driver busy: fall back to the condvar
        slot.state = ParkedCondvar;
        slot.cv.wait(lock, [&]{ return slot.state == Notified; });
    }
    slot.state = Empty;
    return n.value_or(0);

void unpark_worker(i):
    { lock(slot.m); prev = slot.state; slot.state = Notified; }
    if      (prev == ParkedDriver)  driver.unpark();       // sticky eventfd write
    else if (prev == ParkedCondvar) slot.cv.notify_one();
    // prev == Empty or Notified: the token is banked; the next park_worker() returns at once
```

`notify_if_needed()` calls `unpark_worker(idx)` for the lowest idle worker. The destructor
sets `m_stop` and then calls `unpark_worker()` on every worker.

### Worker loop

- **Park** calls `park_worker(i)`, then clears its idle bit. If the turn dispatched
  anything and the worker's local queue now holds more than one task, `after_turn()` calls
  `notify_if_needed()` so another worker can help (tokio's `should_notify_others`). With
  exactly one task the worker just runs it, so a single I/O wake never costs a
  cross-thread wake.
- **After each task poll,** a counter is bumped. Every `kEventInterval` polls, the worker
  calls `try_turn(0)` inside an `InDriverTurn` guard and applies the same `after_turn()`
  rule. If another worker holds the driver, the call returns at once and costs nothing.
- **`enqueue()` on a worker of this executor** skips `notify_if_needed()` while the
  thread-local `InDriverTurn` flag is set. The turning worker runs those tasks itself as
  soon as the turn returns. Remote enqueues are unchanged.

### Parking and its races

- **A remote wake before the worker parks** (the lost-wakeup check). The enqueuer pushes,
  then reads `m_searching` and `m_idle_mask`. The worker sets its idle bit, then re-checks
  its queues. This is a store-then-load pattern on both sides. It is sound only because
  every queue a parking worker re-checks is mutex-protected, or only ever pushed to by its
  owner:
    - the injection queue is mutex-protected;
    - with `CORO_USE_LOCAL_RUN_QUEUE` (the default), only the owner pushes to its own
      local queue;
    - without it, `WorkStealingDeque` is mutex-protected.

    The mutex orders the two sides: either the worker's re-check sees the task, or the
    enqueuer's later load sees the idle bit.
- **A remote wake while the worker is in the driver, or about to be.** The worker sets
  `ParkedDriver` only once it holds the driver (inside `before_poll`). `unpark_worker()`
  sees it and calls `driver.unpark()`. That write is sticky, and only the driver's holder
  ever consumes it, so it reaches this worker's `epoll_wait` even if it lands first.

    !!! danger "WARNING: publish ParkedDriver only while holding the driver"
        If a worker set `ParkedDriver` and *then* called `try_turn()`, an unpark in that
        gap could write the eventfd while another worker was in a turn (a busy worker's
        `try_turn(0)`, or the previous holder on its way out). That worker would reset
        the eventfd, and the parking worker would then win the driver and block in
        `epoll_wait` with `Notified` unread: a lost wake-up, which shows up as a rare hang
        (most likely at `Runtime` teardown, whose unparks have no other backstop).
- **A remote wake while the worker is on the condvar.** `unpark_worker()` sees
  `ParkedCondvar`, sets `Notified`, and notifies. The wait predicate makes a spurious or
  early return harmless.
- **An unpark after a failed `try_turn()`, before the condvar wait.** The slot is still
  `Empty`, so `unpark_worker()` banks `Notified`; the worker checks it under the slot
  mutex and returns without waiting.
- **A stale driver unpark (benign).** The holder returns from `turn()` for an I/O event
  just before an `unpark_worker()` sees `ParkedDriver` and writes the eventfd. The next
  worker to turn returns at once, finds nothing, and parks again. Tokio has the same
  property.
- **Two enqueuers wake the same idle worker.** Both read its idle bit before the worker
  clears it. `Notified` is idempotent, so the second unpark only re-banks the token, at
  the cost of one extra loop iteration. (With a `binary_semaphore`, a second `release()`
  while the count is already 1 is undefined behaviour.)
- **Wakes dispatched inside a turn.** They fire on the turning worker, whose idle bit is
  still set. Without the `InDriverTurn` skip, `notify_if_needed()` could pick the turning
  worker itself (an eventfd write that makes its next turn return at once), or wake a
  peer that steals a task the turning worker was about to run. Both are pure overhead
  for the common "one event, one task" case.
- **The driver goes unturned while the holder runs tasks.** After the holder leaves
  `turn()`, nobody is in the driver until:
    - it parks again,
    - another worker parks and finds the driver free, or
    - some busy worker reaches its `kEventInterval` turn.

    Edge-triggered events wait in the kernel meanwhile, so nothing is lost. But a single
    long-running task on the old holder delays I/O and timer delivery when every other
    worker sits on its condvar. Tokio has the same property.
- **Shutdown.** The destructor sets `m_stop` under `m_mutex`, then unparks every worker.
  A worker in the driver gets `driver.unpark()`, and one on the condvar gets notified.
  Each sees `m_stop` and exits. The driver outlives the executor (`Runtime` member
  order), so the unparks are safe.

!!! tip "PERF: hand the driver off when the holder leaves it with work"
    To close the latency gap in the "driver goes unturned" case, a holder that returns
    from `turn()` with tasks to run could wake one condvar-parked worker, which would then
    take the driver. That costs a futex wake per driver wake-up whenever idle workers
    exist, and at high event rates it could ping-pong the driver between two workers on
    every event. Tokio doesn't do it either; decide from profiling.

!!! tip "PERF: prefer condvar-parked workers in `notify_if_needed()`"
    `notify_if_needed()` wakes the lowest idle bit, which may be the driver holder. Waking
    it costs an eventfd write and leaves the driver unturned until it parks again. Waking
    a condvar-parked worker first would avoid that, but needs a "driver holder" index next
    to the idle mask. Measure before adding it.

!!! tip "PERF: mutex + condvar park slot instead of a `binary_semaphore`"
    A semaphore can only wake a worker that waits on it. Once workers can also park in the
    driver, `unpark_worker()` has to know where the worker is parked: in `epoll_wait`,
    which needs `driver.unpark()`, or on its own wait primitive. The worker changes that
    state partway through parking (`ParkedDriver`, or `ParkedCondvar` after a failed
    `try_turn()`). The waker's read-and-set of the state has to be atomic with respect to
    that change, or the wake goes to the wrong place and is lost. A mutex keeps that
    protocol obvious, as CLAUDE.md prefers, and `cv.wait(lock, pred)` makes the move to
    `ParkedCondvar` and the sleep one step.

    Tokio's multi-thread parker (`scheduler/multi_thread/park.rs`) has the same shape:
    `EMPTY`/`PARKED_CONDVAR`/`PARKED_DRIVER`/`NOTIFIED`, with a mutex and a condvar.

    Compared with the semaphore:

    - **Sleep and wake:** the same futex wait, futex wake and context switch either way.
      This is the dominant cost, several µs.
    - **Extra work per park and unpark:** one uncontended lock and unlock, about 20–50 ns
      with no syscall. `notify_one()` with no waiter does not make a syscall either.
    - **Contention:** a waker can briefly contend on `park_mutex` with a worker that is
      waking up. `unpark_worker()` notifies after unlocking to keep that window short.

    Unparks happen only when the idle mask shows a sleeping worker, so a busy runtime
    rarely reaches this path. If profiling shows a regression, add tokio's fast path:
    make `park_state` an atomic that the waker `swap(Notified)`s, and take the lock and
    notify only when the old state was `ParkedCondvar`. That falls under the
    "scheduling-state atomics" exception in CLAUDE.md. Do not add it without numbers.

!!! warning "FIXME: lost wakeup on the non-`CORO_USE_LOCAL_RUN_QUEUE` affinity path"
    Without `CORO_USE_LOCAL_RUN_QUEUE`, a remote `enqueue()` pushes to the task's last
    worker `k` (the affinity path). It then calls `notify_if_needed()`, which returns early
    if any worker is searching. Before the last searcher re-checked its peers' queues, the
    searcher looked only at its own queue and the injection queue before parking, never
    `k`'s, and if `k` was parked the task stranded until some later wake. That re-check
    should now find the task and steal it, but this has not been tested in that build,
    which the default does not use. If it still strands, unpark `k` directly when its idle
    bit is set, or drop the affinity path.

### Tests

| Test | Proves |
|---|---|
| `IoDriver.TryTurnReturnsNulloptWhileAnotherThreadTurns` | `try_turn()` never blocks on a held driver. |
| `IoDriver.TryTurnDispatchesWhenFree` | `try_turn()` behaves like `turn()` otherwise. |
| `WorkStealingIoTest.IoEventWakesTaskOnIdleRuntime` | An idle worker parked in the driver receives I/O (socketpair, writer thread after 50 ms). |
| `WorkStealingIoTest.BusyWorkersStillPollIo` | With every worker spinning on self-waking tasks (nobody parks), an I/O wait still completes through the `kEventInterval` turns. |
| `WorkStealingIoTest.ManySocketsPingPong` | 16 socketpairs × 200 round trips on `Runtime(4)`. A lost wakeup anywhere in the handoff shows up as a hang. |
| `WorkStealingIoTest.RemoteWakeUnparksDriverHolder` | A wake from a foreign thread reaches a runtime whose only idle worker is in the driver. |
| `WorkStealingIoTest.ShutdownWithPendingIoWait` | Destroying the runtime with a spawned task still waiting on a socket neither hangs nor crashes. |

These are in `test/runtime/test_io_driver.cpp` and `test/runtime/test_work_stealing_io.cpp`.
`ReadByteFuture`, `AsyncByteReader` and `SocketPair` live in the shared
`test/runtime/io_test_util.h`. Every idle worker parks through this path, so the
`AllExecutors` and work-stealing suites exercise it heavily too.

---

## Data Structures

### WorkStealingDeque

The current `WorkStealingDeque<T>` is mutex-backed and presents `push` / `pop` /
`steal`. A `steal_half(WorkStealingDeque& dst)` method is added: under the victim's
lock it computes `n = (size + 1) / 2`, moves the front `n` items into `dst`, and
returns `n`. The caller pops one task immediately and leaves the rest in its own queue
for future iterations. This amortizes steal overhead and avoids repeated lock
acquisitions when a victim has many tasks.

**The mutex-backed implementation will be kept for this phase.** The lock-free Chase-Lev
upgrade is deferred — see the
[Future Work: Chase-Lev Lock-Free Deque](#future-work-chase-lev-lock-free-deque) section
below for a full description of how it works and what changes when the time comes.

### Injection Queue

A `std::deque<TaskBase*>` protected by `m_mutex`. Remote wakers, `schedule()` calls from
non-worker threads, and local-queue overflow push here. Workers pop from it under the lock.

The queue's length is also published in `std::atomic<std::size_t> m_injection_len`,
written only under `m_mutex`. A worker whose local queue is empty reads it first and
takes the lock only when it is non-zero. Tokio's inject queue does the same with its
`len: AtomicUsize` and `is_empty()`.

A zero read can be stale, so it is used only where a miss is harmless: Step 2 of the
worker loop and the re-check after a steal sweep. The check before parking still takes
`m_mutex` after the idle bit is set. A push that races with a lock-free read is
therefore found before the worker sleeps, or its `notify_if_needed()` sees the idle bit.

With the fixed-capacity local run queue (`CORO_USE_LOCAL_RUN_QUEUE`), Step 2 takes a
batch instead of one task. The first task runs immediately and the rest are pushed onto
the worker's local queue. The batch size follows Tokio's `Core::next_task()`:

```cpp
cap  = min(local.remaining_slots(), local.max_capacity() / 2);
want = max(1, min(injection_len / num_workers + 1, cap));
```

The `1/num_workers` share leaves work for the other workers. The half-capacity limit
keeps the batch in the half of the ring that overflow never spills back to the
injection queue. The other injection-queue pops (after a steal sweep, before parking)
still take one task.

!!! note "NOTE: why Step 2 reads the length lock-free and pops in batches"
    With the owned tasks sharded, a profile of `SkynetJoinTest.Stealing` put 36% of
    samples on waiting for, or waking waiters of, `m_mutex` in Step 2. Adding the
    lock-free length test alone moved that only to 25%, still at the Step 2 lock, so
    the queue was not empty. The likely source is a fan-out workload overflowing the
    256-slot local queues: each overflow spills a batch into the injection queue, and
    workers were then taking the lock once per task to get them back.

### Worker Affinity

`TaskBase` carries `last_worker_index` (initialized to -1). After each successful
`poll()`, the executor stores `t_worker_index` into it. When `wake()` fires,
`enqueue()` uses the stored index to push directly onto that worker's local queue
instead of the injection queue, provided the executor pointer still matches.

The benefit: a woken task re-enters on the same core it last ran on, keeping its data
warm in L1/L2 and avoiding an injection-queue lock acquisition on the hot wakeup path.

### State Additions

A single shared atomic `std::atomic<int> m_searching{0}` tracks the number of workers
currently performing a steal sweep. No per-worker state is needed.

### Owned tasks

The executor holds one `shared_ptr` to every live task from `schedule()` until `poll()`
returns done. Run queues hold raw `TaskBase*`, so this reference is what keeps a
suspended task alive.

The references live in `detail::OwnedTasks` (`include/coro/detail/owned_tasks.h`), which
follows Tokio's `OwnedTasks` / `ShardedList`: an array of shards, each an intrusive
doubly-linked list behind its own `std::mutex`.

| Aspect | Choice |
|---|---|
| Shard count | `min(65536, bit_ceil(workers) * 4)`, the rule Tokio uses |
| Shard selection | Hash of the task's address. Tokio uses its task ID; coro tasks have none, and the address is fixed for the task's lifetime |
| Links | `TaskBase::owned_prev` / `owned_next`, stored in the task |
| Strong reference | `TaskBase::owned_self`, stored in the task and cleared on removal |
| Insert / remove | Lock one shard, a few pointer writes, no allocation |

Storing the strong reference in the task makes a deliberate `shared_ptr` cycle. It is
broken by `OwnedTasks::remove()` when the task finishes, or by `~OwnedTasks()` at
executor shutdown for tasks that never finished. Both release the reference after
dropping the shard lock, so task and user destructors never run under it.

Each shard also has a `closed` flag, as Tokio's does, set by `close_and_collect()` when
the runtime shuts down. `insert()` and `remove()` report it, under the shard lock they
already hold. A task inserted into a closed shard is not rejected as in Tokio: it is
born cancelled and drained (see [runtime_shutdown.md](runtime_shutdown.md)). Tokio's
live-task counter is left out because nothing in coro reads it.

!!! note "NOTE: why the owned tasks are sharded"
    The first version was one `std::unordered_set<shared_ptr<TaskBase>>` behind one
    mutex. Every spawn and every completion on every worker took that lock. A
    frame-pointer profile of `SkynetJoinTest.Stealing` attributed 79% of samples to
    waiting on that mutex or waking its waiters, with about 7.5 s of system time against
    2.5 s of user time for 1.25 s of wall time.

!!! tip "TODO: `WorkSharingExecutor` and `CurrentThreadExecutor` still use a single set"
    `CurrentThreadExecutor` has no contention to remove, but the intrusive list would
    still save it a hash-table insert and erase per task.

---

## enqueue() Routing

```cpp
void WorkStealingExecutor::enqueue(shared_ptr<Task> task) {
    int affinity = task->waker().last_worker_index();

    if (t_worker_index >= 0 && t_owning_executor == this) {
        // Called from a worker thread — push to own local queue.
        m_local_queues[t_worker_index].push(move(task));
    } else if (affinity >= 0) {
        // Waker carries an affinity hint — push to that worker's local queue.
        // No lock needed: WorkStealingDeque::push is thread-safe.
        m_local_queues[affinity].push(move(task));
    } else {
        // No affinity — fall back to the shared injection queue.
        lock_guard lock(m_mutex);
        m_injection_queue.push_back(move(task));
    }
    notify_if_needed();
}

void WorkStealingExecutor::notify_if_needed() {
    // If at least one worker is already searching, it will find the task.
    // seq_cst read-modify-write: see "The last searcher looks again".
    if (m_searching.fetch_add(0, memory_order_seq_cst) > 0) return;
    // Otherwise wake one parked worker to begin searching.
    uint64_t idle = m_idle_mask.load(memory_order_acquire);
    if (idle) {
        int idx = std::countr_zero(idle);
        unpark_worker(idx);  // driver.unpark() or condvar notify, by park state
    }
}
```

All three enqueue paths share `notify_if_needed()`. A searching worker will find a
locally-enqueued task without an explicit wake: on its steal sweep, or in the check the
last searcher makes before it parks.

Two checks come before these paths: an open [`WakeBatch`](#batched-wakes-wakebatch) for
this executor takes the task first, and on the local path a wake from a task poll goes
to the [LIFO slot](#lifo-slot) (see the flowchart there).

---

## Shutdown

1. Destructor sets `m_stop = true` inside `m_mutex` (still guards the injection queue).
2. Calls `unpark_worker()` on every worker: a worker in the driver gets
   `driver.unpark()`, one on its condvar is notified, and one not yet parked banks a
   token so its next park returns at once.
3. Joins all worker threads.

Workers exit their loop when `m_stop` is true and all queues are drained.

---

## Naming and Placement

`WorkSharingExecutor` is kept as-is (useful for debugging scheduler issues due to its
simpler concurrency). `WorkStealingExecutor` is added alongside it as a new class.

Files:

- `include/coro/runtime/work_stealing_executor.h`
- `src/runtime/work_stealing_executor.cpp`

`Runtime` selects the implementation: `num_threads <= 1` → `CurrentThreadExecutor`,
otherwise → `WorkStealingExecutor`.

---

## State Machine Invariants

The `SchedulingState` CAS machine is unchanged. The steal path is just another way to
dequeue a `Notified` task — the transition `Notified → Running` is always a CAS and
will fail if two workers race to claim the same task (which should not happen since
`steal()` is atomic, but the CAS is the ultimate guard).

---

## Tests

Unit tests are in `test/runtime/test_work_stealing_executor.cpp`. Key cases covered:

- Workers complete tasks when work is uneven (one spawning coroutine fans out N tasks).
- Tasks complete correctly with `N` workers and `M >> N` short-lived tasks.
- Stealing occurs: verify that with tasks pinned to worker 0's local queue, idle workers
  steal and execute them.
- A burst of spawns reaches every worker (`BurstOfSpawnsReachesEveryWorker`): eight
  tasks spawned at once on four workers each hold their worker until four have started.
  The spawns wake one worker only, so this passes only if a worker that finds work by
  stealing wakes the next one.
- A `WakeBatch` of `k` wakes on a pool with at least `k` parked workers: every task has
  started on a different worker before any of them finishes, with each task holding its
  worker until all `k` have started (as in `BurstOfSpawnsReachesEveryWorker`, but
  without relying on the chain).
- A batch of 200 spawns, more than it holds without allocating: every task runs.
- Nested batches: the outer one is open again after the inner one closes, and every
  task runs.
- A batch closed by an exception still enqueues its tasks.
- A batch opened on a non-worker thread delivers its wake.
- A batch on `CurrentThreadExecutor` or `WorkSharingExecutor` changes nothing.
- Shutdown drains all in-flight tasks.
- Skynet benchmark (disabled by default — run with `--gtest_also_run_disabled_tests`).

All existing tests (`test_sleep`, `test_join_handle`, `test_join_set`, `test_coro_scope`,
channel tests) also exercise the work-stealing executor via `Runtime`.

---

## Sequence Diagram: Stealing Path

```mermaid
sequenceDiagram
    participant W0 as Worker 0 (busy)
    participant W1 as Worker 1 (idle)
    participant LQ0 as LocalQueue[0]
    participant LQ1 as LocalQueue[1]

    W0->>LQ0: push(task A)
    W0->>LQ0: push(task B)
    W1->>LQ1: pop() → nullopt
    W1->>LQ0: steal() → task A
    W1->>W1: CAS Notified→Running (task A)
    W1->>W1: poll(task A)
    W0->>LQ0: pop() → task B
    W0->>W0: CAS Notified→Running (task B)
    W0->>W0: poll(task B)
```

---

## Future Work: Chase-Lev Lock-Free Deque

> **Deferred.** The mutex-backed `WorkStealingDeque` is sufficient for correctness.
> This section documents the Chase-Lev algorithm so the upgrade path is understood
> before it is needed. When performance profiling shows deque contention is a
> bottleneck, this section becomes the implementation spec.

### Why Bother?

Every `steal()` call on the current mutex-backed deque acquires a lock, even when the
victim's queue is empty. Under high thread counts with frequent stealing this creates
contention on the victim's mutex. Chase-Lev eliminates all locks on the common paths:

| Operation | Mutex version | Chase-Lev |
|-----------|--------------|-----------|
| `push`    | lock + write  | write + atomic store |
| `pop`     | lock + read   | read + optional CAS (only on last element) |
| `steal`   | lock + read   | CAS on `top` |

### The Algorithm

Chase-Lev is a double-ended queue (deque) where:

- The **owner** thread pushes and pops from the **bottom** (back). No contention from
  thieves on this end in the common case.
- **Thief** threads steal from the **top** (front). Multiple thieves can race here, resolved
  by a CAS.

Three fields drive the algorithm:

```
top    — atomic<int64_t>, incremented by thieves when they take an item
bottom — int64_t, written only by the owner (load/store, no atomic needed from owner's view)
buf    — pointer to a circular array of capacity 2^k
```

The number of items in the deque at any moment is `bottom - top`. The deque is empty
when `bottom == top` and full when `bottom - top == capacity`.

```
┌─────────────────────────────────────────────────────┐
│  circular buffer (capacity = 8, indices mod 8)       │
│                                                      │
│  index:  0    1    2    3    4    5    6    7         │
│        ┌────┬────┬────┬────┬────┬────┬────┬────┐    │
│        │    │ T  │ T  │ T  │    │    │    │    │    │
│        └────┴────┴────┴────┴────┴────┴────┴────┘    │
│                ▲                   ▲                 │
│               top=1             bottom=4             │
│          (thieves steal        (owner pushes/pops   │
│           from here)            from here)          │
│                                                      │
│  items in deque: bottom - top = 3  (indices 1,2,3)  │
└─────────────────────────────────────────────────────┘
```

### Owner: push

```
buf[bottom % capacity] = item
bottom++   (release store — makes item visible to thieves)
```

If the buffer is full (`bottom - top == capacity`), allocate a new buffer of double
the size, copy all items, and swap the pointer atomically before writing the new item.

### Owner: pop

```
bottom--
item = buf[bottom % capacity]   (acquire load)
if bottom < top:
    // deque is now empty; restore bottom and give up
    bottom = top
    return nullopt
if bottom == top:
    // racing with a thief for the last item
    if CAS(top, top, top+1) succeeds:
        bottom = top+1   // thief won, deque truly empty now
        return nullopt
    // owner won the CAS; item is ours
return item
```

The CAS on pop only fires when there is exactly one item remaining, which is the rare
case. All other pops are pure reads and writes with no contention.

### Thief: steal

```
t = top.load(acquire)
b = bottom.load(acquire)
if t >= b: return nullopt   // empty
item = buf[t % capacity]   (acquire load — must read before CAS commits)
if CAS(top, t, t+1):
    return item
return nullopt   // lost the race; try another victim
```

Multiple thieves race via `CAS(top, t, t+1)`. Only one wins per item. The loser
simply moves on — it does not retry the same victim.

### Sequence Diagram: Concurrent steal race

```mermaid
sequenceDiagram
    participant W0 as Owner (Worker 0)
    participant W1 as Thief A (Worker 1)
    participant W2 as Thief B (Worker 2)

    Note over W0,W2: Deque has 1 item (top=3, bottom=4)
    W1->>W1: t=3, b=4, read buf[3]=task
    W2->>W2: t=3, b=4, read buf[3]=task
    W1->>W1: CAS(top, 3, 4) ✓ wins
    W2->>W2: CAS(top, 4, 5) ✗ fails (top already 4)
    W2->>W2: return nullopt — try next victim
    W1->>W1: return task
```

### Memory Ordering Summary

| Access | Ordering | Reason |
|--------|----------|--------|
| `bottom` store after push | `release` | Makes the new item visible to thieves reading `bottom` |
| `bottom` load in thief | `acquire` | Pairs with owner's release store |
| `top` CAS in thief | `acq_rel` | Synchronises all thieves with each other |
| buffer element read in thief | `acquire` | Must happen before CAS commits the steal |

### What Changes in the Codebase

Only `include/coro/detail/work_stealing_deque.h` changes. The three atomic fields
replace `m_mutex` and `m_deque`. All call sites (`push`, `pop`, `steal_half`) keep the
same signatures. The executor code, wakers, and task state machine are unaffected.

One new concern: the growable buffer requires careful memory reclamation. When the
owner resizes, the old buffer cannot be freed immediately because a thief may have
loaded a pointer to it before the resize. The standard solution is epoch-based
reclamation or hazard pointers. A simpler alternative that avoids this entirely is to
use a **fixed-capacity** deque (bounded at construction time) and abort/assert if the
owner exceeds capacity. For task queues with a reasonable bound this is acceptable and
eliminates the reclamation problem.

---

## Open Questions

**Chase-Lev lock-free deque:** the `WorkStealingDeque` is currently mutex-backed.
Chase-Lev is documented in the [Future Work](#future-work-chase-lev-lock-free-deque)
section as a follow-on once profiling shows deque contention is a real bottleneck.

