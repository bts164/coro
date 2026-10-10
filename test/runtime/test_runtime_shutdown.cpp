// Runtime shutdown — doc/design/runtime_shutdown.md.
//
// A failure here is a hang inside ~Runtime or memory misuse, not a wrong value, so
// every test runs under a watchdog and on each executor, and the suite is meant to be
// run under AddressSanitizer and ThreadSanitizer as well.

#include <coro/coro.h>
#include <coro/co_invoke.h>
#include <coro/coro_stream.h>
#include <coro/future.h>
#include <coro/io/udp_socket.h>
#include <coro/runtime/runtime.h>
#include <coro/runtime/work_sharing_executor.h>
#include <coro/stream.h>
#include <coro/sync/broadcast.h>
#include <coro/sync/event.h>
#include <coro/sync/mpsc.h>
#include <coro/sync/oneshot.h>
#include <coro/sync/sleep.h>
#include <coro/sync/watch.h>
#include <coro/task/spawn_blocking.h>
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

namespace {

// Aborts the test binary, naming the test, if the test outlives `limit`. Without it
// a regression in this area stalls the whole suite inside a destructor.
class Watchdog {
public:
    explicit Watchdog(std::chrono::seconds limit) {
        const auto* info = testing::UnitTest::GetInstance()->current_test_info();
        m_name = std::string(info->test_suite_name()) + "." + info->name();
        m_thread = std::thread([this, limit] {
            std::unique_lock lock(m_mutex);
            if (m_cv.wait_for(lock, limit, [this] { return m_done; })) return;
            std::fprintf(stderr, "watchdog: %s did not finish within %lld s\n",
                         m_name.c_str(), static_cast<long long>(limit.count()));
            std::fflush(stderr);
            std::abort();
        });
    }

    ~Watchdog() {
        {
            std::lock_guard lock(m_mutex);
            m_done = true;
        }
        m_cv.notify_one();
        m_thread.join();
    }

private:
    std::mutex              m_mutex;
    std::condition_variable m_cv;
    bool                    m_done = false;
    std::string             m_name;
    std::thread             m_thread;
};

// One Runtime per test, built on the heap so the test decides when it is destroyed.
struct SingleThread {
    static std::unique_ptr<coro::Runtime> make() {
        return std::make_unique<coro::Runtime>(std::size_t{1});
    }
};
struct WorkStealing {
    static std::unique_ptr<coro::Runtime> make() {
        return std::make_unique<coro::Runtime>(std::size_t{4});
    }
};
struct WorkSharing {
    static std::unique_ptr<coro::Runtime> make() {
        return std::make_unique<coro::Runtime>(
            std::in_place_type<coro::WorkSharingExecutor>, std::size_t{4});
    }
};

template<typename Traits>
class RuntimeShutdown : public testing::Test {
    // Generous: the suite also runs under ThreadSanitizer.
    Watchdog m_watchdog{60s};
};

using Runtimes = testing::Types<SingleThread, WorkStealing, WorkSharing>;
TYPED_TEST_SUITE(RuntimeShutdown, Runtimes);

// Shared between a test and the tasks and callables it starts. Owned through a
// shared_ptr by all of them: nothing here may borrow from the test's stack, since
// the point of these tests is that the work outlives block_on().
struct Probe {
    coro::Event        started;          // the task or callable is running
    std::atomic<bool>  unwound{false};   // a callable saw BlockingCancelled
    std::atomic<bool>  finished{false};  // ran past its wait, or a body ran at all
    std::atomic<bool>  cleaned{false};   // a coroutine frame's locals were destroyed
    std::atomic<bool>  flag{false};      // test-specific
    std::atomic<int>   count{0};         // test-specific
    // Written by one runtime thread, read by the test once the runtime is gone.
    std::weak_ptr<int> token;
};

std::shared_ptr<Probe> make_probe() { return std::make_shared<Probe>(); }

// A frame-local whose destructor records that the frame was destroyed.
struct Cleanup {
    std::shared_ptr<Probe> probe;
    ~Cleanup() { probe->cleaned.store(true); }
};

// A Cancellable future that never completes on its own. Its first poll sets
// `started`. Once cancelled it needs two more polls to drain: the first wakes its
// own waker and stays pending, the second reports PollDropped. `count == 2`
// therefore means it was drained by a running executor, not merely destroyed.
class DrainingFuture {
public:
    using OutputType = int;
    explicit DrainingFuture(std::shared_ptr<Probe> probe) : m_probe(std::move(probe)) {}

    coro::PollResult<int> poll(coro::detail::Context& ctx) {
        if (!m_cancelled) {
            m_probe->started.set();
            return coro::PollPending;
        }
        if (m_probe->count.fetch_add(1) == 0) {
            ctx.getWaker()->wake();
            return coro::PollPending;
        }
        return coro::PollDropped;
    }

    void cancel() {
        m_cancelled = true;
        m_probe->flag.store(true);
    }

private:
    std::shared_ptr<Probe> m_probe;
    bool                   m_cancelled = false;
};
static_assert(coro::Cancellable<DrainingFuture>);

bool drained(const Probe& probe) { return probe.flag.load() && probe.count.load() == 2; }

coro::Coro<void> await_draining_future(std::shared_ptr<Probe> probe) {
    (void)co_await DrainingFuture(std::move(probe));
}

// A stream that never yields and owns a child task.
coro::CoroStream<int> stream_with_child(std::shared_ptr<Probe> child_probe) {
    auto child = coro::spawn(await_draining_future(std::move(child_probe)));
    co_yield co_await coro::never<int>();
}

// A task that owns a child task and never finishes.
coro::Coro<void> parent_with_child(std::shared_ptr<Probe> probe,
                                   std::shared_ptr<Probe> child_probe) {
    Cleanup cleanup{probe};
    auto child = coro::spawn(await_draining_future(std::move(child_probe)));
    (void)co_await coro::never<int>();
}

// Must never run: spawned only once the runtime is shutting down.
coro::Coro<void> record_body_ran(std::shared_ptr<Probe> probe, std::shared_ptr<int> token) {
    (void)token;
    probe->finished.store(true);
    co_return;
}

coro::Coro<void> noop() { co_return; }

// Waits until every probe has reported `started`.
//
// Inside a coroutine, build the vector in a statement of its own rather than
// writing `co_await wait_started({a, b})`: GCC 13 crashes (internal compiler
// error) on a braced list of non-trivial objects inside a co_await expression.
// See doc/known_issues.md, KI.1.
coro::Coro<void> wait_started(std::vector<std::shared_ptr<Probe>> probes) {
    for (auto& probe : probes) co_await probe->started.wait();
}

// Parks the calling blocking thread until its task is cancelled.
void park_until_cancelled(Probe& probe) {
    probe.started.set();
    try {
        (void)coro::blocking_wait(coro::never<int>());
    } catch (const coro::BlockingCancelled&) {
        probe.unwound.store(true);
        throw;
    }
    probe.finished.store(true);
}

// Spins, shielded by the caller, until the calling blocking task has been asked to
// stop: that is, until shutdown has begun.
void wait_for_cancel_request() {
    while (!coro::detail::current_blocking_task()->is_cancelled())
        std::this_thread::sleep_for(1ms);
}

} // namespace

// ---------------------------------------------------------------------------
// Scenario A: parked in blocking_wait() on a leaf future
// ---------------------------------------------------------------------------

TYPED_TEST(RuntimeShutdown, ParkedCallableIsCancelled) {
    auto probe = make_probe();
    {
        auto rt = TypeParam::make();
        rt->block_on([](std::shared_ptr<Probe> probe) -> coro::Coro<void> {
            coro::spawn_blocking([probe] { park_until_cancelled(*probe); }).detach();
            co_await probe->started.wait();
        }(probe));
    }
    EXPECT_TRUE(probe->unwound.load());
    EXPECT_FALSE(probe->finished.load());
}

// ---------------------------------------------------------------------------
// Scenario B: waiting on something an executor task must finish
// ---------------------------------------------------------------------------

TYPED_TEST(RuntimeShutdown, CallableWaitingOnTaskDrainsIt) {
    auto probe = make_probe();
    auto task_probe = make_probe();
    {
        auto rt = TypeParam::make();
        rt->block_on([](std::shared_ptr<Probe> probe,
                        std::shared_ptr<Probe> task_probe) -> coro::Coro<void> {
            coro::spawn_blocking([probe, task_probe] {
                auto h = coro::spawn(await_draining_future(task_probe));
                try {
                    coro::blocking_wait(std::move(h));
                } catch (const coro::BlockingCancelled&) {
                    probe->unwound.store(true);
                    // The task had drained before the callable was let go.
                    probe->flag.store(drained(*task_probe));
                    throw;
                }
                probe->finished.store(true);
            }).detach();
            // Set by the task's first poll: the callable is waiting on a running task.
            co_await task_probe->started.wait();
        }(probe, task_probe));
    }
    EXPECT_TRUE(drained(*task_probe));
    EXPECT_TRUE(probe->flag.load());
    EXPECT_TRUE(probe->unwound.load());
    EXPECT_FALSE(probe->finished.load());
}

TYPED_TEST(RuntimeShutdown, CallableOwningScopeDrainsChildren) {
    auto probe = make_probe();
    auto child_probe = make_probe();
    {
        auto rt = TypeParam::make();
        rt->block_on([](std::shared_ptr<Probe> probe,
                        std::shared_ptr<Probe> child_probe) -> coro::Coro<void> {
            coro::spawn_blocking([probe, child_probe] {
                try {
                    coro::blocking_wait(coro::co_invoke([child_probe]() -> coro::Coro<void> {
                        auto child = coro::spawn(await_draining_future(child_probe));
                        (void)co_await coro::never<int>();
                    }));
                } catch (const coro::BlockingCancelled&) {
                    probe->unwound.store(true);
                    probe->flag.store(drained(*child_probe));
                    throw;
                }
                probe->finished.store(true);
            }).detach();
            co_await child_probe->started.wait();
        }(probe, child_probe));
    }
    EXPECT_TRUE(drained(*child_probe));
    EXPECT_TRUE(probe->flag.load());
    EXPECT_TRUE(probe->unwound.load());
    EXPECT_FALSE(probe->finished.load());
}

TYPED_TEST(RuntimeShutdown, CallableOwningStreamDrainsIt) {
    auto probe = make_probe();
    auto child_probe = make_probe();
    {
        auto rt = TypeParam::make();
        rt->block_on([](std::shared_ptr<Probe> probe,
                        std::shared_ptr<Probe> child_probe) -> coro::Coro<void> {
            coro::spawn_blocking([probe, child_probe] {
                auto stream = stream_with_child(child_probe);
                try {
                    (void)coro::blocking_next(stream);
                } catch (const coro::BlockingCancelled&) {
                    probe->unwound.store(true);
                    probe->flag.store(drained(*child_probe));
                    throw;
                }
                probe->finished.store(true);
            }).detach();
            co_await child_probe->started.wait();
        }(probe, child_probe));
    }
    EXPECT_TRUE(drained(*child_probe));
    EXPECT_TRUE(probe->flag.load());
    EXPECT_TRUE(probe->unwound.load());
    EXPECT_FALSE(probe->finished.load());
}

// ---------------------------------------------------------------------------
// Scenario C: dropping a handle while unwinding
// ---------------------------------------------------------------------------

TYPED_TEST(RuntimeShutdown, CallableDroppingHandleDuringUnwind) {
    auto probe = make_probe();
    auto task_probe = make_probe();
    {
        auto rt = TypeParam::make();
        rt->block_on([](std::shared_ptr<Probe> probe,
                        std::shared_ptr<Probe> task_probe) -> coro::Coro<void> {
            coro::spawn_blocking([probe, task_probe] {
                // Destroyed by the BlockingCancelled that leaves this callable.
                auto h = coro::spawn(await_draining_future(task_probe));
                park_until_cancelled(*probe);
            }).detach();
            std::vector<std::shared_ptr<Probe>> probes{probe, task_probe};
            co_await wait_started(std::move(probes));
        }(probe, task_probe));
    }
    EXPECT_TRUE(probe->unwound.load());
    EXPECT_TRUE(drained(*task_probe));
}

// ---------------------------------------------------------------------------
// Scenario D: spawn() from a callable while shutting down
// ---------------------------------------------------------------------------

TYPED_TEST(RuntimeShutdown, CallableSpawningDuringShutdown) {
    auto probe = make_probe();
    auto late = make_probe();
    {
        auto rt = TypeParam::make();
        rt->block_on([](std::shared_ptr<Probe> probe,
                        std::shared_ptr<Probe> late) -> coro::Coro<void> {
            coro::spawn_blocking([probe, late] {
                try {
                    park_until_cancelled(*probe);
                } catch (const coro::BlockingCancelled&) {
                    // Shielded, so the wait below reports the new task's outcome and
                    // not this callable's own cancellation.
                    coro::BlockingCancelShield shield;
                    auto token = std::make_shared<int>(0);
                    late->token = token;
                    auto h = coro::spawn(record_body_ran(late, std::move(token)));
                    try {
                        coro::blocking_wait(std::move(h));
                    } catch (const coro::BlockingCancelled&) {
                        late->flag.store(true);
                    }
                }
            }).detach();
            co_await probe->started.wait();
        }(probe, late));
    }
    EXPECT_TRUE(probe->unwound.load());
    EXPECT_FALSE(late->finished.load());   // born cancelled: the body never ran
    EXPECT_TRUE(late->token.expired());    // its arguments were destroyed
    EXPECT_TRUE(late->flag.load());        // and its handle reported it dropped
}

// ---------------------------------------------------------------------------
// Scenario E: a callable that finishes and wakes a task as shutdown starts
// ---------------------------------------------------------------------------

namespace {

coro::Coro<void> receive_until_closed(coro::MpscReceiver<int> rx, std::shared_ptr<Probe> probe) {
    Cleanup cleanup{probe};
    probe->started.set();
    while (auto item = co_await rx.recv()) probe->count.fetch_add(1);
}

} // namespace

TYPED_TEST(RuntimeShutdown, CallableFinishingWakesTask) {
    auto probe = make_probe();
    auto rx_probe = make_probe();
    {
        auto rt = TypeParam::make();
        rt->block_on([](std::shared_ptr<Probe> probe,
                        std::shared_ptr<Probe> rx_probe) -> coro::Coro<void> {
            auto [tx, rx] = coro::mpsc_channel<int>(4);
            coro::spawn(receive_until_closed(std::move(rx), rx_probe)).detach();
            auto sender = std::make_shared<coro::MpscSender<int>>(std::move(tx));
            coro::spawn_blocking([probe, sender] {
                coro::BlockingCancelShield shield;
                probe->started.set();
                wait_for_cancel_request();
                // Shutdown has begun: this send wakes a task that is being drained,
                // and the callable then completes normally.
                (void)coro::blocking_wait(sender->send(1));
                probe->finished.store(true);
            }).detach();
            std::vector<std::shared_ptr<Probe>> probes{probe, rx_probe};
            co_await wait_started(std::move(probes));
        }(probe, rx_probe));
    }
    EXPECT_TRUE(probe->finished.load());
    EXPECT_TRUE(rx_probe->cleaned.load());
}

// ---------------------------------------------------------------------------
// Scenario F: shielded cleanup that needs the I/O driver
// ---------------------------------------------------------------------------

TYPED_TEST(RuntimeShutdown, ShieldedCleanupUsesTimer) {
    auto probe = make_probe();
    {
        auto rt = TypeParam::make();
        rt->block_on([](std::shared_ptr<Probe> probe) -> coro::Coro<void> {
            coro::spawn_blocking([probe] {
                try {
                    park_until_cancelled(*probe);
                } catch (const coro::BlockingCancelled&) {
                    coro::BlockingCancelShield shield;
                    // Completes only if something still turns the I/O driver.
                    coro::blocking_wait(coro::sleep_for(20ms));
                    probe->flag.store(true);
                }
            }).detach();
            co_await probe->started.wait();
        }(probe));
    }
    EXPECT_TRUE(probe->unwound.load());
    EXPECT_TRUE(probe->flag.load());
}

// ---------------------------------------------------------------------------
// Scenario G: a draining task drops its BlockingHandle
// ---------------------------------------------------------------------------

namespace {

coro::Coro<void> hold_blocking_handle(std::shared_ptr<Probe> probe,
                                      std::shared_ptr<Probe> callable_probe) {
    Cleanup cleanup{probe};
    auto h = coro::spawn_blocking([callable_probe] { park_until_cancelled(*callable_probe); });
    (void)co_await coro::never<int>();
}

} // namespace

TYPED_TEST(RuntimeShutdown, TaskDroppingBlockingHandle) {
    auto probe = make_probe();
    auto callable_probe = make_probe();
    {
        auto rt = TypeParam::make();
        rt->block_on([](std::shared_ptr<Probe> probe,
                        std::shared_ptr<Probe> callable_probe) -> coro::Coro<void> {
            coro::spawn(hold_blocking_handle(probe, callable_probe)).detach();
            co_await callable_probe->started.wait();
        }(probe, callable_probe));
    }
    EXPECT_TRUE(probe->cleaned.load());
    EXPECT_TRUE(callable_probe->unwound.load());
    EXPECT_FALSE(callable_probe->finished.load());
}

// ---------------------------------------------------------------------------
// Scenario H: spawn_blocking() while shutting down
// ---------------------------------------------------------------------------

TYPED_TEST(RuntimeShutdown, SpawnBlockingDuringShutdown) {
    auto probe = make_probe();
    auto late = make_probe();
    {
        auto rt = TypeParam::make();
        rt->block_on([](std::shared_ptr<Probe> probe,
                        std::shared_ptr<Probe> late) -> coro::Coro<void> {
            coro::spawn_blocking([probe, late] {
                try {
                    park_until_cancelled(*probe);
                } catch (const coro::BlockingCancelled&) {
                    coro::BlockingCancelShield shield;
                    auto token = std::make_shared<int>(0);
                    late->token = token;
                    auto h = coro::spawn_blocking(
                        [late, token = std::move(token)] { late->finished.store(true); });
                    try {
                        coro::blocking_wait(std::move(h));
                    } catch (const coro::BlockingCancelled&) {
                        late->flag.store(true);
                    }
                }
            }).detach();
            co_await probe->started.wait();
        }(probe, late));
    }
    EXPECT_TRUE(probe->unwound.load());
    EXPECT_FALSE(late->finished.load());   // the callable was never called
    EXPECT_TRUE(late->token.expired());    // its captures were destroyed
    EXPECT_TRUE(late->flag.load());        // and its handle reported it dropped
}

// ---------------------------------------------------------------------------
// Scenario I: a shielded wait that only a task can end
// ---------------------------------------------------------------------------

namespace {

coro::Coro<void> hold_sender(coro::OneshotSender<int> tx, std::shared_ptr<Probe> probe) {
    Cleanup cleanup{probe};
    (void)tx;
    probe->started.set();
    (void)co_await coro::never<int>();
}

} // namespace

TYPED_TEST(RuntimeShutdown, CallableReleasedByChannelClose) {
    auto probe = make_probe();
    auto tx_probe = make_probe();
    {
        auto rt = TypeParam::make();
        rt->block_on([](std::shared_ptr<Probe> probe,
                        std::shared_ptr<Probe> tx_probe) -> coro::Coro<void> {
            auto [tx, rx] = coro::oneshot_channel<int>();
            coro::spawn(hold_sender(std::move(tx), tx_probe)).detach();
            auto receiver = std::make_shared<coro::OneshotReceiver<int>>(std::move(rx));
            coro::spawn_blocking([probe, receiver] {
                // Shielded: cancellation cannot end this wait. Only the sender going
                // away can, and that takes the task holding it being drained.
                coro::BlockingCancelShield shield;
                probe->started.set();
                auto result = coro::blocking_wait(receiver->recv());
                probe->flag.store(!result.has_value());
                probe->finished.store(true);
            }).detach();
            std::vector<std::shared_ptr<Probe>> probes{probe, tx_probe};
            co_await wait_started(std::move(probes));
        }(probe, tx_probe));
    }
    EXPECT_TRUE(tx_probe->cleaned.load());
    EXPECT_TRUE(probe->finished.load());
    EXPECT_TRUE(probe->flag.load());       // the receive failed: sender dropped
}

// ---------------------------------------------------------------------------
// Rules 2 to 4: detached tasks, spawning while draining, the single-thread drain
// ---------------------------------------------------------------------------

TYPED_TEST(RuntimeShutdown, DetachedTaskIsDrained) {
    auto probe = make_probe();
    auto child_probe = make_probe();
    {
        auto rt = TypeParam::make();
        rt->block_on([](std::shared_ptr<Probe> probe,
                        std::shared_ptr<Probe> child_probe) -> coro::Coro<void> {
            coro::spawn(parent_with_child(probe, child_probe)).detach();
            co_await child_probe->started.wait();
        }(probe, child_probe));
    }
    EXPECT_TRUE(probe->cleaned.load());
    EXPECT_TRUE(drained(*child_probe));
}

namespace {

// Spawns a task from its destructor, which here runs only when the frame that owns
// it is destroyed by shutdown.
struct SpawnOnDestroy {
    std::shared_ptr<Probe> late;
    ~SpawnOnDestroy() {
        auto token = std::make_shared<int>(0);
        late->token = token;
        coro::spawn(record_body_ran(late, std::move(token))).detach();
        late->flag.store(true);
    }
};

coro::Coro<void> spawn_when_destroyed(std::shared_ptr<Probe> probe, std::shared_ptr<Probe> late) {
    SpawnOnDestroy spawner{std::move(late)};
    probe->started.set();
    (void)co_await coro::never<int>();
}

} // namespace

TYPED_TEST(RuntimeShutdown, TaskSpawningDuringShutdown) {
    auto probe = make_probe();
    auto late = make_probe();
    {
        auto rt = TypeParam::make();
        rt->block_on([](std::shared_ptr<Probe> probe,
                        std::shared_ptr<Probe> late) -> coro::Coro<void> {
            coro::spawn(spawn_when_destroyed(probe, late)).detach();
            co_await probe->started.wait();
        }(probe, late));
    }
    EXPECT_TRUE(late->flag.load());        // the destructor ran and spawn() returned
    EXPECT_FALSE(late->finished.load());   // born cancelled: the body never ran
    EXPECT_TRUE(late->token.expired());    // and the task was drained, not leaked
}

TYPED_TEST(RuntimeShutdown, IdleTasksOnSingleThreadRuntime) {
    // On Runtime(1) nothing polls these once block_on() has returned, so the thread
    // destroying the runtime has to. Run on the other executors for symmetry.
    std::vector<std::shared_ptr<Probe>> probes;
    for (int i = 0; i < 8; ++i) probes.push_back(make_probe());
    {
        auto rt = TypeParam::make();
        rt->block_on([](std::vector<std::shared_ptr<Probe>> probes) -> coro::Coro<void> {
            for (auto& probe : probes) coro::spawn(await_draining_future(probe)).detach();
            co_await wait_started(probes);
        }(probes));
    }
    for (auto& probe : probes) EXPECT_TRUE(drained(*probe));
}

// ---------------------------------------------------------------------------
// A drain that needs both the timer and the blocking pool
// ---------------------------------------------------------------------------

namespace {

// A Cancellable future whose drain waits first for a timer and then for a blocking
// job that was already running when it was cancelled.
class TimerAndPoolDrain {
public:
    using OutputType = int;
    TimerAndPoolDrain(std::shared_ptr<Probe> probe, std::shared_ptr<Probe> job_probe)
        : m_probe(std::move(probe)), m_job_probe(std::move(job_probe)) {}

    coro::PollResult<int> poll(coro::detail::Context& ctx) {
        if (!m_cancelled) {
            if (!m_job) {
                m_job.emplace(coro::spawn_blocking([job_probe = m_job_probe] {
                    coro::BlockingCancelShield shield;
                    job_probe->started.set();
                    wait_for_cancel_request();
                    std::this_thread::sleep_for(20ms);
                    job_probe->finished.store(true);
                }));
            }
            return coro::PollPending;
        }
        if (!m_timer_done) {
            if (!m_sleep) m_sleep.emplace(coro::sleep_for(20ms));
            if (m_sleep->poll(ctx).isPending()) return coro::PollPending;
            m_timer_done = true;
            m_probe->count.fetch_add(1);
        }
        if (m_job && m_job->poll(ctx).isPending()) return coro::PollPending;
        m_probe->flag.store(m_job_probe->finished.load());
        m_probe->finished.store(true);
        return coro::PollDropped;
    }

    void cancel() { m_cancelled = true; }

private:
    std::shared_ptr<Probe>                    m_probe;
    std::shared_ptr<Probe>                    m_job_probe;
    std::optional<coro::BlockingHandle<void>> m_job;
    std::optional<coro::SleepFuture>          m_sleep;
    bool                                      m_cancelled  = false;
    bool                                      m_timer_done = false;
};
static_assert(coro::Cancellable<TimerAndPoolDrain>);

coro::Coro<void> await_timer_and_pool_drain(std::shared_ptr<Probe> probe,
                                            std::shared_ptr<Probe> job_probe) {
    (void)co_await TimerAndPoolDrain(std::move(probe), std::move(job_probe));
}

} // namespace

TYPED_TEST(RuntimeShutdown, DrainNeedsTimerAndPool) {
    auto probe = make_probe();
    auto job_probe = make_probe();
    {
        auto rt = TypeParam::make();
        rt->block_on([](std::shared_ptr<Probe> probe,
                        std::shared_ptr<Probe> job_probe) -> coro::Coro<void> {
            coro::spawn(await_timer_and_pool_drain(probe, job_probe)).detach();
            co_await job_probe->started.wait();
        }(probe, job_probe));
    }
    EXPECT_EQ(probe->count.load(), 1);     // the timer fired during the drain
    EXPECT_TRUE(probe->flag.load());       // the blocking job had finished
    EXPECT_TRUE(probe->finished.load());   // and the drain ran to its end
}

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------

TYPED_TEST(RuntimeShutdown, ExplicitShutdownThenDestroy) {
    auto probe = make_probe();
    auto rt = TypeParam::make();
    rt->block_on([](std::shared_ptr<Probe> probe) -> coro::Coro<void> {
        coro::spawn(await_draining_future(probe)).detach();
        co_await probe->started.wait();
    }(probe));

    rt->shutdown();
    EXPECT_TRUE(drained(*probe));
    rt->shutdown();                        // no effect
    rt.reset();                            // nor has the destructor
    EXPECT_EQ(probe->count.load(), 2);
}

TYPED_TEST(RuntimeShutdown, UseAfterShutdownThrows) {
    auto rt = TypeParam::make();
    rt->block_on(noop());
    rt->shutdown();

    EXPECT_THROW(rt->block_on(noop()), std::runtime_error);
    EXPECT_THROW((void)rt->spawn(noop()), std::runtime_error);

    // coro::spawn_blocking() needs a current runtime, which this thread no longer
    // has, so reach the pool the way it does.
    auto ran = std::make_shared<std::atomic<bool>>(false);
    auto fn = [ran] { ran->store(true); };
    auto task = coro::detail::make_rc<coro::detail::BlockingTaskImpl<decltype(fn)>>(fn);
    EXPECT_THROW(rt->blocking_pool().submit(task), std::runtime_error);
    EXPECT_FALSE(ran->load());
}

// ---------------------------------------------------------------------------
// Handles and channel ends that outlive the runtime
// ---------------------------------------------------------------------------

namespace {

struct Handles {
    coro::JoinHandle<void>     task;
    coro::StreamHandle<int>    stream;
    coro::BlockingHandle<void> blocking;
};

coro::Coro<Handles> make_handles(std::shared_ptr<Probe> task_probe,
                                 std::shared_ptr<Probe> child_probe,
                                 std::shared_ptr<Probe> blocking_probe) {
    auto task     = coro::spawn(await_draining_future(task_probe));
    auto stream   = coro::spawn(stream_with_child(child_probe));
    auto blocking = coro::spawn_blocking(
        [blocking_probe] { park_until_cancelled(*blocking_probe); });
    std::vector<std::shared_ptr<Probe>> probes{task_probe, child_probe, blocking_probe};
    co_await wait_started(std::move(probes));
    co_return Handles{std::move(task), std::move(stream), std::move(blocking)};
}

} // namespace

TYPED_TEST(RuntimeShutdown, HandleOutlivesRuntime) {
    auto task_probe = make_probe();
    auto child_probe = make_probe();
    auto blocking_probe = make_probe();
    {
        auto rt = TypeParam::make();
        Handles handles = rt->block_on(make_handles(task_probe, child_probe, blocking_probe));
        rt.reset();
        // Shutdown did not wait for the handles to be dropped.
        EXPECT_TRUE(drained(*task_probe));
        EXPECT_TRUE(drained(*child_probe));
        EXPECT_TRUE(blocking_probe->unwound.load());
    }  // the handles are dropped here: each cancels a task that has already finished
}

namespace {

coro::Coro<void> park_on_mpsc(coro::MpscReceiver<int> rx, std::shared_ptr<Probe> probe) {
    Cleanup cleanup{probe};
    probe->started.set();
    (void)co_await rx.recv();
    probe->finished.store(true);
}

coro::Coro<void> park_on_oneshot(coro::OneshotReceiver<int> rx, std::shared_ptr<Probe> probe) {
    Cleanup cleanup{probe};
    probe->started.set();
    (void)co_await rx.recv();
    probe->finished.store(true);
}

coro::Coro<void> park_on_broadcast(coro::BroadcastReceiver<int> rx, std::shared_ptr<Probe> probe) {
    Cleanup cleanup{probe};
    probe->started.set();
    (void)co_await rx.recv();
    probe->finished.store(true);
}

coro::Coro<void> park_on_watch(coro::WatchReceiver<int> rx, std::shared_ptr<Probe> probe) {
    Cleanup cleanup{probe};
    probe->started.set();
    (void)co_await rx.changed();
    probe->finished.store(true);
}

// Gives the detached tasks time to go from `started` to parked on their channel.
coro::Coro<void> let_tasks_park(std::vector<std::shared_ptr<Probe>> probes) {
    co_await wait_started(std::move(probes));
    co_await coro::sleep_for(20ms);
}

} // namespace

TYPED_TEST(RuntimeShutdown, ChannelEndOutlivesRuntime) {
    auto mpsc_probe = make_probe();
    auto oneshot_probe = make_probe();
    auto broadcast_probe = make_probe();
    auto watch_probe = make_probe();

    // The sending ends live here, outside the runtime, and outlive it.
    auto [mpsc_tx, mpsc_rx]           = coro::mpsc_channel<int>(4);
    auto [oneshot_tx, oneshot_rx]     = coro::oneshot_channel<int>();
    auto [broadcast_tx, broadcast_rx] = coro::broadcast_channel<int>(4);
    auto [watch_tx, watch_rx]         = coro::watch_channel<int>(0);
    {
        auto rt = TypeParam::make();
        (void)rt->spawn(park_on_mpsc(std::move(mpsc_rx), mpsc_probe)).detach();
        (void)rt->spawn(park_on_oneshot(std::move(oneshot_rx), oneshot_probe)).detach();
        (void)rt->spawn(park_on_broadcast(std::move(broadcast_rx), broadcast_probe)).detach();
        (void)rt->spawn(park_on_watch(std::move(watch_rx), watch_probe)).detach();
        rt->block_on(let_tasks_park({mpsc_probe, oneshot_probe, broadcast_probe, watch_probe}));
    }
    // Each task's future was destroyed at shutdown, which unlinked its waiter.
    for (auto* probe : {mpsc_probe.get(), oneshot_probe.get(), broadcast_probe.get(),
                        watch_probe.get()}) {
        EXPECT_TRUE(probe->cleaned.load());
        EXPECT_FALSE(probe->finished.load());
    }

    // Sending now reaches no task, and must not reach for the destroyed executor.
    (void)mpsc_tx.try_send(1);
    (void)oneshot_tx.send(1);
    (void)broadcast_tx.send(1);
    (void)watch_tx.send(1);
    for (auto* probe : {mpsc_probe.get(), oneshot_probe.get(), broadcast_probe.get(),
                        watch_probe.get()})
        EXPECT_FALSE(probe->finished.load());
}  // the sending ends are destroyed here, after the runtime

// ---------------------------------------------------------------------------
// Waiters the runtime does not own: released when its I/O driver shuts down
// ---------------------------------------------------------------------------

namespace {

// Long enough that only shutdown can end the wait.
constexpr auto kNever = 1h;

coro::Coro<void> recv_and_record(coro::UdpSocket socket, std::shared_ptr<Probe> probe) {
    probe->started.set();
    try {
        (void)co_await socket.recv_from(std::vector<std::byte>(64));
        probe->finished.store(true);
    } catch (const std::system_error& e) {
        probe->flag.store(e.code() == std::errc::operation_canceled);
    }
}

coro::Coro<void> join(coro::JoinHandle<void> handle) {
    co_await std::move(handle);
}

} // namespace

// A thread that gave itself this runtime as context and waits on one of its timers
// is not a task of the runtime: shutdown neither cancels nor waits for it. It must
// still be woken, and told, rather than left parked on a timer nothing will fire.
TYPED_TEST(RuntimeShutdown, OutsideThreadWaitingOnTimerIsReleased) {
    auto probe = make_probe();
    auto rt = TypeParam::make();
    std::thread waiter([rt = rt.get(), probe] {
        coro::set_current_runtime(rt);
        probe->started.set();
        try {
            coro::blocking_wait(coro::sleep_for(kNever));
            probe->finished.store(true);
        } catch (const std::runtime_error&) {
            probe->flag.store(true);
        }
    });
    rt->block_on(wait_started({probe}));
    std::this_thread::sleep_for(20ms);   // let it park; either side of that must work
    rt->shutdown();
    waiter.join();
    EXPECT_TRUE(probe->flag.load());
    EXPECT_FALSE(probe->finished.load());
}

// The same for a socket: the receive fails with "operation cancelled".
TYPED_TEST(RuntimeShutdown, OutsideThreadWaitingOnSocketIsReleased) {
    auto probe = make_probe();
    auto rt = TypeParam::make();
    std::thread waiter(
        [probe](coro::UdpSocket socket) {
            probe->started.set();
            try {
                (void)coro::blocking_wait(socket.recv_from(std::vector<std::byte>(64)));
                probe->finished.store(true);
            } catch (const std::system_error& e) {
                probe->flag.store(e.code() == std::errc::operation_canceled);
            }
        },   // the socket is destroyed here, before the runtime
        rt->block_on(coro::UdpSocket::bind("127.0.0.1", 0)));
    rt->block_on(wait_started({probe}));
    std::this_thread::sleep_for(20ms);
    rt->shutdown();
    waiter.join();
    EXPECT_TRUE(probe->flag.load());
    EXPECT_FALSE(probe->finished.load());
}

// A task of one runtime waiting on a socket that belongs to another: when the
// socket's runtime shuts down, the task is woken on its own executor and the
// receive fails.
TYPED_TEST(RuntimeShutdown, TaskOfAnotherRuntimeWaitingOnSocketIsReleased) {
    auto probe = make_probe();
    auto owner = TypeParam::make();
    auto user  = TypeParam::make();
    auto handle = user->spawn(
        recv_and_record(owner->block_on(coro::UdpSocket::bind("127.0.0.1", 0)), probe));
    user->block_on(let_tasks_park({probe}));

    owner->shutdown();
    user->block_on(join(std::move(handle)));   // the socket is destroyed with the task
    EXPECT_TRUE(probe->flag.load());
    EXPECT_FALSE(probe->finished.load());
}

// Started after the shutdown, a timer or a socket fails at once.
TYPED_TEST(RuntimeShutdown, TimerAndSocketAfterShutdownThrow) {
    auto rt = TypeParam::make();
    rt->block_on(noop());
    rt->shutdown();

    coro::set_current_runtime(rt.get());
    EXPECT_THROW(coro::blocking_wait(coro::sleep_for(kNever)), std::runtime_error);
    EXPECT_THROW((void)coro::blocking_wait(coro::UdpSocket::bind("127.0.0.1", 0)),
                 std::runtime_error);
    coro::set_current_runtime(nullptr);
}
