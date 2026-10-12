// Tests for CurrentThreadExecutor: the ready queue, and parking through a Parker.
// Executor-level tests use a TestParker (records every park() call); the Runtime-level
// tests at the end use the real IoDriverParker.
// See doc/design/executor_design.md, "CurrentThreadExecutor".
//
// Also built for Pico (stub suite and board). The tests under #ifndef CORO_PICO need
// a second thread or the desktop Runtime; everything else is single-threaded, and
// makes its "remote" wakes with wake_remotely() below.

#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <coro/coro.h>
#include <coro/runtime/current_thread_executor.h>
#include <coro/runtime/runtime.h>
#include <coro/sync/sleep.h>
#include <coro/detail/task.h>
#include <coro/detail/task_state.h>

#ifndef CORO_PICO
#include <coro/runtime/io_driver.h>
#include "io_test_util.h"

#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <condition_variable>
#include <future>
#include <thread>
#endif

#include <chrono>
#include <functional>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace coro;
using namespace coro::detail;
using namespace std::chrono_literals;

namespace {

class MockWaker : public Waker {
public:
    MOCK_METHOD(void, wake, (), (override));
    MOCK_METHOD(Rc<Waker>, clone, (), (override));
};

#ifdef CORO_PICO

// Records each call. There is no second thread to unpark it, so it never blocks for
// longer than `max_wait`, which it waits out on the clock.
class TestParker final : public Parker {
public:
    void park(std::optional<std::chrono::nanoseconds> max_wait) override {
        m_waits.push_back(max_wait);
        if (auto on_park = std::exchange(m_on_park, {})) {
            // Like the driver dispatching an event: fire wakers, then return
            // without blocking.
            on_park();
            return;
        }
        if (std::exchange(m_unparked, false)) return;
        if (!max_wait) {
            // Nothing could ever end this park. Thrown through the executor's loop
            // to the test body, so that the test fails instead of hanging the board.
            throw std::logic_error("TestParker: park() with no limit and nothing to unpark it");
        }
        const auto deadline = std::chrono::steady_clock::now() + *max_wait;
        while (std::chrono::steady_clock::now() < deadline) {}
    }

    void unpark() noexcept override {
        ++m_unpark_calls;
        m_unparked = true;
    }

    /// Runs `fn` inside the next park() call.
    void on_next_park(std::function<void()> fn) { m_on_park = std::move(fn); }

    std::vector<std::optional<std::chrono::nanoseconds>> waits() const { return m_waits; }

    int unpark_calls() const { return m_unpark_calls; }

private:
    bool                  m_unparked = false;
    int                   m_unpark_calls = 0;
    std::function<void()> m_on_park;
    std::vector<std::optional<std::chrono::nanoseconds>> m_waits;
};

#else  // --- desktop: other threads unpark it ---

// Blocks like a real parker — until unpark() or max_wait — and records each call.
class TestParker final : public Parker {
public:
    void park(std::optional<std::chrono::nanoseconds> max_wait) override {
        std::function<void()> on_park;
        {
            std::lock_guard lock(m_mutex);
            m_waits.push_back(max_wait);
            m_in_park = true;
            on_park = std::exchange(m_on_park, {});
            m_cv.notify_all();
        }
        if (on_park) {
            // Like IoDriver::turn() dispatching an event: fire wakers on this
            // thread, then return without blocking.
            on_park();
            std::lock_guard lock(m_mutex);
            m_in_park = false;
            return;
        }
        std::unique_lock lock(m_mutex);
        auto unparked = [this] { return m_unparked; };
        if (!max_wait)
            m_cv.wait(lock, unparked);
        else
            m_cv.wait_for(lock, *max_wait, unparked);
        m_unparked = false;
        m_in_park  = false;
    }

    void unpark() noexcept override {
        std::lock_guard lock(m_mutex);
        ++m_unpark_calls;
        m_unparked = true;
        m_cv.notify_all();
    }

    /// Runs `fn` inside the next park() call, on the executor thread.
    void on_next_park(std::function<void()> fn) {
        std::lock_guard lock(m_mutex);
        m_on_park = std::move(fn);
    }

    /// Blocks until the executor is inside park().
    void wait_until_parked() {
        std::unique_lock lock(m_mutex);
        m_cv.wait(lock, [this] { return m_in_park; });
    }

    std::vector<std::optional<std::chrono::nanoseconds>> waits() const {
        std::lock_guard lock(m_mutex);
        return m_waits;
    }

    int unpark_calls() const {
        std::lock_guard lock(m_mutex);
        return m_unpark_calls;
    }

private:
    mutable std::mutex      m_mutex;
    std::condition_variable m_cv;
    bool                    m_unparked = false;
    bool                    m_in_park  = false;
    int                     m_unpark_calls = 0;
    std::function<void()>   m_on_park;
    std::vector<std::optional<std::chrono::nanoseconds>> m_waits;
};

#endif  // CORO_PICO

// An executor plus a non-owning handle on its TestParker.
struct Harness {
    TestParker*                            parker;
    std::unique_ptr<CurrentThreadExecutor> ex;
};

Harness make_executor(
        unsigned event_interval        = CurrentThreadExecutor::kDefaultEventInterval,
        unsigned global_queue_interval = CurrentThreadExecutor::kDefaultGlobalQueueInterval) {
    auto parker = std::make_unique<TestParker>();
    TestParker* raw = parker.get();
    return {raw, std::make_unique<CurrentThreadExecutor>(
                     std::move(parker), event_interval, global_queue_interval)};
}

// Builds a task the way the library's own call sites do.
template<typename F>
Rc<TaskImpl<F>> make_task(F future) {
    auto impl = make_rc<TaskImpl<F>>(std::move(future));
#ifdef CORO_PICO
    // Not automatic on Pico (see TaskBase::set_self()). Without it the task's
    // waker clones to null.
    impl->set_self(Weak<TaskBase>(impl));
#endif
    return impl;
}

// Schedules `future` on `ex`; returns the typed state for inspecting the result.
template<typename F>
Rc<TaskState<typename F::OutputType>> schedule(CurrentThreadExecutor& ex, F future) {
    auto impl = make_task(std::move(future));
    Rc<TaskState<typename F::OutputType>> state = impl;
    ex.schedule(Rc<TaskBase>(std::move(impl)));
    return state;
}

int result_of(TaskState<int>& state) {
    std::lock_guard lock(state.mutex);
    EXPECT_TRUE(state.result.has_value());
    return state.result.value_or(-1);
}

struct ImmediateFuture {
    using OutputType = int;
    int m_value;
    PollResult<int> poll(Context&) { return m_value; }
};

struct NeverReadyFuture {
    using OutputType = int;
    PollResult<int> poll(Context&) { return PollPending; }
};

// Returns Pending once, calls wake(), then returns Ready.
struct SelfWakingFuture {
    using OutputType = int;
    int  m_value;
    bool m_polled_once = false;
    PollResult<int> poll(Context& ctx) {
        if (!m_polled_once) {
            m_polled_once = true;
            ctx.getWaker()->wake();
            return PollPending;
        }
        return m_value;
    }
};

#ifndef CORO_PICO
// Suspends on the first poll, handing its waker to a std::promise for safe
// cross-thread transfer; returns the value on the second poll.
struct PromiseWakeFuture {
    using OutputType = int;
    int                               m_value;
    std::promise<Rc<detail::Waker>>* m_promise;
    bool                              m_first = true;

    PollResult<int> poll(Context& ctx) {
        if (m_first) {
            m_first = false;
            m_promise->set_value(ctx.getWaker()->clone());
            return PollPending;
        }
        return m_value;
    }
};
#endif  // CORO_PICO

// Suspends on the first poll, leaving its waker in *m_slot (same thread only).
struct SlotWakeFuture {
    using OutputType = int;
    int               m_value;
    Rc<detail::Waker>* m_slot;
    bool              m_first = true;

    PollResult<int> poll(Context& ctx) {
        if (m_first) {
            m_first = false;
            *m_slot = ctx.getWaker()->clone();
            return PollPending;
        }
        return m_value;
    }
};

// Arms an executor timer `m_delay` from now on the first poll; ready once woken.
struct TimerFuture {
    using OutputType = int;
    CurrentThreadExecutor*    m_ex;
    std::chrono::microseconds m_delay;
    bool                      m_armed = false;

    PollResult<int> poll(Context& ctx) {
        if (!m_armed) {
            m_armed = true;
            m_ex->add_timer(Clock::now() + m_delay, ctx.get_weak_waker());
            return PollPending;
        }
        return 1;
    }
};

// Wakes itself on every poll until *m_stop is set, counting its polls in *m_polls.
// `m_on_poll`, if set, runs at the start of each poll with the poll's number (from 1).
struct SpinFuture {
    using OutputType = int;
    bool*                    m_stop;
    int*                     m_polls;
    std::function<void(int)> m_on_poll = {};

    PollResult<int> poll(Context& ctx) {
        ++*m_polls;
        if (m_on_poll) m_on_poll(*m_polls);
        if (*m_stop) return *m_polls;
        ctx.getWaker()->wake();
        return PollPending;
    }
};

// Suspends on the first poll, leaving its waker in *m_slot; on the second poll
// appends its name to *m_log, runs `m_on_ready` if set, and completes.
struct NamedSlotFuture {
    using OutputType = int;
    std::string               m_name;
    Rc<detail::Waker>*        m_slot;
    std::vector<std::string>* m_log;
    std::function<void()>     m_on_ready = {};
    bool                      m_first = true;

    PollResult<int> poll(Context& ctx) {
        if (m_first) {
            m_first = false;
            *m_slot = ctx.getWaker()->clone();
            return PollPending;
        }
        m_log->push_back(m_name);
        if (m_on_ready) m_on_ready();
        return 0;
    }
};

// Appends its name to *m_log and completes.
struct NamedFuture {
    using OutputType = int;
    std::string               m_name;
    std::vector<std::string>* m_log;
    PollResult<int> poll(Context&) {
        m_log->push_back(m_name);
        return 0;
    }
};

// Runs `m_fn` on its only poll and completes.
struct CallFuture {
    using OutputType = int;
    std::function<void()> m_fn;
    PollResult<int> poll(Context&) {
        m_fn();
        return 0;
    }
};

// One half of a pair that hand the thread back and forth: each poll wakes the other
// task and waits to be woken by it, `m_left` times. Wakes itself while the other
// has not been polled yet or has already finished.
struct PingPongFuture {
    using OutputType = int;
    Rc<detail::Waker>* m_mine;
    Rc<detail::Waker>* m_peer;
    bool*              m_done;
    bool*              m_peer_done;
    int*               m_polls;
    int                m_left;

    PollResult<int> poll(Context& ctx) {
        ++*m_polls;
        *m_mine = ctx.getWaker()->clone();
        if (m_left == 0) {
            *m_done = true;
            if (*m_peer && !*m_peer_done) (*m_peer)->wake();
            return 0;
        }
        --m_left;
        if (*m_peer && !*m_peer_done) (*m_peer)->wake();
        else                          ctx.getWaker()->wake();
        return PollPending;
    }
};

// Makes a remote wake at a known point without a second thread: a task of another
// executor makes the wake, and that executor's loop is run here, on the calling
// thread. To the executor that owns the woken task this is a wake from outside its
// own loop, even when the caller is one of its tasks, so the task goes through the
// inject queue.
void wake_remotely(const Rc<detail::Waker>& waker) {
    auto other = make_executor();
    auto state = schedule(*other.ex, CallFuture{[&waker] { waker->wake(); }});
    other.ex->poll_ready_tasks();
}

} // namespace

// --- Ready queue ---

TEST(CurrentThreadExecutorTest, EmptyReturnsFalse) {
    auto h = make_executor();
    EXPECT_FALSE(h.ex->poll_ready_tasks());
    EXPECT_TRUE(h.ex->empty());
}

TEST(CurrentThreadExecutorTest, ScheduleAndPollTask) {
    auto h = make_executor();
    auto state = schedule(*h.ex, ImmediateFuture{7});
    EXPECT_FALSE(h.ex->empty());
    EXPECT_TRUE(h.ex->poll_ready_tasks());
    EXPECT_TRUE(h.ex->empty());
    EXPECT_EQ(result_of(*state), 7);
}

TEST(CurrentThreadExecutorTest, PendingTaskBecomesIdle) {
    auto h = make_executor();
    auto state = schedule(*h.ex, NeverReadyFuture{});
    h.ex->poll_ready_tasks();
    // Pending without storing a waker: the task goes Idle and leaves the ready queue.
    EXPECT_TRUE(h.ex->empty());
}

TEST(CurrentThreadExecutorTest, SelfWakingTaskCompletesInOneBatch) {
    // The wake fires during the poll, so the task goes back onto the local queue
    // and the same batch polls it again.
    auto h = make_executor();
    auto state = schedule(*h.ex, SelfWakingFuture{42});

    EXPECT_TRUE(h.ex->poll_ready_tasks());
    EXPECT_EQ(result_of(*state), 42);
    EXPECT_TRUE(h.ex->empty());
}

TEST(CurrentThreadExecutorTest, EventIntervalBoundsTheBatch) {
    auto h = make_executor(/*event_interval=*/1);
    auto state = schedule(*h.ex, SelfWakingFuture{42});

    h.ex->poll_ready_tasks();  // Pending; waker fires synchronously → re-enqueued
    {
        std::lock_guard lock(state->mutex);
        EXPECT_FALSE(state->result.has_value());
    }
    EXPECT_FALSE(h.ex->empty());

    h.ex->poll_ready_tasks();  // Ready(42)
    EXPECT_EQ(result_of(*state), 42);
}

TEST(CurrentThreadExecutorTest, SetResultCallsJoinWaker) {
    auto join_waker = make_rc<MockWaker>();
    EXPECT_CALL(*join_waker, wake()).Times(1);

    auto h = make_executor();
    auto impl = make_task(ImmediateFuture{1});
    Rc<TaskState<int>> state = impl;
    state->waker = join_waker;
    h.ex->schedule(Rc<TaskBase>(std::move(impl)));
    h.ex->poll_ready_tasks();
}

TEST(CurrentThreadExecutorTest, ScheduleSetsNotified) {
    auto h = make_executor();
    auto impl = make_task(NeverReadyFuture{});
    TaskBase* raw = impl.get();
    h.ex->schedule(Rc<TaskBase>(impl));
    EXPECT_EQ(raw->scheduling_state.load(), SchedulingState::Notified);
}

// --- wait_for_completion() and parking ---

TEST(CurrentThreadExecutorTest, FinishedRootTaskDoesNotPark) {
    // The root task completes in the first poll; the loop must exit without
    // parking (an unlimited park here would never return).
    auto h = make_executor();
    auto state = schedule(*h.ex, ImmediateFuture{3});
    h.ex->wait_for_completion(*state);
    EXPECT_EQ(result_of(*state), 3);
    EXPECT_TRUE(h.parker->waits().empty());
}

TEST(CurrentThreadExecutorTest, ReadyTasksParkWithZeroWait) {
    // One poll per batch, so the batch ends with the task still queued.
    auto h = make_executor(/*event_interval=*/1);
    auto state = schedule(*h.ex, SelfWakingFuture{5});
    h.ex->wait_for_completion(*state);
    EXPECT_EQ(result_of(*state), 5);

    const auto waits = h.parker->waits();
    ASSERT_EQ(waits.size(), 1u);
    ASSERT_TRUE(waits[0].has_value());
    EXPECT_EQ(waits[0]->count(), 0);
}

TEST(CurrentThreadExecutorTest, LocalWakeDoesNotUnpark) {
    // A wake on the executor thread while it isn't parked needs no unpark.
    auto h = make_executor();
    auto state = schedule(*h.ex, SelfWakingFuture{5});
    h.ex->wait_for_completion(*state);
    EXPECT_EQ(h.parker->unpark_calls(), 0);
}

TEST(CurrentThreadExecutorTest, WakeFromInsideParkDoesNotUnpark) {
    // Simulates an I/O event dispatched by the driver inside park(): the waker
    // fires on the executor thread while the executor is marked parked. That is
    // a local enqueue; unparking would only make the next park() return early.
    auto h = make_executor();
    Rc<detail::Waker> slot;
    auto state = schedule(*h.ex, SlotWakeFuture{8, &slot});
    h.parker->on_next_park([&slot] { slot->wake(); });

    h.ex->wait_for_completion(*state);
    EXPECT_EQ(result_of(*state), 8);
    EXPECT_EQ(h.parker->unpark_calls(), 0);
}

TEST(CurrentThreadExecutorTest, RemoteWakeFromInsideParkUnparks) {
    // The single-threaded form of the tests below: with nothing queued and no timer
    // the executor parks with no limit, and a remote wake made while it is marked
    // parked must unpark it.
    auto h = make_executor();
    Rc<detail::Waker> slot;
    auto state = schedule(*h.ex, SlotWakeFuture{8, &slot});
    h.parker->on_next_park([&slot] { wake_remotely(slot); });

    h.ex->wait_for_completion(*state);
    EXPECT_EQ(result_of(*state), 8);
    const auto waits = h.parker->waits();
    ASSERT_FALSE(waits.empty());
    EXPECT_FALSE(waits[0].has_value());
    EXPECT_EQ(h.parker->unpark_calls(), 1);
}

#ifndef CORO_PICO  // a second thread wakes the parked executor

TEST(CurrentThreadExecutorTest, IdleWithoutTimersParksWithoutLimit) {
    auto h = make_executor();
    std::promise<Rc<detail::Waker>> waker_promise;
    auto waker_future = waker_promise.get_future();
    auto state = schedule(*h.ex, PromiseWakeFuture{1, &waker_promise});

    std::thread waker_thread([parker = h.parker, wf = std::move(waker_future)]() mutable {
        auto waker = wf.get();
        parker->wait_until_parked();
        waker->wake();
    });
    h.ex->wait_for_completion(*state);
    waker_thread.join();

    const auto waits = h.parker->waits();
    ASSERT_FALSE(waits.empty());
    EXPECT_FALSE(waits[0].has_value());
}

TEST(CurrentThreadExecutorTest, RemoteWakeWhileParkedUnparks) {
    auto h = make_executor();
    std::promise<Rc<detail::Waker>> waker_promise;
    auto waker_future = waker_promise.get_future();
    auto state = schedule(*h.ex, PromiseWakeFuture{99, &waker_promise});

    // Wakes only once the executor is inside park(), so the unpark is required:
    // without it the TestParker blocks forever (it was given no time limit).
    std::thread waker_thread([parker = h.parker, wf = std::move(waker_future)]() mutable {
        auto waker = wf.get();
        parker->wait_until_parked();
        waker->wake();
    });
    h.ex->wait_for_completion(*state);
    waker_thread.join();

    EXPECT_EQ(result_of(*state), 99);
    EXPECT_GE(h.parker->unpark_calls(), 1);
}

TEST(CurrentThreadExecutorTest, ExternalThreadWakeup) {
    // A wake from another thread at an arbitrary time — before, during or after the
    // executor parks — must not be lost, and wait_for_completion() must not return
    // before the task completes.
    auto h = make_executor();
    std::promise<Rc<detail::Waker>> waker_promise;
    auto waker_future = waker_promise.get_future();
    auto state = schedule(*h.ex, PromiseWakeFuture{7, &waker_promise});

    std::thread waker_thread([wf = std::move(waker_future)]() mutable {
        wf.get()->wake();
    });
    h.ex->wait_for_completion(*state);
    waker_thread.join();

    EXPECT_EQ(result_of(*state), 7);
}

TEST(CurrentThreadExecutorTest, StaleUnparkOnlyCostsAnIteration) {
    // An unpark with nothing behind it (as left by the benign enqueue race) makes
    // one park() return early; the executor must then park again and still
    // complete normally.
    auto h = make_executor();
    h.parker->unpark();
    std::promise<Rc<detail::Waker>> waker_promise;
    auto waker_future = waker_promise.get_future();
    auto state = schedule(*h.ex, PromiseWakeFuture{4, &waker_promise});

    std::thread waker_thread([wf = std::move(waker_future)]() mutable {
        auto waker = wf.get();
        std::this_thread::sleep_for(20ms);
        waker->wake();
    });
    h.ex->wait_for_completion(*state);
    waker_thread.join();

    EXPECT_EQ(result_of(*state), 4);
    EXPECT_GE(h.parker->waits().size(), 2u);
}

#endif  // CORO_PICO

TEST(CurrentThreadExecutorTest, NextTimerBoundsParkWait) {
    auto h = make_executor();
    auto state = schedule(*h.ex, TimerFuture{h.ex.get(), 20ms});

    const auto start = std::chrono::steady_clock::now();
    h.ex->wait_for_completion(*state);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_EQ(result_of(*state), 1);
    EXPECT_GE(elapsed, 20ms);

    const auto waits = h.parker->waits();
    ASSERT_FALSE(waits.empty());
    ASSERT_TRUE(waits[0].has_value());
    EXPECT_GT(waits[0]->count(), 0);
    EXPECT_LE(*waits[0], 20ms);
    EXPECT_EQ(h.parker->unpark_calls(), 0);
}

// --- Batches, the local queue and the inject queue ---

TEST(CurrentThreadExecutorTest, LocalWakesDoNotParkBetweenPolls) {
    constexpr unsigned kInterval = 10;
    constexpr int      kRounds   = 100;
    auto h = make_executor(kInterval);
    Rc<detail::Waker> waker_a, waker_b;
    bool done_a = false, done_b = false;
    int  polls = 0;
    auto a = schedule(*h.ex, PingPongFuture{&waker_a, &waker_b, &done_a, &done_b, &polls, kRounds});
    auto b = schedule(*h.ex, PingPongFuture{&waker_b, &waker_a, &done_b, &done_a, &polls, kRounds});

    h.ex->wait_for_completion(*a);
    h.ex->wait_for_completion(*b);

    EXPECT_GE(polls, 2 * kRounds);
    // One park per batch of kInterval polls, not one per poll.
    const auto waits = h.parker->waits();
    EXPECT_LE(waits.size(), static_cast<std::size_t>(polls) / kInterval + 1);
    for (const auto& wait : waits) {
        ASSERT_TRUE(wait.has_value());
        EXPECT_EQ(wait->count(), 0);
    }
    EXPECT_EQ(h.parker->unpark_calls(), 0);
}

TEST(CurrentThreadExecutorTest, DriverTurnedEveryEventInterval) {
    // A task that would wake itself for ever still lets the loop reach park().
    constexpr unsigned kInterval = 7;
    auto h = make_executor(kInterval);
    bool stop = false;
    int  polls = 0;
    int  polls_at_park = -1;
    auto state = schedule(*h.ex, SpinFuture{&stop, &polls});
    h.parker->on_next_park([&] {
        polls_at_park = polls;
        stop = true;
    });

    h.ex->wait_for_completion(*state);
    EXPECT_EQ(polls_at_park, static_cast<int>(kInterval));
}

TEST(CurrentThreadExecutorTest, YieldEndsTheBatch) {
    struct YieldOnceFuture {
        using OutputType = int;
        std::vector<std::string>* m_log;
        bool                      m_yielded = false;
        PollResult<int> poll(Context& ctx) {
            m_log->push_back("yielder");
            if (m_yielded) return 0;
            m_yielded = true;
            ctx.yield_to_executor();
            return PollPending;
        }
    };

    auto h = make_executor();
    std::vector<std::string> log;
    auto yielder = schedule(*h.ex, YieldOnceFuture{&log});
    auto other   = schedule(*h.ex, NamedFuture{"other", &log});
    h.parker->on_next_park([&log] { log.push_back("park"); });

    h.ex->wait_for_completion(*yielder);
    // The other task was queued all along, but the park comes first.
    EXPECT_EQ(log, (std::vector<std::string>{"yielder", "park", "other", "yielder"}));
    const auto waits = h.parker->waits();
    ASSERT_FALSE(waits.empty());
    ASSERT_TRUE(waits[0].has_value());
    EXPECT_EQ(waits[0]->count(), 0);
}

namespace {
// A spinning task keeps the local queue non-empty; on its `kWakeAt`-th poll a remote
// wake is made for a task that was idle. Returns how many polls the spinner had made
// when that task ran.
struct BusyLoopResult {
    int woken_task_ran_at = -1;
    int unpark_calls      = 0;
};
constexpr int kWakeAt = 10;

BusyLoopResult remote_wake_into_busy_loop(unsigned global_queue_interval) {
    // An event interval far larger than the test, so that only the periodic look at
    // the inject queue can let the woken task in.
    auto h = make_executor(/*event_interval=*/100000, global_queue_interval);
    BusyLoopResult result;
    Rc<detail::Waker> slot;
    std::vector<std::string> log;
    bool stop  = false;
    int  polls = 0;

    auto woken = schedule(*h.ex, NamedSlotFuture{"woken", &slot, &log, [&] {
        result.woken_task_ran_at = polls;
        stop = true;
    }});
    auto spinner = schedule(*h.ex, SpinFuture{&stop, &polls, [&slot](int poll) {
        if (poll == kWakeAt) wake_remotely(slot);
    }});

    h.ex->wait_for_completion(*spinner);
    result.unpark_calls = h.parker->unpark_calls();
    return result;
}
} // namespace

TEST(CurrentThreadExecutorTest, InjectQueueServedWhileLocalQueueBusy) {
    constexpr unsigned kGlobalInterval = 5;
    const auto result = remote_wake_into_busy_loop(kGlobalInterval);
    ASSERT_GE(result.woken_task_ran_at, kWakeAt);
    // Within one interval of the wake; the woken task then waits behind the spinner
    // once more.
    EXPECT_LE(result.woken_task_ran_at, kWakeAt + static_cast<int>(kGlobalInterval) + 1);
}

TEST(CurrentThreadExecutorTest, RemoteWakeWhileLocalQueueBusyDoesNotUnpark) {
    // The loop is running, not parked: it finds the task on its own.
    const auto result = remote_wake_into_busy_loop(/*global_queue_interval=*/5);
    ASSERT_GE(result.woken_task_ran_at, kWakeAt);
    EXPECT_EQ(result.unpark_calls, 0);
}

TEST(CurrentThreadExecutorTest, InjectedTasksKeepTheirOrder) {
    auto h = make_executor();
    std::vector<std::string> log;
    Rc<detail::Waker> slot_a, slot_b, slot_c;
    auto a = schedule(*h.ex, NamedSlotFuture{"a", &slot_a, &log});
    auto b = schedule(*h.ex, NamedSlotFuture{"b", &slot_b, &log});
    auto c = schedule(*h.ex, NamedSlotFuture{"c", &slot_c, &log});
    h.ex->poll_ready_tasks();   // all three are idle, their wakers in the slots
    ASSERT_TRUE(h.ex->empty());

    // From outside the loop, so all three are remote wakes.
    slot_c->wake();
    slot_a->wake();
    slot_b->wake();

    h.ex->poll_ready_tasks();
    EXPECT_EQ(log, (std::vector<std::string>{"c", "a", "b"}));
}

TEST(CurrentThreadExecutorTest, WakeFromOtherExecutorLoopOnSameThreadIsRemote) {
    // While a task of executor A is being polled, executor B's loop runs on the same
    // thread and wakes A's task "remote". That wake is not a local one for A: it
    // must go through A's inject queue. A's task "local", woken afterwards by A's
    // own running task, goes onto A's local queue and so runs first. That also
    // shows A is the running executor again once B's loop has returned.
    auto a = make_executor();
    auto b = make_executor();
    std::vector<std::string> log;
    Rc<detail::Waker> remote_slot, local_slot;
    auto remote = schedule(*a.ex, NamedSlotFuture{"remote", &remote_slot, &log});
    auto local  = schedule(*a.ex, NamedSlotFuture{"local", &local_slot, &log});
    a.ex->poll_ready_tasks();   // both idle
    ASSERT_TRUE(a.ex->empty());

    auto in_b = schedule(*b.ex, CallFuture{[&remote_slot] { remote_slot->wake(); }});
    auto in_a = schedule(*a.ex, CallFuture{[&] {
        b.ex->poll_ready_tasks();
        local_slot->wake();
    }});

    a.ex->poll_ready_tasks();
    EXPECT_EQ(log, (std::vector<std::string>{"local", "remote"}));
    EXPECT_EQ(a.parker->unpark_calls(), 0);
}

TEST(CurrentThreadExecutorTest, SpawnFromTaskUsesLocalQueue) {
    // A running task has "woken" woken remotely (inject queue) and then spawns
    // "spawned". The spawn is local, so it runs first.
    auto h = make_executor();
    std::vector<std::string> log;
    Rc<detail::Waker> slot;
    auto woken = schedule(*h.ex, NamedSlotFuture{"woken", &slot, &log});
    h.ex->poll_ready_tasks();   // idle
    ASSERT_TRUE(h.ex->empty());

    CurrentThreadExecutor& ex = *h.ex;
    auto spawner = schedule(ex, CallFuture{[&] {
        wake_remotely(slot);
        schedule(ex, NamedFuture{"spawned", &log});
    }});

    h.ex->poll_ready_tasks();
    EXPECT_EQ(log, (std::vector<std::string>{"spawned", "woken"}));
}

#ifndef CORO_PICO  // threads, and the desktop Runtime

TEST(CurrentThreadExecutorTest, SecondThreadInLoopIsRefused) {
    // The local queue has no lock, so a second thread must not run the loop while
    // the first is in it. The same thread may nest it.
    auto h = make_executor();
    CurrentThreadExecutor& ex = *h.ex;
    bool other_thread_threw  = false;
    bool same_thread_threw   = false;
    auto state = schedule(ex, CallFuture{[&] {
        std::thread([&] {
            try {
                ex.poll_ready_tasks();
            } catch (const std::logic_error&) {
                other_thread_threw = true;
            }
        }).join();
        try {
            ex.poll_ready_tasks();
        } catch (const std::logic_error&) {
            same_thread_threw = true;
        }
    }});

    h.ex->poll_ready_tasks();
    EXPECT_TRUE(other_thread_threw);
    EXPECT_FALSE(same_thread_threw);

    // Once the first thread has left the loop, another may run it.
    auto later = schedule(ex, ImmediateFuture{6});
    std::thread([&ex] { EXPECT_TRUE(ex.poll_ready_tasks()); }).join();
    EXPECT_EQ(result_of(*later), 6);
}

// --- Runtime(1): CurrentThreadExecutor + IoDriverParker ---

using io_test::ReadByteFuture;

TEST(CurrentThreadRuntimeTest, IoEventWakesTaskParkedInDriver) {
    int fds[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds), 0);

    Runtime rt(std::size_t{1});
    std::thread writer([fd = fds[1]] {
        std::this_thread::sleep_for(50ms);
        const char c = 'q';
        EXPECT_EQ(::write(fd, &c, 1), 1);
    });
    const char got = rt.block_on(ReadByteFuture{fds[0], {}});
    writer.join();
    EXPECT_EQ(got, 'q');

    ::close(fds[0]);
    ::close(fds[1]);
}

TEST(CurrentThreadRuntimeTest, DriverTimerWakesBlockedTurn) {
    // On desktop, sleep_for() puts its timer in the IoDriver's queue: the executor's
    // turn() must bound its epoll wait by that deadline and fire it on return.
    Runtime rt(std::size_t{1});
    const auto start = std::chrono::steady_clock::now();
    rt.block_on(sleep_for(20ms));
    const auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_GE(elapsed, 20ms);
    EXPECT_LT(elapsed, 2s);
}

namespace {
Coro<void> fulfil(std::shared_ptr<std::promise<void>> ran) {
    ran->set_value();
    co_return;
}

bool spawn_and_wait() {
    std::this_thread::sleep_for(50ms);   // let the executor park first
    auto ran = std::make_shared<std::promise<void>>();
    auto done = ran->get_future();
    spawn(fulfil(ran)).detach();
    return done.wait_for(2s) == std::future_status::ready;
}

// spawn_blocking() needs the current runtime, which block_on() only sets once it
// starts, so it has to be called from inside the root task.
Coro<bool> await_blocking_spawn() {
    co_return co_await spawn_blocking(spawn_and_wait);
}
} // namespace

TEST(CurrentThreadRuntimeTest, SpawnFromBlockingThreadUnparksDriver) {
    // A common pattern: the root task awaits spawn_blocking(), so the executor
    // parks in the driver with no limit; the blocking thread then spawn()s a task.
    // schedule() must unpark the executor, or the task never runs. The blocking
    // thread waits with a bound, so a regression fails instead of hanging.
    Runtime rt(std::size_t{1});
    EXPECT_TRUE(rt.block_on(await_blocking_spawn()));
}

#endif  // CORO_PICO
