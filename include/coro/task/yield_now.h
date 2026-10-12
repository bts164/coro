#pragma once

#include <coro/detail/context.h>
#include <coro/detail/poll_result.h>

namespace coro {

/**
 * @brief Future returned by @ref yield_now. Pending on its first poll, ready on its second.
 *
 * The first `poll()` wakes the task and marks the poll as yielded
 * (`Context::yield_to_executor()`), so the executor re-queues the task behind the
 * other runnable ones and turns the I/O driver before polling it again. Nothing stays
 * registered, so dropping it mid-wait is safe.
 */
class YieldNowFuture {
public:
    using OutputType = void;

    PollResult<void> poll(detail::Context& ctx) {
        // Known limit, left as it is on purpose: "second poll" is not the same as
        // "after the executor ran". If something polled this future twice within one
        // task poll, the second poll would return ready here and the coroutine would
        // go on without having yielded.
        //
        // - Nothing in the library does that. Coro, CoroStream, select(), join() and
        //   timeout() poll a child at most once per task poll; fiber_await() and
        //   blocking_wait() poll again only after returning to the executor or being
        //   woken. It would take a hand-written future that polls its child in a loop
        //   until it is no longer pending, which is a busy-wait and already wrong.
        // - If it did happen, the damage is bounded. The first poll has made the wake
        //   and set the mark, so the task is still re-queued and the I/O driver still
        //   turned when the task poll returns, and the task budget still limits how
        //   far it runs before then.
        // - Tokio's yield_now() has the same shape and the same limit.
        //
        // Not a fix: staying pending while ctx.yielded() is true. Another branch of
        // the same join() that yields earlier in every poll would then keep this one
        // pending for good.
        //
        // A fix, if one is ever needed: give Context a sequence number that the
        // executor changes for each task poll (its address will not do; the executors
        // build it at the same stack address every time). Record the number on the
        // first poll here and stay pending while it still matches. That costs a
        // counter in every executor and a wider Context, which is more than the case
        // above is worth.
        if (m_yielded) return PollReady;
        m_yielded = true;
        ctx.yield_to_executor();
        return PollPending;
    }

private:
    bool m_yielded = false;
};

/**
 * @brief Gives the executor the thread back once, then continues.
 *
 * `co_await coro::yield_now()` lets the other runnable tasks, timers and I/O events
 * be handled before the caller goes on. Use it in code that works for a long time
 * without awaiting anything that can be pending:
 *
 * @code
 * for (std::size_t i = 0; i < items.size(); ++i) {
 *     process(items[i]);                              // no co_await in here
 *     if (i % 64 == 63) co_await coro::yield_now();
 * }
 * @endcode
 *
 * A loop that does `co_await` something on each pass needs no `yield_now()`: the
 * task budget makes it yield (doc/design/task_budget.md).
 *
 * Only the coroutine that awaits it stops. Under `select()` or `join()` the other
 * branches are still polled in the same task poll, with the budget that is left; the
 * executor gets the thread back when that poll returns.
 */
[[nodiscard]] inline YieldNowFuture yield_now() noexcept { return {}; }

} // namespace coro
