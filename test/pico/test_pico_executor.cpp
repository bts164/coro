#include <coro/runtime/runtime.h>
#include <coro/coro.h>
#include <coro/task/join_set.h>
#include <coro/stream.h>
#include <coro/sync/mpsc.h>
#include <coro/sync/oneshot.h>
#include <coro/sync/watch.h>
#include <coro/sync/select.h>
#include <gtest/gtest.h>

// ---------------------------------------------------------------------------
// Basic block_on behaviour
// ---------------------------------------------------------------------------

TEST(PicoExecutor, BlockOnReturnsValue) {
    coro::Runtime rt;
    int result = rt.block_on([]() -> coro::Coro<int> {
        co_return 42;
    }());
    EXPECT_EQ(result, 42);
}

TEST(PicoExecutor, BlockOnVoid) {
    coro::Runtime rt;
    bool ran = false;
    rt.block_on([](bool& ran) -> coro::Coro<void> {
        ran = true;
        co_return;
    }(ran));
    EXPECT_TRUE(ran);
}

TEST(PicoExecutor, BlockOnPropagatesException) {
    coro::Runtime rt;
    EXPECT_THROW(
        rt.block_on([]() -> coro::Coro<void> {
            throw std::runtime_error("boom");
            co_return;
        }()),
        std::runtime_error);
}

// ---------------------------------------------------------------------------
// spawn() and JoinHandle
// ---------------------------------------------------------------------------

TEST(PicoExecutor, SpawnAndAwait) {
    coro::Runtime rt;
    int result = rt.block_on([]() -> coro::Coro<int> {
        auto h = coro::spawn([]() -> coro::Coro<int> {
            co_return 42;
        }());
        co_return co_await h;
    }());
    EXPECT_EQ(result, 42);
}

TEST(PicoExecutor, SpawnMultiple) {
    coro::Runtime rt;
    int total = rt.block_on([]() -> coro::Coro<int> {
        auto a = coro::spawn([](int n) -> coro::Coro<int> { co_return n; }(10));
        auto b = coro::spawn([](int n) -> coro::Coro<int> { co_return n; }(20));
        auto c = coro::spawn([](int n) -> coro::Coro<int> { co_return n; }(12));
        co_return co_await a + co_await b + co_await c;
    }());
    EXPECT_EQ(total, 42);
}

TEST(PicoExecutor, SpawnPropagatesException) {
    coro::Runtime rt;
    EXPECT_THROW(
        rt.block_on([]() -> coro::Coro<void> {
            auto h = coro::spawn([]() -> coro::Coro<void> {
                throw std::runtime_error("child boom");
                co_return;
            }());
            co_await h;
        }()),
        std::runtime_error);
}

// ---------------------------------------------------------------------------
// JoinSet
// ---------------------------------------------------------------------------

TEST(PicoExecutor, JoinSetCollectsResults) {
    coro::Runtime rt;
    int total = rt.block_on([]() -> coro::Coro<int> {
        coro::JoinSet<int> js;
        for (int i = 0; i < 5; ++i)
            js.spawn([](int n) -> coro::Coro<int> { co_return n; }(i));

        int sum = 0;
        while (auto v = co_await coro::next(js))
            sum += *v;
        co_return sum;
    }());
    EXPECT_EQ(total, 0 + 1 + 2 + 3 + 4);
}

// ---------------------------------------------------------------------------
// Cancellation and drain
// ---------------------------------------------------------------------------

TEST(PicoExecutor, CancelOnDrop) {
    // The child is spawned and its handle dropped within the same poll as the
    // parent, so it is cancelled before its body ever runs. The key property
    // under test is that the parent's scope drains the child before block_on()
    // returns — if drain were broken, block_on() would hang indefinitely.
    coro::Runtime rt;
    bool parent_completed = false;

    rt.block_on([](bool& completed) -> coro::Coro<void> {
        {
            auto [tx, rx] = coro::mpsc_channel<int>(1);
            auto h = coro::spawn([](coro::MpscReceiver<int> rx) -> coro::Coro<void> {
                co_await coro::next(rx);  // suspends; tx kept alive in parent scope
            }(std::move(rx)));
            // h drops here — cancel signal sent, scope waits for child to drain.
        }
        completed = true;
        co_return;
    }(parent_completed));

    EXPECT_TRUE(parent_completed);
}

// ---------------------------------------------------------------------------
// Channels
// ---------------------------------------------------------------------------

TEST(PicoExecutor, MpscSendReceive) {
    coro::Runtime rt;
    rt.block_on([]() -> coro::Coro<void> {
        auto [tx, rx] = coro::mpsc_channel<int>(4);

        auto sender = coro::spawn([](coro::MpscSender<int> tx) -> coro::Coro<void> {
            co_await tx.send(1);
            co_await tx.send(2);
            co_await tx.send(3);
            co_return;
        }(std::move(tx)));

        co_await sender;

        auto v1 = co_await coro::next(rx);
        auto v2 = co_await coro::next(rx);
        auto v3 = co_await coro::next(rx);
        // ASSERT_TRUE cannot be used inside a coroutine — it generates a return statement.
        EXPECT_TRUE(v1.has_value());
        EXPECT_TRUE(v2.has_value());
        EXPECT_TRUE(v3.has_value());
        EXPECT_EQ(*v1, 1);
        EXPECT_EQ(*v2, 2);
        EXPECT_EQ(*v3, 3);
    }());
}

TEST(PicoExecutor, OneshotChannel) {
    coro::Runtime rt;
    int received = rt.block_on([]() -> coro::Coro<int> {
        auto [tx, rx] = coro::oneshot_channel<int>();

        auto sender = coro::spawn([](coro::OneshotSender<int> tx) -> coro::Coro<void> {
            tx.send(99);
            co_return;
        }(std::move(tx)));

        auto result = co_await rx.recv();
        int val = result.value_or(0);
        co_await sender;
        co_return val;
    }());
    EXPECT_EQ(received, 99);
}

// ---------------------------------------------------------------------------
// RunningAndNotified CAS path
//
// Simulates an ISR or threadsafe waker firing during a task's own poll by
// having a custom future call its waker before returning PollPending. If the
// RunningAndNotified → Notified transition in poll_ready_tasks were missing,
// the task would park after the first PollPending and poll_count would stay
// at 1 rather than reaching 3.
// ---------------------------------------------------------------------------

TEST(PicoExecutor, RunningAndNotified) {
    coro::Runtime rt;
    int poll_count = 0;

    rt.block_on([](int& count) -> coro::Coro<void> {
        struct SelfWakingFuture {
            using OutputType = void;
            int& count;
            coro::PollResult<void> poll(coro::detail::Context& ctx) {
                ++count;
                if (count >= 3) return coro::PollReady;
                // Wake self while still inside poll() — triggers the
                // Running → RunningAndNotified transition. The executor
                // must re-enqueue the task after poll returns PollPending.
                ctx.getWaker()->wake();
                return coro::PollPending;
            }
        };

        co_await SelfWakingFuture{count};
    }(poll_count));

    EXPECT_EQ(poll_count, 3);
}

// ---------------------------------------------------------------------------
// Task interleaving
// ---------------------------------------------------------------------------

TEST(PicoExecutor, ProducerConsumerInterleaving) {
    // Two tasks alternating via a bounded channel — verifies that the executor
    // correctly interleaves tasks rather than running one to completion first.
    coro::Runtime rt;
    int received_total = rt.block_on([]() -> coro::Coro<int> {
        auto [tx, rx] = coro::mpsc_channel<int>(1);

        auto producer = coro::spawn([](coro::MpscSender<int> tx) -> coro::Coro<void> {
            for (int i = 0; i < 5; ++i)
                co_await tx.send(i);
        }(std::move(tx)));

        int sum = 0;
        for (int i = 0; i < 5; ++i) {
            auto v = co_await coro::next(rx);
            std::cout << "Received" << v.value() << ", ";
            if (v) sum += *v;
        }
        std::cout << "\n";
        co_await producer;
        co_return sum;
    }());
    EXPECT_EQ(received_total, 0 + 1 + 2 + 3 + 4);
}

// ---------------------------------------------------------------------------
// Nested spawn
// ---------------------------------------------------------------------------

TEST(PicoExecutor, NestedSpawn) {
    // A spawned task itself spawns a sub-task. Exercises the scope mechanism
    // across two levels of nesting and verifies all results propagate.
    coro::Runtime rt;
    int result = rt.block_on([]() -> coro::Coro<int> {
        auto outer = coro::spawn([]() -> coro::Coro<int> {
            auto inner = coro::spawn([]() -> coro::Coro<int> {
                co_return 21;
            }());
            co_return co_await inner * 2;
        }());
        co_return co_await outer;
    }());
    EXPECT_EQ(result, 42);
}

// ---------------------------------------------------------------------------
// Many concurrent tasks
// ---------------------------------------------------------------------------

TEST(PicoExecutor, ManyConcurrentTasks) {
    coro::Runtime rt;
    int total = rt.block_on([]() -> coro::Coro<int> {
        coro::JoinSet<int> js;
        for (int i = 0; i < 100; ++i)
            js.spawn([](int n) -> coro::Coro<int> { co_return n; }(i));

        int sum = 0;
        while (auto v = co_await coro::next(js))
            sum += *v;
        co_return sum;
    }());
    EXPECT_EQ(total, 100 * 99 / 2);
}

// ---------------------------------------------------------------------------
// cancelOnDestroy(false)
// ---------------------------------------------------------------------------

TEST(PicoExecutor, CancelOnDestroyFalse) {
    // With cancelOnDestroy(false), dropping the handle does not send a cancel
    // signal — the child runs to natural completion and the parent waits for it.
    coro::Runtime rt;
    bool child_completed = false;

    rt.block_on([](bool& completed) -> coro::Coro<void> {
        auto [tx, rx] = coro::mpsc_channel<int>(1);
        {
            auto h = coro::spawn([](coro::MpscSender<int> tx, bool& completed)
                                   -> coro::Coro<void> {
                co_await tx.send(1);  // signal parent we're running
                completed = true;
                co_return;
            }(std::move(tx), completed));
            h.cancelOnDestroy(false);
            // h drops without cancel — parent must wait for child to finish.
        }
        co_await coro::next(rx);  // wait so we don't race past completed = true
        co_return;
    }(child_completed));

    EXPECT_TRUE(child_completed);
}

// ---------------------------------------------------------------------------
// detach()
// ---------------------------------------------------------------------------

TEST(PicoExecutor, Detach) {
    // detach() fully severs the task — the parent does not wait for the child.
    // We verify the parent completes even though the child would otherwise
    // suspend forever.
    coro::Runtime rt;
    bool parent_completed = false;

    rt.block_on([](bool& completed) -> coro::Coro<void> {
        auto [tx, rx] = coro::mpsc_channel<int>(1);
        coro::spawn([](coro::MpscReceiver<int> rx) -> coro::Coro<void> {
            co_await coro::next(rx);  // suspends forever
        }(std::move(rx))).detach();
        completed = true;
        co_return;
    }(parent_completed));

    EXPECT_TRUE(parent_completed);
}

// ---------------------------------------------------------------------------
// select()
// ---------------------------------------------------------------------------

TEST(PicoExecutor, SelectPicksReadyBranch) {
    coro::Runtime rt;
    // select() returns std::variant<SelectBranch<0,T>, SelectBranch<1,T>>.
    // Use .index() to identify which branch won and .value to extract the result.
    std::size_t branch_index = rt.block_on([]() -> coro::Coro<std::size_t> {
        auto [tx1, rx1] = coro::mpsc_channel<int>(1);
        auto [tx2, rx2] = coro::mpsc_channel<int>(1);

        // Send on rx1 only — select should pick branch 0.
        co_await tx1.send(1);

        auto sel = co_await coro::select(
            [](coro::MpscReceiver<int> r) -> coro::Coro<int> {
                auto v = co_await coro::next(r);
                co_return v.value_or(-1);
            }(std::move(rx1)),
            [](coro::MpscReceiver<int> r) -> coro::Coro<int> {
                auto v = co_await coro::next(r);
                co_return v.value_or(-1);
            }(std::move(rx2))
        );
        co_return sel.index();
    }());
    EXPECT_EQ(branch_index, 0u);  // branch 0 (rx1) had data ready
}

// ---------------------------------------------------------------------------
// Watch channel
// ---------------------------------------------------------------------------

TEST(PicoExecutor, WatchChannel) {
    coro::Runtime rt;
    int seen = rt.block_on([]() -> coro::Coro<int> {
        auto [tx, rx] = coro::watch_channel<int>(0);

        auto updater = coro::spawn([](coro::WatchSender<int> tx) -> coro::Coro<void> {
            tx.send(42);
            co_return;
        }(std::move(tx)));

        co_await updater;
        auto v = co_await rx.changed();
        co_return *rx.borrow();
    }());
    EXPECT_EQ(seen, 42);
}

// ---------------------------------------------------------------------------
// Multiple mpsc senders
// ---------------------------------------------------------------------------

TEST(PicoExecutor, MpscMultipleSenders) {
    coro::Runtime rt;
    int total = rt.block_on([]() -> coro::Coro<int> {
        auto [tx, rx] = coro::mpsc_channel<int>(8);
        auto tx2 = tx.clone();
        auto tx3 = tx.clone();

        auto s1 = coro::spawn([](coro::MpscSender<int> t) -> coro::Coro<void> {
            co_await t.send(10);
        }(std::move(tx)));
        auto s2 = coro::spawn([](coro::MpscSender<int> t) -> coro::Coro<void> {
            co_await t.send(20);
        }(std::move(tx2)));
        auto s3 = coro::spawn([](coro::MpscSender<int> t) -> coro::Coro<void> {
            co_await t.send(12);
        }(std::move(tx3)));

        co_await s1; co_await s2; co_await s3;

        int sum = 0;
        while (auto v = co_await coro::next(rx))
            sum += *v;
        co_return sum;
    }());
    EXPECT_EQ(total, 42);
}
