#include <coro/coro.h>
#include <coro/co_invoke.h>
#include <coro/coro_stream.h>
#include <coro/runtime/runtime.h>
#include <coro/future.h>
#include <coro/stream.h>
#include <coro/sync/event.h>
#include <coro/sync/sleep.h>
#include <coro/task/join_set.h>
#include <coro/task/spawn_blocking.h>
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

using namespace std::chrono_literals;

// ---------------------------------------------------------------------------
// Basic result retrieval
// ---------------------------------------------------------------------------

TEST(SpawnBlocking, ReturnsValue) {
    coro::Runtime rt(1);
    int result = rt.block_on([]() -> coro::Coro<int> {
        co_return co_await coro::spawn_blocking([] { return 42; });
    }());
    EXPECT_EQ(result, 42);
}

TEST(SpawnBlocking, ReturnsVoid) {
    coro::Runtime rt(1);
    std::atomic<bool> ran{false};
    rt.block_on([&]() -> coro::Coro<void> {
        co_await coro::spawn_blocking([&] { ran.store(true); });
    }());
    EXPECT_TRUE(ran.load());
}

TEST(SpawnBlocking, ReturnsString) {
    coro::Runtime rt(1);
    std::string result = rt.block_on([]() -> coro::Coro<std::string> {
        co_return co_await coro::spawn_blocking([] { return std::string("hello"); });
    }());
    EXPECT_EQ(result, "hello");
}

// ---------------------------------------------------------------------------
// T = std::exception_ptr (must not be confused with a captured exception)
// ---------------------------------------------------------------------------

TEST(SpawnBlocking, ReturnsExceptionPtr) {
    coro::Runtime rt(1);
    std::exception_ptr ep = rt.block_on([]() -> coro::Coro<std::exception_ptr> {
        co_return co_await coro::spawn_blocking([] {
            try { throw std::runtime_error("inner"); }
            catch (...) { return std::current_exception(); }
        });
    }());
    ASSERT_NE(ep, nullptr);
    EXPECT_THROW(std::rethrow_exception(ep), std::runtime_error);
}

// ---------------------------------------------------------------------------
// Exception propagation
// ---------------------------------------------------------------------------

TEST(SpawnBlocking, PropagatesException) {
    coro::Runtime rt(1);
    EXPECT_THROW(
        rt.block_on([]() -> coro::Coro<int> {
            co_return co_await coro::spawn_blocking([]() -> int {
                throw std::runtime_error("blocking error");
            });
        }()),
        std::runtime_error);
}

TEST(SpawnBlocking, PropagatesExceptionVoid) {
    coro::Runtime rt(1);
    EXPECT_THROW(
        rt.block_on([]() -> coro::Coro<void> {
            co_await coro::spawn_blocking([]() {
                throw std::runtime_error("blocking void error");
            });
        }()),
        std::runtime_error);
}

// ---------------------------------------------------------------------------
// Concurrency — executor thread is freed while blocking work runs
// ---------------------------------------------------------------------------

TEST(SpawnBlocking, ExecutorThreadIsFreeDuringBlockingWork) {
    coro::Runtime rt(1);

    // Spawn two tasks. The blocking work in the first sleeps briefly.
    // If the executor thread were blocked, the second task could never run.
    // We verify both complete.
    std::atomic<int> count{0};

    rt.block_on([&]() -> coro::Coro<void> {
        auto h1 = coro::spawn(coro::co_invoke([&]() -> coro::Coro<void> {
            co_await coro::spawn_blocking([&] {
                std::this_thread::sleep_for(10ms);
                count.fetch_add(1);
            });
            co_return;
        }));

        auto h2 = coro::spawn(coro::co_invoke([&]() -> coro::Coro<void> {
            count.fetch_add(1);
            co_return;
        }));

        co_await h1;
        co_await h2;
    }());

    EXPECT_EQ(count.load(), 2);
}

// ---------------------------------------------------------------------------
// Multiple concurrent blocking tasks
// ---------------------------------------------------------------------------

TEST(SpawnBlocking, MultipleConcurrentTasks) {
    coro::Runtime rt(4);
    constexpr int N = 10;
    std::atomic<int> sum{0};

    rt.block_on([&]() -> coro::Coro<void> {
        coro::JoinSet<void> js;
        for (int i = 0; i < N; ++i) {
            js.spawn(coro::co_invoke([&, i]() -> coro::Coro<void> {
                int v = co_await coro::spawn_blocking([i] { return i * i; });
                sum.fetch_add(v);
            }));
        }
        co_await js.drain();
    }());

    // sum of squares 0..9
    int expected = 0;
    for (int i = 0; i < N; ++i) expected += i * i;
    EXPECT_EQ(sum.load(), expected);
}

// ---------------------------------------------------------------------------
// detach() — handle dropped without awaiting, blocking work still runs
// ---------------------------------------------------------------------------

TEST(SpawnBlocking, DetachedHandleDroppedStillRuns) {
    coro::Runtime rt(1);
    auto ran = std::make_shared<std::atomic<bool>>(false);

    rt.block_on([](std::shared_ptr<std::atomic<bool>> ran) -> coro::Coro<void> {
        // The temporary handle is dropped at once. Detached, so that does not
        // request cancellation: the cancellation point below must not throw.
        coro::spawn_blocking([ran] {
            std::this_thread::sleep_for(5ms);
            coro::blocking_cancellation_point();
            ran->store(true);
        }).detach();
        // Give the detached thread time to finish.
        co_await coro::sleep_for(50ms);
    }(ran));

    EXPECT_TRUE(ran->load());
}

TEST(SpawnBlocking, DetachedHandleCanStillBeAwaited) {
    coro::Runtime rt(1);
    int result = rt.block_on([]() -> coro::Coro<int> {
        auto h = coro::spawn_blocking([] { return 5; });
        h.detach();
        co_return co_await h;
    }());
    EXPECT_EQ(result, 5);
}

// ---------------------------------------------------------------------------
// blocking_wait() on a handle — synchronous wait from a blocking thread
// ---------------------------------------------------------------------------

TEST(SpawnBlocking, BlockingWaitOnHandleRecursive) {
    coro::Runtime rt(1);
    int result = rt.block_on([]() -> coro::Coro<int> {
        co_return co_await coro::spawn_blocking([] {
            // From a blocking thread, submit sub-work and wait synchronously.
            auto h = coro::spawn_blocking([] { return 99; });
            return coro::blocking_wait(std::move(h));
        });
    }());
    EXPECT_EQ(result, 99);
}

TEST(SpawnBlocking, BlockingWaitOnHandleVoid) {
    coro::Runtime rt(1);
    auto ran = std::make_shared<std::atomic<bool>>(false);
    rt.block_on([](std::shared_ptr<std::atomic<bool>> ran) -> coro::Coro<void> {
        co_await coro::spawn_blocking([ran] {
            auto h = coro::spawn_blocking([ran] { ran->store(true); });
            coro::blocking_wait(std::move(h));
        });
    }(ran));
    EXPECT_TRUE(ran->load());
}

TEST(SpawnBlocking, BlockingWaitOnHandleRethrows) {
    coro::Runtime rt(1);
    bool caught = rt.block_on([]() -> coro::Coro<bool> {
        co_return co_await coro::spawn_blocking([] {
            auto h = coro::spawn_blocking([]() -> int { throw std::runtime_error("inner"); });
            try {
                coro::blocking_wait(std::move(h));
            } catch (const std::runtime_error&) {
                return true;
            }
            return false;
        });
    }());
    EXPECT_TRUE(caught);
}

// ---------------------------------------------------------------------------
// current_runtime() must be reachable from a blocking-pool thread --
// worker_loop() sets it (see blocking_pool.cpp) so futures that touch the
// runtime (sleep_for's timer, sockets on the IoDriver) can be polled
// synchronously from inside spawn_blocking work, not just from the Runtime's
// own block_on() thread.
// ---------------------------------------------------------------------------

TEST(SpawnBlocking, RuntimeAvailableOnBlockingThread) {
    coro::Runtime rt(1);
    coro::Runtime* seen = nullptr;

    rt.block_on([](coro::Runtime*& s) -> coro::Coro<void> {
        co_await coro::spawn_blocking([&s] {
            // Throws std::runtime_error if no runtime is active on this thread.
            s = &coro::current_runtime();
        });
    }(seen));

    EXPECT_EQ(seen, &rt);
}

TEST(SpawnBlocking, RuntimeAvailableOnRecursiveBlockingThread) {
    coro::Runtime rt(1);
    coro::Runtime* seen = nullptr;

    rt.block_on([](coro::Runtime*& s) -> coro::Coro<void> {
        co_await coro::spawn_blocking([&s] {
            // A nested spawn_blocking() call runs its work on another
            // blocking-pool thread -- confirm that thread also has the
            // runtime set, not just the first one.
            auto h = coro::spawn_blocking([&s] { s = &coro::current_runtime(); });
            coro::blocking_wait(std::move(h));
        });
    }(seen));

    EXPECT_EQ(seen, &rt);
}

// ---------------------------------------------------------------------------
// Moved-from handle throws on use
// ---------------------------------------------------------------------------

TEST(SpawnBlocking, MovedFromHandleThrows) {
    coro::Runtime rt(1);
    bool threw = rt.block_on([]() -> coro::Coro<bool> {
        // On a blocking thread, so that blocking_wait() is allowed.
        co_return co_await coro::spawn_blocking([] {
            auto h  = coro::spawn_blocking([] { return 1; });
            auto h2 = std::move(h);
            bool threw = false;
            // h is now moved-from — polling it throws.
            try {
                coro::blocking_wait(std::move(h));
            } catch (const std::logic_error&) {
                threw = true;
            }
            coro::blocking_wait(std::move(h2));  // consume h2 so the inner task completes
            return threw;
        });
    }());
    EXPECT_TRUE(threw);
}

// ---------------------------------------------------------------------------
// Cancellation — doc/design/spawn_blocking.md "Cancellation and ownership"
// ---------------------------------------------------------------------------

static_assert(coro::Future<coro::BlockingHandle<int>>);
static_assert(coro::Future<coro::BlockingHandle<void>>);
// request_cancel(), not cancel(): a coroutine cancelled while awaiting a handle
// must not be held until the blocking thread reaches a cancellation point.
static_assert(!coro::Cancellable<coro::BlockingHandle<int>>);
static_assert(!coro::Cancellable<coro::BlockingHandle<void>>);

namespace {

// Shared between a test, its coroutine and its blocking callable. Owned by all
// three through a shared_ptr: a callable must not borrow from the test's stack,
// since nothing waits for it when its handle is dropped.
struct Probe {
    coro::Event       started;          // set by the callable once it is running
    std::atomic<bool> unwound{false};   // the callable saw BlockingCancelled
    std::atomic<bool> finished{false};  // the callable ran past its wait
    std::atomic<bool> flag{false};      // test-specific
    std::atomic<int>  count{0};         // test-specific
};

// Ready at once.
struct ReadyInt {
    using OutputType = int;
    coro::PollResult<int> poll(coro::detail::Context&) { return 7; }
};

// A Cancellable future that never completes on its own. Its first poll sets
// `probe->started`. Once cancelled it needs two more polls to drain: the first
// wakes its own waker and stays pending, the second reports PollDropped.
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

// Runs a DrainingFuture as a task of its own.
coro::Coro<void> await_draining_future(std::shared_ptr<Probe> probe) {
    (void)co_await DrainingFuture(std::move(probe));
}

// A stream that never yields and owns a child task. Its frame cannot simply be
// destroyed: the child has to be cancelled and waited for.
coro::CoroStream<int> stream_with_child(std::shared_ptr<Probe> child_probe) {
    auto child = coro::spawn(await_draining_future(std::move(child_probe)));
    co_yield co_await coro::never<int>();
}

// next(stream) borrows the stream and must not cancel it; blocking_next() drains it.
static_assert(!coro::Cancellable<coro::NextFuture<coro::CoroStream<int>>>);
static_assert(coro::Cancellable<coro::detail::BlockingNextFuture<coro::CoroStream<int>>>);

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

} // namespace

TEST(SpawnBlockingCancel, CancellationPointIsNoOpOffThePool) {
    EXPECT_NO_THROW(coro::blocking_cancellation_point());
    coro::BlockingCancelShield shield;  // no effect here either
    EXPECT_NO_THROW(coro::blocking_cancellation_point());
}

TEST(SpawnBlockingCancel, CancelAndJoinUnwindsParkedCallable) {
    coro::Runtime rt(1);
    auto probe = std::make_shared<Probe>();
    rt.block_on([](std::shared_ptr<Probe> probe) -> coro::Coro<void> {
        auto h = coro::spawn_blocking([probe] { park_until_cancelled(*probe); });
        co_await probe->started.wait();
        // Completes only once the callable has unwound.
        co_await std::move(h).cancel_and_join();
    }(probe));
    EXPECT_TRUE(probe->unwound.load());
    EXPECT_FALSE(probe->finished.load());
}

TEST(SpawnBlockingCancel, DroppingHandleRequestsCancellation) {
    coro::Runtime rt(1);
    auto probe = std::make_shared<Probe>();
    rt.block_on([](std::shared_ptr<Probe> probe) -> coro::Coro<void> {
        {
            auto h = coro::spawn_blocking([probe] { park_until_cancelled(*probe); });
            co_await probe->started.wait();
        }  // dropped: asks the callable to stop, does not wait for it
        for (int i = 0; i < 500 && !probe->unwound.load(); ++i)
            co_await coro::sleep_for(10ms);
    }(probe));
    EXPECT_TRUE(probe->unwound.load());
    EXPECT_FALSE(probe->finished.load());
}

TEST(SpawnBlockingCancel, MoveAssignmentRequestsCancellationOfOldTask) {
    coro::Runtime rt(1);
    auto probe = std::make_shared<Probe>();
    rt.block_on([](std::shared_ptr<Probe> probe) -> coro::Coro<void> {
        auto h = coro::spawn_blocking([probe] { park_until_cancelled(*probe); });
        co_await probe->started.wait();
        h = coro::spawn_blocking([] {});
        co_await h;
        for (int i = 0; i < 500 && !probe->unwound.load(); ++i)
            co_await coro::sleep_for(10ms);
    }(probe));
    EXPECT_TRUE(probe->unwound.load());
}

TEST(SpawnBlockingCancel, CancellationPointThrowsInBusyCallable) {
    coro::Runtime rt(1);
    auto probe = std::make_shared<Probe>();
    rt.block_on([](std::shared_ptr<Probe> probe) -> coro::Coro<void> {
        auto h = coro::spawn_blocking([probe] {
            probe->started.set();
            try {
                for (;;) {
                    coro::blocking_cancellation_point();
                    std::this_thread::sleep_for(1ms);
                }
            } catch (const coro::BlockingCancelled&) {
                probe->unwound.store(true);
                throw;
            }
        });
        co_await probe->started.wait();
        co_await std::move(h).cancel_and_join();
    }(probe));
    EXPECT_TRUE(probe->unwound.load());
}

TEST(SpawnBlockingCancel, CancellationIsSticky) {
    coro::Runtime rt(1);
    auto probe = std::make_shared<Probe>();
    rt.block_on([](std::shared_ptr<Probe> probe) -> coro::Coro<void> {
        auto h = coro::spawn_blocking([probe] {
            try {
                park_until_cancelled(*probe);
            } catch (const coro::BlockingCancelled&) {
                // Swallowed (which a real callable must not do): every later
                // cancellation point throws again.
            }
            try {
                coro::blocking_cancellation_point();
            } catch (const coro::BlockingCancelled&) {
                probe->count.fetch_add(1);
            }
            try {
                (void)coro::blocking_wait(ReadyInt{});  // thrown on entry, not polled for a value
            } catch (const coro::BlockingCancelled&) {
                probe->count.fetch_add(1);
            }
        });
        co_await probe->started.wait();
        co_await std::move(h).cancel_and_join();
    }(probe));
    EXPECT_EQ(probe->count.load(), 2);
}

TEST(SpawnBlockingCancel, ShieldDefersCancellation) {
    coro::Runtime rt(1);
    auto probe = std::make_shared<Probe>();
    rt.block_on([](std::shared_ptr<Probe> probe) -> coro::Coro<void> {
        auto h = coro::spawn_blocking([probe] {
            {
                coro::BlockingCancelShield outer;
                probe->started.set();
                // Wait, shielded, until the cancellation request has arrived.
                while (!coro::detail::current_blocking_task()->is_cancelled())
                    std::this_thread::sleep_for(1ms);
                {
                    coro::BlockingCancelShield inner;  // shields nest
                    coro::blocking_cancellation_point();
                }
                coro::blocking_cancellation_point();              // still shielded
                if (coro::blocking_wait(ReadyInt{}) == 7)         // waits normally
                    probe->flag.store(true);
            }
            // The request was kept, and is delivered now.
            try {
                coro::blocking_cancellation_point();
            } catch (const coro::BlockingCancelled&) {
                probe->unwound.store(true);
                throw;
            }
            probe->finished.store(true);
        });
        co_await probe->started.wait();
        co_await std::move(h).cancel_and_join();
    }(probe));
    EXPECT_TRUE(probe->flag.load());
    EXPECT_TRUE(probe->unwound.load());
    EXPECT_FALSE(probe->finished.load());
}

TEST(SpawnBlockingCancel, CancellableFutureIsDrainedBeforeUnwinding) {
    coro::Runtime rt(1);
    auto probe = std::make_shared<Probe>();
    rt.block_on([](std::shared_ptr<Probe> probe) -> coro::Coro<void> {
        auto h = coro::spawn_blocking([probe] {
            try {
                (void)coro::blocking_wait(DrainingFuture(probe));
            } catch (const coro::BlockingCancelled&) {
                probe->unwound.store(true);
                throw;
            }
            probe->finished.store(true);
        });
        // Set by the future's first poll, so the request arrives while waiting
        // rather than before blocking_wait() is entered.
        co_await probe->started.wait();
        co_await std::move(h).cancel_and_join();
    }(probe));
    EXPECT_TRUE(probe->flag.load());        // cancel() was called
    EXPECT_EQ(probe->count.load(), 2);      // and the future polled until it drained
    EXPECT_TRUE(probe->unwound.load());
    EXPECT_FALSE(probe->finished.load());
}

TEST(SpawnBlockingCancel, CancellableFutureIsDrainedWhenAlreadyCancelled) {
    coro::Runtime rt(1);
    auto probe = std::make_shared<Probe>();
    auto future_probe = std::make_shared<Probe>();
    rt.block_on([](std::shared_ptr<Probe> probe, std::shared_ptr<Probe> future_probe) -> coro::Coro<void> {
        auto h = coro::spawn_blocking([probe, future_probe] {
            try {
                park_until_cancelled(*probe);
            } catch (const coro::BlockingCancelled&) {
                // Swallowed, to reach a blocking_wait() with the request already pending.
            }
            try {
                (void)coro::blocking_wait(DrainingFuture(future_probe));
            } catch (const coro::BlockingCancelled&) {
                probe->flag.store(true);
            }
        });
        co_await probe->started.wait();
        co_await std::move(h).cancel_and_join();
    }(probe, future_probe));
    EXPECT_TRUE(probe->flag.load());              // thrown
    EXPECT_TRUE(future_probe->flag.load());       // after cancel() was called
    EXPECT_EQ(future_probe->count.load(), 2);     // and the never-polled future had drained
}

TEST(SpawnBlockingCancel, BlockingNextDrainsStreamBeforeUnwinding) {
    coro::Runtime rt(1);
    auto probe = std::make_shared<Probe>();
    auto child_probe = std::make_shared<Probe>();
    rt.block_on([](std::shared_ptr<Probe> probe, std::shared_ptr<Probe> child_probe) -> coro::Coro<void> {
        auto h = coro::spawn_blocking([probe, child_probe] {
            auto stream = stream_with_child(child_probe);
            try {
                (void)coro::blocking_next(stream);
            } catch (const coro::BlockingCancelled&) {
                probe->unwound.store(true);
                // The stream's child task has already been cancelled and has drained.
                probe->flag.store(child_probe->count.load() == 2);
                throw;
            }
            probe->finished.store(true);
        });
        // Set by the child's first poll, so the stream has started and spawned it.
        co_await child_probe->started.wait();
        co_await std::move(h).cancel_and_join();
    }(probe, child_probe));
    EXPECT_TRUE(child_probe->flag.load());  // the child's future was cancelled
    EXPECT_TRUE(probe->flag.load());        // and drained before BlockingCancelled was thrown
    EXPECT_TRUE(probe->unwound.load());
    EXPECT_FALSE(probe->finished.load());
}

TEST(SpawnBlockingCancel, WaitingOnCancelledHandleThrowsBlockingCancelled) {
    // The inner task is cancelled and leaves no result, so its handle reports
    // PollDropped; blocking_wait() turns that into BlockingCancelled even though
    // the outer task itself was never cancelled.
    coro::Runtime rt(1);
    bool caught = rt.block_on([]() -> coro::Coro<bool> {
        co_return co_await coro::spawn_blocking([] {
            auto inner = coro::spawn_blocking([] { (void)coro::blocking_wait(coro::never<int>()); });
            inner.request_cancel();
            try {
                coro::blocking_wait(std::move(inner));
            } catch (const coro::BlockingCancelled&) {
                return true;
            }
            return false;
        });
    }());
    EXPECT_TRUE(caught);
}

TEST(SpawnBlockingCancel, ShutdownCancelsParkedCallable) {
    auto probe = std::make_shared<Probe>();
    {
        coro::Runtime rt(1);
        rt.block_on([](std::shared_ptr<Probe> probe) -> coro::Coro<void> {
            // Detached, so only the pool's shutdown can cancel it.
            coro::spawn_blocking([probe] { park_until_cancelled(*probe); }).detach();
            co_await probe->started.wait();
        }(probe));
    }  // ~Runtime: would wait for ever if the pool did not cancel the parked task
    EXPECT_TRUE(probe->unwound.load());
    EXPECT_FALSE(probe->finished.load());
}

TEST(BlockingPoolTest, CancelledWhileQueuedSkipsCallable) {
    coro::Runtime rt(1);
    std::promise<void> release;
    std::shared_future<void> released = release.get_future().share();
    auto ran = std::make_shared<std::atomic<bool>>(false);

    auto first_fn  = [released] { released.wait(); };
    auto second_fn = [ran] { ran->store(true); };
    auto first  = coro::detail::make_rc<coro::detail::BlockingTaskImpl<decltype(first_fn)>>(first_fn);
    auto second = coro::detail::make_rc<coro::detail::BlockingTaskImpl<decltype(second_fn)>>(second_fn);
    {
        // One thread: `second` stays queued while `first` holds it.
        coro::BlockingPool pool(&rt, 1);
        pool.submit(first);
        pool.submit(second);
        second->cancel_task();
        release.set_value();
        // Before the pool goes away: its shutdown cancels every live task, and a
        // cancelled task discards its result even if the callable finished.
        first->wait_until_done();
    }  // ~BlockingPool waits for the thread, which has run both tasks by then

    EXPECT_FALSE(ran->load());
    EXPECT_TRUE(first->is_complete());
    EXPECT_TRUE(first->result);
    // Skipped, but still completed, so a handle awaiting it would see PollDropped.
    EXPECT_TRUE(second->is_complete());
    EXPECT_FALSE(second->result);
    EXPECT_TRUE(second->exception == nullptr);
}
