#include <gtest/gtest.h>
#include <coro/task/spawn_on.h>
#include <coro/runtime/single_threaded_uv_executor.h>
#include <coro/runtime/runtime.h>
#include <coro/coro.h>
#include <coro/co_invoke.h>
#include <thread>

using namespace coro;

// ---------------------------------------------------------------------------
// spawn_on
// ---------------------------------------------------------------------------

// A future spawned via spawn_on runs on the target executor (verified by checking
// current_uv_executor() from inside the child coroutine).
TEST(SpawnOnTest, RunsOnTargetExecutor) {
    Runtime rt(1);
    SingleThreadedUvExecutor uv_exec;

    SingleThreadedUvExecutor* observed = nullptr;
    rt.block_on(
        [](SingleThreadedUvExecutor& exec, SingleThreadedUvExecutor*& obs) -> Coro<void> {
            co_await spawn_on(exec,
                [](SingleThreadedUvExecutor*& o) -> Coro<void> {
                    o = &current_uv_executor();
                    co_return;
                }(obs)
            ).submit();
        }(uv_exec, observed)
    );

    EXPECT_EQ(observed, &uv_exec);
}

// spawn_on with a value-returning future: the JoinHandle carries the result.
TEST(SpawnOnTest, ReturnsValue) {
    Runtime rt(1);
    SingleThreadedUvExecutor uv_exec;

    int result = 0;
    rt.block_on(
        [](SingleThreadedUvExecutor& exec, int& out) -> Coro<void> {
            out = co_await spawn_on(exec, []() -> Coro<int> {
                co_return 42;
            }()).submit();
        }(uv_exec, result)
    );

    EXPECT_EQ(result, 42);
}

// ---------------------------------------------------------------------------
// with_context
// ---------------------------------------------------------------------------

// with_context is equivalent to spawn_on(...).submit(): the child runs on the
// target executor and the result is returned to the awaiting caller.
TEST(WithContextTest, ReturnsValue) {
    Runtime rt(1);
    SingleThreadedUvExecutor uv_exec;

    int result = 0;
    rt.block_on(
        [](SingleThreadedUvExecutor& exec, int& out) -> Coro<void> {
            out = co_await with_context(exec, []() -> Coro<int> {
                co_return 99;
            }());
        }(uv_exec, result)
    );

    EXPECT_EQ(result, 99);
}

// The child coroutine passed to with_context runs on the target executor.
TEST(WithContextTest, ChildRunsOnTargetExecutor) {
    Runtime rt(1);
    SingleThreadedUvExecutor uv_exec;

    SingleThreadedUvExecutor* observed = nullptr;
    rt.block_on(
        [](SingleThreadedUvExecutor& exec, SingleThreadedUvExecutor*& obs) -> Coro<void> {
            co_await with_context(exec,
                [](SingleThreadedUvExecutor*& o) -> Coro<void> {
                    o = &current_uv_executor();
                    co_return;
                }(obs)
            );
        }(uv_exec, observed)
    );

    EXPECT_EQ(observed, &uv_exec);
}

// with_context void overload: completes without returning a value.
TEST(WithContextTest, VoidFuture) {
    Runtime rt(1);
    SingleThreadedUvExecutor uv_exec;

    bool ran = false;
    rt.block_on(
        [](SingleThreadedUvExecutor& exec, bool& r) -> Coro<void> {
            co_await with_context(exec,
                [](bool& ran) -> Coro<void> {
                    ran = true;
                    co_return;
                }(r)
            );
        }(uv_exec, ran)
    );

    EXPECT_TRUE(ran);
}

// Caller resumes after with_context completes — work after the co_await executes.
TEST(WithContextTest, CallerResumesAfterCompletion) {
    Runtime rt(1);
    SingleThreadedUvExecutor uv_exec;

    int sequence = 0;
    rt.block_on(
        [](SingleThreadedUvExecutor& exec, int& seq) -> Coro<void> {
            seq = 1;
            co_await with_context(exec,
                [](int& s) -> Coro<void> {
                    s = 2;
                    co_return;
                }(seq)
            );
            seq = 3;
        }(uv_exec, sequence)
    );

    EXPECT_EQ(sequence, 3);
}
