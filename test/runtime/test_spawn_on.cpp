#include <gtest/gtest.h>
#include <coro/task/spawn_on.h>
#include <coro/runtime/work_stealing_executor.h>
#include <coro/runtime/runtime.h>
#include <coro/coro.h>
#include <coro/co_invoke.h>
#include <thread>

using namespace coro;

// The target in these tests is a WorkStealingExecutor bound to a second Runtime
// (`other`), separate from the Runtime driving block_on (`rt`, single-threaded).
// Its workers set current_runtime() to `other` and run on their own threads, so a
// child can tell it ran on the target. `other` is declared before `exec` so the
// executor, which turns other's IoDriver, is destroyed first.

struct Observed {
    Runtime*        runtime = nullptr;
    std::thread::id thread;
};

// ---------------------------------------------------------------------------
// spawn_on
// ---------------------------------------------------------------------------

// A future spawned via spawn_on runs on the target executor: on one of its worker
// threads, with the target's runtime current.
TEST(SpawnOnTest, RunsOnTargetExecutor) {
    Runtime rt(1);
    Runtime other(1);
    WorkStealingExecutor exec(&other, 2);

    Observed observed;
    rt.block_on(
        [](Executor& ex, Observed& obs) -> Coro<void> {
            co_await spawn_on(ex,
                [](Observed& o) -> Coro<void> {
                    o.runtime = &current_runtime();
                    o.thread  = std::this_thread::get_id();
                    co_return;
                }(obs)
            );
        }(exec, observed)
    );

    EXPECT_EQ(observed.runtime, &other);
    EXPECT_NE(observed.thread, std::this_thread::get_id());
}

// spawn_on with a value-returning future: the JoinHandle carries the result.
TEST(SpawnOnTest, ReturnsValue) {
    Runtime rt(1);
    Runtime other(1);
    WorkStealingExecutor exec(&other, 2);

    int result = 0;
    rt.block_on(
        [](Executor& ex, int& out) -> Coro<void> {
            out = co_await spawn_on(ex, []() -> Coro<int> {
                co_return 42;
            }());
        }(exec, result)
    );

    EXPECT_EQ(result, 42);
}

// ---------------------------------------------------------------------------
// with_context
// ---------------------------------------------------------------------------

// with_context is equivalent to spawn_on(...): the child runs on the
// target executor and the result is returned to the awaiting caller.
TEST(WithContextTest, ReturnsValue) {
    Runtime rt(1);
    Runtime other(1);
    WorkStealingExecutor exec(&other, 2);

    int result = 0;
    rt.block_on(
        [](Executor& ex, int& out) -> Coro<void> {
            out = co_await with_context(ex, []() -> Coro<int> {
                co_return 99;
            }());
        }(exec, result)
    );

    EXPECT_EQ(result, 99);
}

// The child coroutine passed to with_context runs on the target executor, and the
// caller resumes back on its own runtime and thread.
TEST(WithContextTest, ChildRunsOnTargetExecutor) {
    Runtime rt(1);
    Runtime other(1);
    WorkStealingExecutor exec(&other, 2);

    Observed child;
    Observed caller_after;
    rt.block_on(
        [](Executor& ex, Observed& c, Observed& after) -> Coro<void> {
            co_await with_context(ex,
                [](Observed& o) -> Coro<void> {
                    o.runtime = &current_runtime();
                    o.thread  = std::this_thread::get_id();
                    co_return;
                }(c)
            );
            after.runtime = &current_runtime();
            after.thread  = std::this_thread::get_id();
        }(exec, child, caller_after)
    );

    EXPECT_EQ(child.runtime, &other);
    EXPECT_NE(child.thread, std::this_thread::get_id());
    EXPECT_EQ(caller_after.runtime, &rt);
    EXPECT_EQ(caller_after.thread, std::this_thread::get_id());
}

// with_context void overload: completes without returning a value.
TEST(WithContextTest, VoidFuture) {
    Runtime rt(1);
    Runtime other(1);
    WorkStealingExecutor exec(&other, 2);

    bool ran = false;
    rt.block_on(
        [](Executor& ex, bool& r) -> Coro<void> {
            co_await with_context(ex,
                [](bool& ran) -> Coro<void> {
                    ran = true;
                    co_return;
                }(r)
            );
        }(exec, ran)
    );

    EXPECT_TRUE(ran);
}

// Caller resumes after with_context completes — work after the co_await executes.
TEST(WithContextTest, CallerResumesAfterCompletion) {
    Runtime rt(1);
    Runtime other(1);
    WorkStealingExecutor exec(&other, 2);

    int sequence = 0;
    rt.block_on(
        [](Executor& ex, int& seq) -> Coro<void> {
            seq = 1;
            co_await with_context(ex,
                [](int& s) -> Coro<void> {
                    s = 2;
                    co_return;
                }(seq)
            );
            seq = 3;
        }(exec, sequence)
    );

    EXPECT_EQ(sequence, 3);
}
