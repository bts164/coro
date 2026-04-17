#include <gtest/gtest.h>
#include <coro/runtime/single_threaded_uv_executor.h>
#include <coro/runtime/task_waker.h>
#include <coro/detail/task.h>
#include <coro/detail/task_state.h>
#include <coro/coro.h>

using namespace coro;

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

TEST(SingleThreadedUvExecutor, ConstructsAndDestructs) {
    // Executor starts its uv thread on construction and joins on destruction.
    SingleThreadedUvExecutor exec;
}

// ---------------------------------------------------------------------------
// Basic task execution
// ---------------------------------------------------------------------------

TEST(SingleThreadedUvExecutor, RunsSimpleCoroutine) {
    SingleThreadedUvExecutor exec;

    bool ran = false;
    auto state = std::make_shared<detail::TaskState<void>>();
    auto task = [&](bool &ran) -> Coro<void> {
        ran = true;
        co_return;
    }(ran);

    exec.schedule(std::make_unique<detail::Task>(std::move(task), state));
    exec.wait_for_completion(*state);

    EXPECT_TRUE(ran);
}

TEST(SingleThreadedUvExecutor, ReturnsValue) {
    SingleThreadedUvExecutor exec;

    auto state = std::make_shared<detail::TaskState<int>>();
    auto task = []() -> Coro<int> {
        co_return 42;
    }();

    exec.schedule(std::make_unique<detail::Task>(std::move(task), state));
    exec.wait_for_completion(*state);

    ASSERT_TRUE(state->result.has_value());
    EXPECT_EQ(*state->result, 42);
}

// ---------------------------------------------------------------------------
// Multiple tasks
// ---------------------------------------------------------------------------

TEST(SingleThreadedUvExecutor, RunsMultipleTasks) {
    SingleThreadedUvExecutor exec;

    int counter = 0;
    const int N = 10;

    std::vector<std::shared_ptr<detail::TaskState<void>>> states;
    for (int i = 0; i < N; ++i) {
        auto state = std::make_shared<detail::TaskState<void>>();
        auto task = [](int &counter) -> Coro<void> {
            ++counter;
            co_return;
        }(counter);
        exec.schedule(std::make_unique<detail::Task>(std::move(task), state));
        states.push_back(state);
    }

    for (auto& s : states)
        exec.wait_for_completion(*s);

    EXPECT_EQ(counter, N);
}

// ---------------------------------------------------------------------------
// Thread-local access
// ---------------------------------------------------------------------------

TEST(SingleThreadedUvExecutor, CurrentUvExecutorAccessibleFromUvThread) {
    SingleThreadedUvExecutor exec;

    SingleThreadedUvExecutor* observed = nullptr;
    auto state = std::make_shared<detail::TaskState<void>>();
    auto task = [](auto &observed) -> Coro<void> {
        observed = &current_uv_executor();
        co_return;
    }(observed);

    exec.schedule(std::make_unique<detail::Task>(std::move(task), state));
    exec.wait_for_completion(*state);

    EXPECT_EQ(observed, &exec);
}
