#pragma once

#include <coro/detail/context.h>
#include <coro/detail/intrusive_list.h>
#include <coro/detail/mutex.h>
#include <coro/detail/poll_result.h>
#include <coro/detail/rc.h>
#include <coro/detail/wake_batch.h>
#include <coro/detail/waker.h>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <mutex>
#include <utility>
#include <vector>

namespace coro {

class Barrier;

/// @brief What `co_await barrier.wait()` returns.
class BarrierWaitResult {
public:
    explicit BarrierWaitResult(bool leader) noexcept : m_leader(leader) {}

    /// True for exactly one task per round: the one whose arrival released the others.
    bool is_leader() const noexcept { return m_leader; }

private:
    bool m_leader;
};

namespace detail {

/// A suspended arrival, linked into Barrier::m_waiters. Lives in the future.
struct BarrierWaitNode {
    BarrierWaitNode* prev = nullptr;
    BarrierWaitNode* next = nullptr;
    Rc<Waker>        waker;
};

} // namespace detail

/**
 * @brief `Future<BarrierWaitResult>` returned by `Barrier::wait()`.
 *
 * Counts as one arrival at its first poll. Must not be moved after that: the barrier
 * links the future's own node while it waits.
 */
class BarrierWaitFuture {
public:
    using OutputType = BarrierWaitResult;

    explicit BarrierWaitFuture(Barrier* barrier) noexcept : m_barrier(barrier) {}

    /// Only before the first poll, as for every future registered in a list.
    BarrierWaitFuture(BarrierWaitFuture&& other) noexcept
        : m_barrier(other.m_barrier)
        , m_state(other.m_state)
        , m_leader(other.m_leader)
        , m_generation(other.m_generation)
    {
        assert(other.m_state != State::Waiting && "BarrierWaitFuture moved while waiting");
        other.m_state = State::Done;
    }

    BarrierWaitFuture(const BarrierWaitFuture&)            = delete;
    BarrierWaitFuture& operator=(const BarrierWaitFuture&) = delete;
    BarrierWaitFuture& operator=(BarrierWaitFuture&&)      = delete;

    /// A waiter dropped before the release unlinks its node; its arrival stays
    /// counted (as in std::barrier and tokio).
    ~BarrierWaitFuture();

    PollResult<BarrierWaitResult> poll(detail::Context& ctx);

private:
    enum class State : std::uint8_t { Init, Waiting, Done };

    Barrier*                m_barrier;
    State                   m_state      = State::Init;
    bool                    m_leader     = false;
    std::uint64_t           m_generation = 0;  ///< Round this future arrived in.
    detail::BarrierWaitNode m_node;
};

/**
 * @brief Makes a fixed number of tasks wait for each other, round after round.
 *
 * Each of the `n` tasks awaits `wait()`; the last to arrive is the leader, which
 * releases the others and carries on without suspending, and the barrier is ready
 * for the next round. The async counterpart of `std::barrier` and tokio's
 * `sync::Barrier`.
 *
 * ```cpp
 * coro::Barrier barrier(n);
 * // In each of the n tasks:
 * step_one(part);
 * co_await barrier.wait();   // every task has finished step_one
 * step_two(part);
 * ```
 *
 * WARNING: a task that stops arriving (it returns or throws) leaves the others
 * waiting for good. Code that spawns the parts must cancel the rest when one fails,
 * as `parallel()` does.
 *
 * **Lifetime:** must outlive every future that has polled it. Not movable.
 *
 * See doc/design/barrier.md.
 */
class Barrier {
public:
    /// `n` tasks per round; n >= 1.
    explicit Barrier(std::size_t n) : m_n(n) { assert(n >= 1 && "Barrier needs n >= 1"); }

    ~Barrier() {
        assert(m_waiters.empty() && "Barrier destroyed with tasks waiting on it");
    }

    // Not copyable or movable: the futures hold a pointer to it.
    Barrier(const Barrier&)            = delete;
    Barrier& operator=(const Barrier&) = delete;
    Barrier(Barrier&&)                 = delete;
    Barrier& operator=(Barrier&&)      = delete;

    /// @brief One arrival. Resolves when the n-th task of this round has arrived.
    /// The arrival counts at the future's first poll.
    [[nodiscard]] BarrierWaitFuture wait() noexcept { return BarrierWaitFuture(this); }

    /// @brief Tasks per round.
    std::size_t size() const noexcept { return m_n; }

private:
    friend class BarrierWaitFuture;

    /// Released wakers held without a vector allocation.
    static constexpr std::size_t kInlineWakers = 16;

    detail::Mutex                              m_mutex;
    const std::size_t                          m_n;
    std::size_t                                m_arrived    = 0; ///< GUARDED BY m_mutex.
    std::uint64_t                              m_generation = 0; ///< GUARDED BY m_mutex.
    detail::IntrusiveList<detail::BarrierWaitNode*> m_waiters;   ///< GUARDED BY m_mutex.
};

inline BarrierWaitFuture::~BarrierWaitFuture() {
    if (m_state != State::Waiting) return;
    std::lock_guard lock(m_barrier->m_mutex);
    // A release takes the node out of the list under this lock, so a node from a
    // round that has been released is no longer linked.
    if (m_barrier->m_generation == m_generation)
        m_barrier->m_waiters.remove(&m_node);
}

inline PollResult<BarrierWaitResult> BarrierWaitFuture::poll(detail::Context& ctx) {
    if (m_state == State::Done) return BarrierWaitResult(m_leader);

    if (m_state == State::Waiting) {
        std::lock_guard lock(m_barrier->m_mutex);
        // Released if the round has moved on, not because this poll was woken: a
        // spurious wake just registers the waker again.
        if (m_barrier->m_generation != m_generation) {
            m_state = State::Done;
            return BarrierWaitResult(false);
        }
        m_node.waker = ctx.getWaker();
        return PollPending;
    }

    // State::Init: arrive.
    detail::Rc<detail::Waker>              inline_wakers[Barrier::kInlineWakers];
    std::vector<detail::Rc<detail::Waker>> spilled_wakers;
    std::size_t                            woken = 0;
    {
        std::lock_guard lock(m_barrier->m_mutex);
        if (m_barrier->m_arrived + 1 < m_barrier->m_n) {
            // RACE: the generation is recorded and the node linked in the same
            // critical section as the arrival, so a release can't fall between them.
            ++m_barrier->m_arrived;
            m_generation = m_barrier->m_generation;
            m_node.waker = ctx.getWaker();
            m_barrier->m_waiters.push_back(&m_node);
            m_state = State::Waiting;
            return PollPending;
        }

        // The last arrival: the leader releases the round.
        //
        // WARNING: the wakers are taken out and every node unlinked before the lock
        // is released. A waiter's destructor unlinks its node under this lock; if the
        // list were walked after unlocking, a node could belong to a future that had
        // just been destroyed.
        const std::size_t waiting = m_barrier->m_waiters.size();
        if (waiting > Barrier::kInlineWakers) {
            // Before any state changes, so a failed allocation leaves the barrier as
            // it was.
            try {
                spilled_wakers.reserve(waiting);
            } catch (...) {
                return PollError(std::current_exception());
            }
        }
        while (auto* node = m_barrier->m_waiters.pop_front()) {
            if (waiting > Barrier::kInlineWakers)
                spilled_wakers.push_back(std::move(node->waker));
            else
                inline_wakers[woken] = std::move(node->waker);
            ++woken;
        }
        ++m_barrier->m_generation;
        m_barrier->m_arrived = 0;
    }

    // Outside the lock, as in Event::set(). One batch: on WorkStealingExecutor the
    // released tasks are pushed at once and as many parked workers woken, instead
    // of one at a time. A stack local of poll(), so it can't span a suspension.
    {
        detail::WakeBatch batch;
        if (!spilled_wakers.empty()) {
            for (auto& waker : spilled_wakers) waker->wake();
        } else {
            for (std::size_t i = 0; i < woken; ++i) inline_wakers[i]->wake();
        }
    }

    m_state  = State::Done;
    m_leader = true;
    return BarrierWaitResult(true);
}

} // namespace coro
