#include <gtest/gtest.h>
#include <coro/runtime/uv_future.h>
#include <coro/runtime/single_threaded_uv_executor.h>
#include <coro/runtime/runtime.h>
#include <coro/task/spawn_on.h>
#include <coro/coro.h>
#include <uv.h>

using namespace coro;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Heap-allocated timer that closes and deletes itself after firing once.
struct OneShotTimer {
    uv_timer_t handle;
    std::function<void()> callback;

    static void fire_cb(uv_timer_t* t) {
        auto* self = static_cast<OneShotTimer*>(t->data);
        self->callback();
        uv_close(reinterpret_cast<uv_handle_t*>(t), [](uv_handle_t* h) {
            delete static_cast<OneShotTimer*>(h->data);
        });
    }

    static OneShotTimer* start(uv_loop_t* loop, uint64_t ms, std::function<void()> cb) {
        auto* t = new OneShotTimer;
        t->callback = std::move(cb);
        uv_timer_init(loop, &t->handle);
        t->handle.data = t;
        uv_timer_start(&t->handle, fire_cb, ms, 0);
        return t;
    }
};

// ---------------------------------------------------------------------------
// UvCallbackResult
// ---------------------------------------------------------------------------

// complete() stores the value and wakes a waker (basic state machine check).
TEST(UvCallbackResultTest, CompleteStoresValue) {
    UvCallbackResult<int> result;
    EXPECT_FALSE(result.value.has_value());

    result.complete(42);

    EXPECT_TRUE(result.value.has_value());
    EXPECT_EQ(std::get<0>(*result.value), 42);
}

// complete() with multiple args stores all of them.
TEST(UvCallbackResultTest, CompleteMultipleArgs) {
    UvCallbackResult<int, std::string> result;
    result.complete(7, std::string("hello"));

    EXPECT_TRUE(result.value.has_value());
    EXPECT_EQ(std::get<0>(*result.value), 7);
    EXPECT_EQ(std::get<1>(*result.value), "hello");
}

// ---------------------------------------------------------------------------
// UvFuture — basic poll behaviour
// ---------------------------------------------------------------------------

// A UvFuture whose result is already set returns Ready on first poll.
TEST(UvFutureTest, PollReadyWhenResultAlreadySet) {
    auto res = std::make_shared<UvCallbackResult<int>>();
    res->complete(10);

    UvFuture<int> fut(res);

    // We need a Context to poll; create a minimal do-nothing waker.
    struct NoopWaker : detail::Waker {
        void wake() override {}
        std::shared_ptr<detail::Waker> clone() override {
            return std::make_shared<NoopWaker>();
        }
    };
    auto waker = std::make_shared<NoopWaker>();
    detail::Context ctx(waker);

    auto result = fut.poll(ctx);
    EXPECT_TRUE(result.isReady());
    auto [v] = std::move(result).value();
    EXPECT_EQ(v, 10);
}

// A UvFuture whose result is not yet set returns Pending and stores the waker.
TEST(UvFutureTest, PollPendingWhenResultNotSet) {
    auto res = std::make_shared<UvCallbackResult<int>>();
    UvFuture<int> fut(res);

    bool woken = false;
    struct RecordWaker : detail::Waker {
        bool& woken;
        explicit RecordWaker(bool& w) : woken(w) {}
        void wake() override { woken = true; }
        std::shared_ptr<detail::Waker> clone() override {
            return std::make_shared<RecordWaker>(woken);
        }
    };
    auto waker = std::make_shared<RecordWaker>(woken);
    detail::Context ctx(waker);

    auto result = fut.poll(ctx);
    EXPECT_TRUE(result.isPending());

    // Completing the result should wake the registered waker.
    res->complete(99);
    EXPECT_TRUE(woken);
}

// ---------------------------------------------------------------------------
// UvFuture — cancellation
// ---------------------------------------------------------------------------

// The cancel fn is called when the UvFuture is destroyed before the result arrives.
TEST(UvFutureTest, CancelFnCalledOnDestructionBeforeComplete) {
    auto res = std::make_shared<UvCallbackResult<int>>();

    bool cancelled = false;
    {
        UvFuture<int> fut(res, [&cancelled] { cancelled = true; });
        // Destroy without calling complete.
    }
    EXPECT_TRUE(cancelled);
}

// The cancel fn is NOT called if the result has already arrived.
TEST(UvFutureTest, CancelFnNotCalledIfAlreadyComplete) {
    auto res = std::make_shared<UvCallbackResult<int>>();
    res->complete(5);

    bool cancelled = false;
    {
        UvFuture<int> fut(res, [&cancelled] { cancelled = true; });
    }
    EXPECT_FALSE(cancelled);
}

// ---------------------------------------------------------------------------
// UvFuture — integration via uv timer
// ---------------------------------------------------------------------------

// A coroutine running on the uv executor awaits a UvFuture that is completed
// by a one-shot uv timer.
TEST(UvFutureTest, TimerCompletesUvFuture) {
    Runtime rt(1);

    int received = 0;
    rt.block_on(
        [](Runtime& rt_ref, int& out) -> Coro<void> {
            out = co_await with_context(rt_ref.uv_executor(),
                []() -> Coro<int> {
                    auto& exec = current_uv_executor();
                    auto result = std::make_shared<UvCallbackResult<int>>();

                    OneShotTimer::start(exec.loop(), 10, [result] {
                        result->complete(42);
                    });

                    auto [value] = co_await UvFuture<int>(result);
                    co_return value;
                }()
            );
        }(rt, received)
    );

    EXPECT_EQ(received, 42);
}

// Two sequential awaits on separate UvFutures both complete correctly.
TEST(UvFutureTest, SequentialTimersCompleteInOrder) {
    Runtime rt(1);

    int first = 0, second = 0;
    rt.block_on(
        [](Runtime& rt_ref, int& a, int& b) -> Coro<void> {
            co_await with_context(rt_ref.uv_executor(),
                [](int& x, int& y) -> Coro<void> {
                    auto& exec = current_uv_executor();

                    auto r1 = std::make_shared<UvCallbackResult<int>>();
                    OneShotTimer::start(exec.loop(), 5, [r1] { r1->complete(1); });
                    auto [v1] = co_await UvFuture<int>(r1);
                    x = v1;

                    auto r2 = std::make_shared<UvCallbackResult<int>>();
                    OneShotTimer::start(exec.loop(), 5, [r2] { r2->complete(2); });
                    auto [v2] = co_await UvFuture<int>(r2);
                    y = v2;
                }(a, b)
            );
        }(rt, first, second)
    );

    EXPECT_EQ(first, 1);
    EXPECT_EQ(second, 2);
}
