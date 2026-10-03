// Tests for CurrentThreadExecutor: the ready queue, and parking through a Parker.
// Executor-level tests use a TestParker (condvar-backed, records every park() call);
// the Runtime-level tests at the end use the real IoDriverParker.
// See doc/design/executor_design.md, "CurrentThreadExecutor".

#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <coro/coro.h>
#include <coro/runtime/current_thread_executor.h>
#include <coro/runtime/io_driver.h>
#include <coro/runtime/runtime.h>
#include <coro/sync/sleep.h>
#include <coro/detail/task.h>
#include <coro/detail/task_state.h>
#include "io_test_util.h"

#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
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

// An executor plus a non-owning handle on its TestParker.
struct Harness {
    TestParker*                            parker;
    std::unique_ptr<CurrentThreadExecutor> ex;
};

Harness make_executor() {
    auto parker = std::make_unique<TestParker>();
    TestParker* raw = parker.get();
    return {raw, std::make_unique<CurrentThreadExecutor>(std::move(parker))};
}

// Schedules `future` on `ex`; returns the typed state for inspecting the result.
template<typename F>
std::shared_ptr<TaskState<typename F::OutputType>> schedule(CurrentThreadExecutor& ex, F future) {
    auto impl = std::make_shared<TaskImpl<F>>(std::move(future));
    std::shared_ptr<TaskState<typename F::OutputType>> state = impl;
    ex.schedule(std::shared_ptr<TaskBase>(std::move(impl)));
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
    Rc<TimerSlot>             m_slot;

    PollResult<int> poll(Context& ctx) {
        if (!m_armed) {
            m_armed = true;
            m_slot = make_rc<TimerSlot>();
            m_slot->waker = ctx.get_weak_waker();
            m_ex->add_timer(Clock::now() + m_delay, m_slot);
            return PollPending;
        }
        return 1;
    }
};

} // namespace

// --- Ready queue (ported from SingleThreadedExecutorTest) ---

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

TEST(CurrentThreadExecutorTest, SelfWakingTaskCompletesInTwoPasses) {
    auto h = make_executor();
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
    auto impl = std::make_shared<TaskImpl<ImmediateFuture>>(ImmediateFuture{1});
    std::shared_ptr<TaskState<int>> state = impl;
    state->waker = join_waker;
    h.ex->schedule(std::shared_ptr<TaskBase>(std::move(impl)));
    h.ex->poll_ready_tasks();
}

TEST(CurrentThreadExecutorTest, ScheduleSetsNotified) {
    auto h = make_executor();
    auto impl = std::make_shared<TaskImpl<NeverReadyFuture>>(NeverReadyFuture{});
    TaskBase* raw = impl.get();
    h.ex->schedule(std::shared_ptr<TaskBase>(impl));
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
    auto h = make_executor();
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
