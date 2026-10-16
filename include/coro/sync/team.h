#pragma once

#include <coro/detail/context.h>
#include <coro/detail/intrusive_list.h>
#include <coro/detail/mutex.h>
#include <coro/detail/poll_result.h>
#include <coro/detail/rc.h>
#include <coro/detail/wake_batch.h>
#include <coro/detail/waker.h>
#include <coro/future.h>
#include <algorithm>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace coro {

class TeamMember;

// ---------------------------------------------------------------------------
// Errors
// ---------------------------------------------------------------------------

/**
 * @brief The error of a collective that can't complete because a member has left the
 * team. Every later collective fails with it too.
 */
class TeamBroken : public std::exception {
public:
    TeamBroken(std::size_t rank, std::uint64_t rounds_joined, std::uint64_t round) noexcept
        : m_rank(rank), m_rounds_joined(rounds_joined), m_round(round) {}

    /// The first member to leave.
    std::size_t rank() const noexcept { return m_rank; }
    /// How many rounds that member had joined when it left.
    std::uint64_t rounds_joined() const noexcept { return m_rounds_joined; }
    /// The round that failed, counted from 0.
    std::uint64_t round() const noexcept { return m_round; }

    const char* what() const noexcept override { return "coro::TeamBroken: a member left the team"; }

private:
    std::size_t   m_rank;
    std::uint64_t m_rounds_joined;
    std::uint64_t m_round;
};

/**
 * @brief The error of every member's collective once a member has used the team in a
 * way it doesn't support. Breaks the team; every later collective fails with it too.
 *
 * Using a moved-from TeamMember is the exception: it has no team to break, so only
 * that call fails, and rank() is SIZE_MAX.
 */
class TeamUsageError : public std::logic_error {
public:
    TeamUsageError(const char* why, std::uint64_t round, std::size_t rank,
                   std::size_t other_rank = SIZE_MAX)
        : std::logic_error(describe(why, round, rank, other_rank))
        , m_round(round), m_rank(rank), m_other_rank(other_rank) {}

    std::uint64_t round() const noexcept { return m_round; }
    /// The member whose check failed.
    std::size_t rank() const noexcept { return m_rank; }
    /// For a mismatch, the member that set the round's signature; SIZE_MAX otherwise.
    std::size_t other_rank() const noexcept { return m_other_rank; }

private:
    static std::string describe(const char* why, std::uint64_t round, std::size_t rank,
                                std::size_t other_rank) {
        std::string s = "coro::TeamUsageError: ";
        s += why;
        // A moved-from handle has no rank or rounds.
        if (rank == SIZE_MAX) return s;
        s += " (round " + std::to_string(round) + ", rank " + std::to_string(rank);
        if (other_rank != SIZE_MAX) s += ", signature set by rank " + std::to_string(other_rank);
        s += ")";
        return s;
    }

    std::uint64_t m_round;
    std::size_t   m_rank;
    std::size_t   m_other_rank;
};

/**
 * @brief The error of a collective whose round was broken by a member's cancelled
 * collective. The next round is unaffected.
 */
class TeamRoundBroken : public std::exception {
public:
    explicit TeamRoundBroken(std::size_t rank) noexcept : m_rank(rank) {}

    /// The first member to cancel in that round.
    std::size_t rank() const noexcept { return m_rank; }

    const char* what() const noexcept override {
        return "coro::TeamRoundBroken: a member cancelled its collective";
    }

private:
    std::size_t m_rank;
};

// ---------------------------------------------------------------------------
// Cheap and expensive ops
// ---------------------------------------------------------------------------

template<typename F> class CheapOp;
template<typename F> class ExpensiveOp;

namespace detail {

// 0: neither wrapper, 1: CheapOp, 2: ExpensiveOp.
template<typename Op> struct TeamOpMode : std::integral_constant<int, 0> {};
template<typename F> struct TeamOpMode<CheapOp<F>> : std::integral_constant<int, 1> {};
template<typename F> struct TeamOpMode<ExpensiveOp<F>> : std::integral_constant<int, 2> {};

template<typename Op>
inline constexpr bool is_team_op_wrapper = TeamOpMode<Op>::value != 0;

/// Whether `op` combines under the team's lock. See doc/design/team.md, "Cheap and
/// expensive ops".
template<typename T, typename Op>
inline constexpr bool team_op_is_cheap =
    TeamOpMode<Op>::value == 1 ||
    (TeamOpMode<Op>::value == 0 && std::is_trivially_copyable_v<T> && sizeof(T) <= 64);

} // namespace detail

/// @brief Calls `F`, and marks it as cheap: a team combines with it under its lock.
template<typename F>
class CheapOp {
    static_assert(!detail::is_team_op_wrapper<F>,
                  "an op is wrapped in cheap_op() or expensive_op() once");

public:
    constexpr explicit CheapOp(F f) : m_f(std::move(f)) {}

    template<typename... A> requires std::invocable<F&, A...>
    constexpr decltype(auto) operator()(A&&... a) { return std::invoke(m_f, std::forward<A>(a)...); }

    template<typename... A> requires std::invocable<const F&, A...>
    constexpr decltype(auto) operator()(A&&... a) const {
        return std::invoke(m_f, std::forward<A>(a)...);
    }

private:
    F m_f;
};

/// @brief Calls `F`, and marks it as expensive: a team combines with it outside its
/// lock, in parallel.
template<typename F>
class ExpensiveOp {
    static_assert(!detail::is_team_op_wrapper<F>,
                  "an op is wrapped in cheap_op() or expensive_op() once");

public:
    constexpr explicit ExpensiveOp(F f) : m_f(std::move(f)) {}

    template<typename... A> requires std::invocable<F&, A...>
    constexpr decltype(auto) operator()(A&&... a) { return std::invoke(m_f, std::forward<A>(a)...); }

    template<typename... A> requires std::invocable<const F&, A...>
    constexpr decltype(auto) operator()(A&&... a) const {
        return std::invoke(m_f, std::forward<A>(a)...);
    }

private:
    F m_f;
};

template<typename F>
    requires (!detail::is_team_op_wrapper<std::decay_t<F>>)
[[nodiscard]] constexpr CheapOp<std::decay_t<F>> cheap_op(F&& f) {
    return CheapOp<std::decay_t<F>>(std::forward<F>(f));
}

template<typename F>
    requires (!detail::is_team_op_wrapper<std::decay_t<F>>)
[[nodiscard]] constexpr ExpensiveOp<std::decay_t<F>> expensive_op(F&& f) {
    return ExpensiveOp<std::decay_t<F>>(std::forward<F>(f));
}

namespace detail {

// ---------------------------------------------------------------------------
// What an op must be
// ---------------------------------------------------------------------------

template<typename Op, typename T, typename A, typename B>
concept TeamOpFor =
    std::invocable<Op&, A, B> &&
    std::convertible_to<std::invoke_result_t<Op&, A, B>, T> &&
    !Future<std::remove_cvref_t<std::invoke_result_t<Op&, A, B>>>;

/// A reduction calls op(T&&, T&&): both operands are used once.
template<typename Op, typename T>
concept TeamReduceOp = TeamOpFor<Op, T, T&&, T&&>;

/// A scan calls op(const T&, T&&): the left operand is kept for the down-sweep.
template<typename Op, typename T>
concept TeamScanOp = TeamOpFor<Op, T, const T&, T&&>;

/// Elementwise operands are input elements (const T&) or intermediates (T&&).
template<typename Op, typename T>
concept TeamElementwiseOp =
    TeamOpFor<Op, T, const T&, const T&> && TeamOpFor<Op, T, const T&, T&&> &&
    TeamOpFor<Op, T, T&&, const T&> && TeamOpFor<Op, T, T&&, T&&>;

// ---------------------------------------------------------------------------
// is_broken() from inside an op
// ---------------------------------------------------------------------------

#ifdef CORO_PICO
// No hardware TLS on Cortex-M0+ (see coro_scope.h); one thread, so a global is the same.
inline bool t_team_in_op        = false;
inline bool t_team_op_misused   = false;
#else
inline thread_local bool t_team_in_op      = false;
inline thread_local bool t_team_op_misused = false;
#endif

/// Marks this thread as running an op, so that is_broken() can tell.
class TeamOpScope {
public:
    TeamOpScope() noexcept : m_prev_in_op(t_team_in_op), m_prev_misused(t_team_op_misused) {
        t_team_in_op      = true;
        t_team_op_misused = false;
    }
    ~TeamOpScope() {
        t_team_in_op      = m_prev_in_op;
        t_team_op_misused = m_prev_misused;
    }
    TeamOpScope(const TeamOpScope&)            = delete;
    TeamOpScope& operator=(const TeamOpScope&) = delete;

    bool misused() const noexcept { return t_team_op_misused; }

private:
    bool m_prev_in_op;
    bool m_prev_misused;
};

/// Runs `f`, which calls an op. Returns what it threw, and sets `misused` if it
/// called is_broken().
template<typename F>
std::exception_ptr team_call_op(bool& misused, F&& f) noexcept {
    TeamOpScope        scope;
    std::exception_ptr error;
    try {
        f();
    } catch (...) {
        error = std::current_exception();
    }
    misused = scope.misused();
    return error;
}

// ---------------------------------------------------------------------------
// Shared state
// ---------------------------------------------------------------------------

inline constexpr std::size_t   kTeamNoRank  = SIZE_MAX;
inline constexpr std::uint64_t kTeamNoRound = UINT64_MAX;

/// [begin, end) of rank's even share of n items among k; the first n % k get one more.
inline std::pair<std::size_t, std::size_t> team_range(std::size_t rank, std::size_t k,
                                                      std::size_t n) noexcept {
    const std::size_t base  = n / k;
    const std::size_t extra = n % k;
    const std::size_t begin = rank * base + (rank < extra ? rank : extra);
    return {begin, begin + base + (rank < extra ? 1 : 0)};
}

/// Identifies a type without RTTI: one address per type.
template<typename T>
inline constexpr char team_type_tag = 0;

enum class TeamCollective : std::uint8_t {
    Sync, AllReduce, Broadcast, AllGather, AllGatherVector, Reduce, Gather, Scatter,
    ExclusiveScan, InclusiveScan, Elementwise,
};

/// What every member's call in a round must agree on. See doc/design/team.md,
/// "Checked misuse".
struct TeamSignature {
    TeamCollective               kind     = TeamCollective::Sync;
    const void*                  type     = nullptr;   // &team_type_tag<T>
    const void*                  op       = nullptr;   // &team_type_tag<Op>
    bool                         cheap    = false;
    std::size_t                  root     = kTeamNoRank;
    const void*                  out      = nullptr;   // elementwise
    std::size_t                  out_size = 0;
    std::optional<std::uint64_t> tag;

    bool operator==(const TeamSignature&) const = default;
};

/// The non-template part of every collective future.
struct TeamNode {
    TeamNode* prev = nullptr;   ///< TeamState::next_round
    TeamNode* next = nullptr;

    Rc<struct TeamState> state;   ///< Null for a collective on a moved-from handle.
    std::size_t          rank  = 0;
    std::uint64_t        round = 0;
    std::optional<std::uint64_t> tag;
    void*                algo  = nullptr;   ///< The future's algorithm, once it has begun.

    Rc<Waker>   waker;        ///< GUARDED BY state->mutex.
    std::size_t claims = 0;   ///< Other members using this future's values. GUARDED.

    bool polled           = false;
    bool counted          = false;   ///< Passed the one-future-at-a-time check.
    bool begun            = false;
    bool done             = false;   ///< Round succeeded, result in. GUARDED.
    bool finished         = false;
    bool cancel_requested = false;   ///< GUARDED once polled.
    bool misused          = false;   ///< Moved after its first poll.
    bool destroying       = false;   ///< GUARDED.
};

/// Wakers collected under the team's lock, woken after it is released.
class TeamWakers {
public:
    TeamWakers() = default;
    TeamWakers(const TeamWakers&)            = delete;
    TeamWakers& operator=(const TeamWakers&) = delete;

    void add(Rc<Waker>&& waker) noexcept {
        if (!waker) return;
        if (m_count < kInline) {
            m_inline[m_count++] = std::move(waker);
            return;
        }
        try {
            m_spilled.push_back(std::move(waker));
        } catch (...) {
            // RACE: no memory to hold it until the lock is released, so it is woken
            // under the lock. A wake only queues the task, so that is safe, just slower.
            waker->wake();
        }
    }

    /// Outside the lock. One batch, as in Barrier: the woken tasks are pushed at once.
    void wake_all() {
        if (m_count == 0) return;
        WakeBatch batch;
        for (std::size_t i = 0; i < m_count; ++i) m_inline[i]->wake();
        for (auto& waker : m_spilled) waker->wake();
    }

private:
    static constexpr std::size_t kInline = 16;

    Rc<Waker>              m_inline[kInline];
    std::size_t            m_count = 0;
    std::vector<Rc<Waker>> m_spilled;
};

using TeamLock = std::unique_lock<Mutex>;

/// Per rank, the member's place in the round sequence.
struct TeamSlot {
    /// The round of the next future this member may poll: the last one has finished.
    std::uint64_t settled = 0;
    /// A round this member dropped a future for unpolled before it could begin.
    std::uint64_t owed = kTeamNoRound;
};

/**
 * Shared by a team's handles and futures. Every field is guarded by `mutex`. See
 * doc/design/team.md, "Rounds" for the rules these functions carry out.
 */
struct TeamState {
    explicit TeamState(std::size_t k)
        : size(k), height(height_for(k)), slots(k), members(k, nullptr), avail(k, -1) {}

    Mutex         mutex;
    CondVar       claims_cv;   ///< A destroyed future waits here for its claims.
    const std::size_t size;
    const int     height;      ///< Of the combining tree: ceil(log2(size)).

    std::uint64_t round       = 0;   ///< Every earlier round is complete.
    std::size_t   arrived     = 0;   ///< Members that have begun the current round.
    std::size_t   checked_in  = 0;   ///< Once the round is broken.
    bool          round_broken      = false;
    std::size_t   round_broken_rank = kTeamNoRank;
    std::exception_ptr op_error;     ///< The round was broken by this exception.

    bool          broken      = false;   ///< The team, for good.
    std::size_t   broken_rank = kTeamNoRank;
    std::uint64_t broken_rounds_joined = 0;
    std::exception_ptr usage_error;      ///< If a check broke it.

    bool          signature_set  = false;
    TeamSignature signature;
    std::size_t   signature_rank = kTeamNoRank;

    bool          phase2      = false;   ///< Elementwise: every member has arrived.
    std::size_t   slices_done = 0;       ///< Elementwise: members done with phase 2.

    IntrusiveList<TeamNode*> next_round;   ///< Waiting for the current round to complete.
    std::vector<TeamSlot>    slots;
    std::vector<TeamNode*>   members;      ///< This round's arrivals, by rank.
    /// Per rank: the tree level of the total available in its future, -1 if none (not
    /// arrived, claimed or used up). A total at level l covers [r, r + 2^l).
    std::vector<int>         avail;

    static int height_for(std::size_t k) noexcept {
        int h = 0;
        while ((std::size_t{1} << h) < k) ++h;
        return h;
    }

    /// Passes the total at `r` up through levels where it has no partner.
    void normalize(std::size_t r) noexcept {
        while (avail[r] >= 0 && avail[r] < height) {
            const std::size_t half = std::size_t{1} << avail[r];
            if (r & ((half << 1) - 1)) break;   // a right input at the next level
            if (r + half < size) break;         // has a partner
            ++avail[r];
        }
    }

    void wake_registered(TeamWakers& wakers) noexcept {
        for (TeamNode* node : members)
            if (node && node->claims == 0) wakers.add(std::move(node->waker));
    }

    /// Rule 2: a cancel, or a future dropped unpolled.
    void break_round(std::size_t rank, TeamWakers& wakers) noexcept {
        if (round_broken) return;
        round_broken      = true;
        round_broken_rank = rank;
        wake_registered(wakers);
    }

    /// Rule 2: an op threw.
    void break_round_error(std::exception_ptr error, TeamWakers& wakers) noexcept {
        if (round_broken) return;
        round_broken = true;
        op_error     = std::move(error);
        wake_registered(wakers);
    }

    /// Rule 3. Waiters whose values are claimed are woken by the claimer.
    void break_team(std::size_t rank, std::uint64_t rounds_joined, TeamWakers& wakers) noexcept {
        if (broken) return;
        broken               = true;
        broken_rank          = rank;
        broken_rounds_joined = rounds_joined;
        wake_registered(wakers);
        while (TeamNode* node = next_round.pop_front()) wakers.add(std::move(node->waker));
    }

    void fail_usage(std::size_t rank, std::uint64_t at_round, std::size_t other_rank,
                    const char* why, TeamWakers& wakers) noexcept {
        if (broken) return;
        try {
            usage_error = std::make_exception_ptr(TeamUsageError(why, at_round, rank, other_rank));
        } catch (...) {
            usage_error = std::current_exception();
        }
        break_team(rank, 0, wakers);
    }

    std::exception_ptr team_error(std::uint64_t at_round) const {
        if (usage_error) return usage_error;
        return std::make_exception_ptr(TeamBroken(broken_rank, broken_rounds_joined, at_round));
    }

    std::exception_ptr round_error() const {
        if (op_error) return op_error;
        return std::make_exception_ptr(TeamRoundBroken(round_broken_rank));
    }

    void claim(const TeamNode& self, TeamNode& node) noexcept {
        if (&node != &self) ++node.claims;
    }

    void release(const TeamNode& self, TeamNode& node, TeamWakers& wakers) noexcept {
        if (&node == &self) return;
        if (--node.claims != 0) return;
        if (node.destroying)
            claims_cv.notify_all();
        else if (broken || round_broken)
            wakers.add(std::move(node.waker));
    }

    /// Takes `node` out of the tables. It must hold no claims.
    void leave(TeamNode& node) noexcept {
        if (members[node.rank] == &node) members[node.rank] = nullptr;
        next_round.remove(&node);
    }

    /// Rule 5.
    void check_in(TeamNode& node, TeamWakers& wakers) noexcept {
        leave(node);
        if (++checked_in == size) advance(wakers);
    }

    /// Rule 4: every member's result is in its future.
    void complete_success(const TeamNode& self, TeamWakers& wakers) noexcept {
        for (TeamNode* node : members) {
            node->done = true;
            if (node != &self) wakers.add(std::move(node->waker));
        }
        advance(wakers);
    }

    /// The current round is complete: clears the tables and lets the next one begin.
    void advance(TeamWakers& wakers) noexcept {
        for (;;) {
            ++round;
            arrived = checked_in = 0;
            round_broken         = false;
            round_broken_rank    = kTeamNoRank;
            op_error             = nullptr;
            signature_set        = false;
            phase2               = false;
            slices_done          = 0;
            std::fill(members.begin(), members.end(), nullptr);
            std::fill(avail.begin(), avail.end(), -1);
            // Futures dropped unpolled before this round could begin.
            for (std::size_t r = 0; r < size; ++r) {
                if (slots[r].owed != round) continue;
                slots[r].owed = kTeamNoRound;
                if (!round_broken) {
                    round_broken      = true;
                    round_broken_rank = r;
                }
                ++checked_in;
            }
            if (checked_in < size) break;
        }
        while (TeamNode* node = next_round.pop_front()) wakers.add(std::move(node->waker));
    }

    /// A future destroyed before its first poll. See doc/design/team.md, "Races".
    void drop_unpolled(TeamNode& node, TeamWakers& wakers) noexcept {
        if (broken) return;
        TeamSlot& slot = slots[node.rank];
        if (slot.settled != node.round) {
            fail_usage(node.rank, node.round, kTeamNoRank,
                       "a collective future was dropped while this member's last one is "
                       "unfinished", wakers);
            return;
        }
        slot.settled = node.round + 1;
        if (node.round == round) {
            break_round(node.rank, wakers);
            check_in(node, wakers);
        } else if (node.round == round + 1 && slot.owed == kTeamNoRound) {
            slot.owed = node.round;
        } else {
            fail_usage(node.rank, node.round, kTeamNoRank,
                       "a collective future was dropped unpolled while an earlier one "
                       "dropped the same way still owes its round", wakers);
        }
    }
};

/// The algorithm of the member at `rank` in this round. Every member's call matched
/// the signature, so it is the same Algo type.
template<typename Algo>
Algo& team_peer(TeamState& s, std::size_t rank) noexcept {
    return *static_cast<Algo*>(s.members[rank]->algo);
}

// ---------------------------------------------------------------------------
// The future
// ---------------------------------------------------------------------------

/**
 * @brief Every collective's future: the round protocol, with the collective's own work
 * in `Algo`. Cancellable.
 *
 * An Algo provides `Output`, `signature()`, `check(k, rank)` (a message if its own
 * arguments are wrong), `prepare(k, rank)` (allocation, outside the lock),
 * `begin(self, state, lock, wakers)` (registered, under the lock), `resume(...)` (a
 * later poll of a begun member, round not broken) and `take(rank)`.
 *
 * Must not be moved after its first poll: the team holds pointers into it. A move then
 * gives a future that fails with TeamUsageError, and leaves this one to be destroyed
 * unfinished, which breaks the team.
 */
template<typename Algo>
class TeamFuture {
public:
    using OutputType = typename Algo::Output;

    TeamFuture(Rc<TeamState> state, std::size_t rank, std::uint64_t round, Algo algo)
        : m_algo(std::move(algo))
    {
        m_node.state = std::move(state);
        m_node.rank  = rank;
        m_node.round = round;
    }

    TeamFuture(TeamFuture&& other) noexcept(std::is_nothrow_move_constructible_v<Algo>) {
        m_node.rank             = other.m_node.rank;
        m_node.round            = other.m_node.round;
        m_node.tag              = other.m_node.tag;
        m_node.cancel_requested = other.m_node.cancel_requested;
        if (other.m_node.polled) {
            // The other future stays registered; its values aren't touched here.
            m_node.state   = other.m_node.state;
            m_node.misused = true;
        } else {
            m_node.state = std::move(other.m_node.state);
            // emplace, not assignment: an op holding references can't be assigned.
            if (other.m_algo) m_algo.emplace(std::move(*other.m_algo));
        }
    }

    TeamFuture(const TeamFuture&)            = delete;
    TeamFuture& operator=(const TeamFuture&) = delete;
    TeamFuture& operator=(TeamFuture&&)      = delete;

    ~TeamFuture() {
        TeamNode& n = m_node;
        if (!n.state || n.finished || n.misused) return;
        TeamWakers wakers;
        {
            TeamLock   lock(n.state->mutex);
            TeamState& s = *n.state;
            if (!n.polled) {
                s.drop_unpolled(n, wakers);
            } else {
                // Rule 7 broken: polled, not finished.
                n.waker.reset();
                s.next_round.remove(&n);
                s.fail_usage(n.rank, n.round, kTeamNoRank,
                             "a collective future was destroyed after its first poll, "
                             "before it finished", wakers);
                // WARNING: another member may be combining this future's values
                // outside the lock. Block the thread until it lets go, so it doesn't
                // write into freed memory: at most one combine of an expensive op.
                // Checked first: on Pico, wait() never returns (and claims is always
                // 0 there, one thread).
                n.destroying = true;
                if (n.claims != 0) s.claims_cv.wait(lock, [&] { return n.claims == 0; });
                s.leave(n);
            }
        }
        wakers.wake_all();
    }

    /// @brief Sets the opt-in tag that this round's signature includes. Before the
    /// first poll. See doc/design/team.md, "Checked misuse".
    [[nodiscard]] TeamFuture tag(std::uint64_t value) && {
        // After the first poll the move below gives a future that fails, and this
        // one, destroyed unfinished, breaks the team.
        if (!m_node.polled) m_node.tag = value;
        return std::move(*this);
    }

    /// @brief Gives up on this collective. Breaks its round unless the round has
    /// already succeeded; poll until `PollDropped`.
    void cancel() {
        TeamNode& n = m_node;
        if (!n.state || n.finished || n.misused) {
            n.cancel_requested = true;
            return;
        }
        TeamWakers wakers;
        {
            std::lock_guard lock(n.state->mutex);
            TeamState&      s = *n.state;
            n.cancel_requested = true;
            // RACE: the lock orders this against the round's last combine. If that
            // came first, the result is in and is only discarded (rule 4).
            if (n.begun && !n.done && !s.broken && s.round == n.round)
                s.break_round(n.rank, wakers);
        }
        wakers.wake_all();
    }

    PollResult<OutputType> poll(Context& ctx) {
        TeamNode& n = m_node;
        assert(!n.finished && "team collective future polled after it finished");
        if (!n.state) {
            n.finished = true;
            if (n.cancel_requested) return PollDropped;
            return PollError(std::make_exception_ptr(TeamUsageError(
                "a collective was called on a moved-from TeamMember", n.round, n.rank)));
        }
        if (!n.polled && !n.misused) {
            // Outside the lock, before the round may begin: a failed allocation breaks
            // the round, as an op that throws does.
            try {
                m_algo->prepare(n.state->size, n.rank);
            } catch (...) {
                m_prepare_error = std::current_exception();
            }
        }
        TeamWakers             wakers;
        PollResult<OutputType> result = poll_locked(ctx, wakers);
        wakers.wake_all();
        return result;
    }

private:
    PollResult<OutputType> poll_locked(Context& ctx, TeamWakers& wakers) {
        TeamNode&  n = m_node;
        TeamLock   lock(n.state->mutex);
        TeamState& s = *n.state;

        if (n.misused) {
            s.fail_usage(n.rank, n.round, kTeamNoRank,
                         "a collective future was moved after its first poll", wakers);
            n.finished = true;
            return PollError(s.team_error(n.round));
        }
        if (!n.polled) {
            n.polled = true;
            if (!s.broken && s.slots[n.rank].settled != n.round) {
                s.fail_usage(n.rank, n.round, kTeamNoRank,
                             "a collective was started while this member's last one is "
                             "unfinished", wakers);
                n.finished = true;
                return PollError(s.team_error(n.round));
            }
            n.counted = true;
        }

        if (!n.done && !s.broken) {
            if (!n.begun) {
                // Rule 1.
                if (n.round != s.round) {
                    n.waker = ctx.getWaker();
                    s.next_round.remove(&n);
                    s.next_round.push_back(&n);
                    return PollPending;
                }
                s.next_round.remove(&n);
                if (n.cancel_requested)
                    s.break_round(n.rank, wakers);
                else if (m_prepare_error)
                    s.break_round_error(m_prepare_error, wakers);
                if (!s.round_broken) begin(s, lock, wakers);
            } else if (!s.round_broken) {
                m_algo->resume(n, s, lock, wakers);
            }
        }

        if (n.done) {
            finish(s);
            if (n.cancel_requested) return PollDropped;   // a late cancel discards it
            if constexpr (std::is_void_v<OutputType>)
                return PollReady;
            else
                return PollResult<OutputType>(m_algo->take(n.rank));
        }

        // Past rule 1, so a round that isn't the team's is this future's round.
        if (s.broken || s.round_broken) {
            if (n.claims > 0) {   // rule 6: the claimer wakes it
                n.waker = ctx.getWaker();
                return PollPending;
            }
            // Before the check-in: the last one advances the round, which clears the
            // round's error.
            std::exception_ptr error;
            if (!n.cancel_requested) error = s.broken ? s.team_error(n.round) : s.round_error();
            if (s.broken)
                s.leave(n);
            else
                s.check_in(n, wakers);
            finish(s);
            if (n.cancel_requested) return PollDropped;
            return PollError(std::move(error));
        }

        n.waker = ctx.getWaker();
        return PollPending;
    }

    // Checks the signature and arguments, then registers and starts the work.
    void begin(TeamState& s, TeamLock& lock, TeamWakers& wakers) {
        TeamNode&     n   = m_node;
        TeamSignature sig = m_algo->signature();
        sig.tag           = n.tag;
        if (s.signature_set && !(sig == s.signature)) {
            s.fail_usage(n.rank, n.round, s.signature_rank,
                         "this call doesn't match the round's collective, types, op, "
                         "root, out or tag", wakers);
            return;
        }
        if (const char* why = m_algo->check(s.size, n.rank)) {
            s.fail_usage(n.rank, n.round, kTeamNoRank, why, wakers);
            return;
        }
        if (!s.signature_set) {
            s.signature_set  = true;
            s.signature      = sig;
            s.signature_rank = n.rank;
        }
        n.begun            = true;
        n.algo             = &*m_algo;
        s.members[n.rank]  = &n;
        ++s.arrived;
        m_algo->begin(n, s, lock, wakers);
    }

    void finish(TeamState& s) noexcept {
        TeamNode& n = m_node;
        n.finished = true;
        n.waker.reset();
        s.next_round.remove(&n);
        if (n.counted) s.slots[n.rank].settled = n.round + 1;
    }

    TeamNode            m_node;
    std::optional<Algo> m_algo;
    std::exception_ptr  m_prepare_error;
};

// ---------------------------------------------------------------------------
// Algorithms
// ---------------------------------------------------------------------------

/// `sync()`: the last arrival completes the round.
struct TeamSyncAlgo {
    using Output = void;

    TeamSignature signature() const noexcept { return {}; }
    const char*   check(std::size_t, std::size_t) const noexcept { return nullptr; }
    void          prepare(std::size_t, std::size_t) {}
    void begin(TeamNode& self, TeamState& s, TeamLock&, TeamWakers& wakers) {
        if (s.arrived == s.size) s.complete_success(self, wakers);
    }
    void resume(TeamNode&, TeamState&, TeamLock&, TeamWakers&) {}
};

enum class TeamTreeShape : std::uint8_t { AllReduce, Reduce, Scan };

/**
 * all_reduce, reduce and the scans: values combined on the team's fixed tree as they
 * become ready. See doc/design/team.md, "Combining" and "Scans".
 *
 * Node (l, r), r a multiple of 2^l, covers ranks [r, min(r + 2^l, k)); its total lives
 * in rank r's `value`. A scan keeps each node's left total in the `saved` of the rank
 * where its right half starts.
 */
template<typename T, typename Op, TeamTreeShape Shape>
class TeamTreeAlgo {
public:
    using Output = std::conditional_t<Shape == TeamTreeShape::Reduce, std::optional<T>, T>;

    static constexpr bool kCheap = team_op_is_cheap<T, Op>;

    TeamTreeAlgo(TeamCollective kind, T value, Op op, std::size_t root = kTeamNoRank,
                 std::optional<T> init = std::nullopt)
        : m_kind(kind), m_value(std::move(value)), m_init(std::move(init))
        , m_op(std::move(op)), m_root(root) {}

    TeamSignature signature() const noexcept {
        TeamSignature sig;
        sig.kind  = m_kind;
        sig.type  = &team_type_tag<T>;
        sig.op    = &team_type_tag<Op>;
        sig.cheap = kCheap;
        sig.root  = m_root;
        return sig;
    }

    const char* check(std::size_t k, std::size_t) const noexcept {
        if (Shape == TeamTreeShape::Reduce && m_root >= k) return "root out of range";
        return nullptr;
    }

    void prepare(std::size_t, std::size_t) {}

    void begin(TeamNode& self, TeamState& s, TeamLock& lock, TeamWakers& wakers) {
        std::size_t cur = self.rank;
        s.avail[cur] = 0;
        s.normalize(cur);
        for (;;) {
            const int level = s.avail[cur];
            if (level == s.height) {   // cur is 0: the root's total is in
                complete(self, s, lock, wakers);
                return;
            }
            const std::size_t half = std::size_t{1} << level;
            const std::size_t p    = cur & ~half;
            const std::size_t q    = p + half;
            if (s.avail[p] != level || s.avail[q] != level) return;   // suspend: rule 2

            TeamNode&     left_node  = *s.members[p];
            TeamNode&     right_node = *s.members[q];
            TeamTreeAlgo& left       = team_peer<TeamTreeAlgo>(s, p);
            TeamTreeAlgo& right      = team_peer<TeamTreeAlgo>(s, q);
            if constexpr (!kCheap) {
                s.avail[p] = s.avail[q] = -1;   // claimed
                s.claim(self, left_node);
                s.claim(self, right_node);
                lock.unlock();
            }
            // RACE: with an expensive op, left's and right's values are used outside
            // the lock. Their claims keep both futures from finishing, and no other
            // member touches a claimed value.
            bool misused = false;
            std::exception_ptr error = team_call_op(misused, [&] { combine(left, right); });
            if constexpr (!kCheap) lock.lock();
            if (misused)
                s.fail_usage(self.rank, self.round, kTeamNoRank,
                             "is_broken() was called from inside an op", wakers);
            else if (error)
                s.break_round_error(std::move(error), wakers);
            if constexpr (!kCheap) {
                // After the break, so the release wakes owners left with no claims.
                s.release(self, left_node, wakers);
                s.release(self, right_node, wakers);
            }
            if (s.broken || s.round_broken) return;
            s.avail[q] = -1;   // used up
            s.avail[p] = level + 1;
            s.normalize(p);
            cur = p;
        }
    }

    void resume(TeamNode&, TeamState&, TeamLock&, TeamWakers&) {}

    Output take(std::size_t rank) {
        if constexpr (Shape == TeamTreeShape::AllReduce) {
            return std::move(*m_value);
        } else if constexpr (Shape == TeamTreeShape::Reduce) {
            if (rank != m_root) return std::nullopt;
            return std::optional<T>(std::move(*m_value));
        } else {
            return std::move(*m_result);
        }
    }

private:
    void combine(TeamTreeAlgo& left, TeamTreeAlgo& right) {
        if constexpr (Shape == TeamTreeShape::Scan) {
            T total(std::invoke(m_op, std::as_const(*left.m_value), std::move(*right.m_value)));
            right.m_saved.emplace(std::move(*left.m_value));
            left.m_value.emplace(std::move(total));
        } else {
            left.m_value.emplace(std::invoke(m_op, std::move(*left.m_value), std::move(*right.m_value)));
        }
    }

    // The root's total is in rank 0's value; hands out the results under the lock.
    void complete(TeamNode& self, TeamState& s, TeamLock& lock, TeamWakers& wakers) {
        TeamTreeAlgo& first = team_peer<TeamTreeAlgo>(s, 0);
        if constexpr (Shape == TeamTreeShape::Scan) {
            if (!down_sweep(self, s, lock, wakers)) return;
        } else {
            std::exception_ptr error;
            try {
                if constexpr (Shape == TeamTreeShape::AllReduce) {
                    // k - 1 copies: rank 0 keeps the original.
                    for (std::size_t r = 1; r < s.size; ++r)
                        team_peer<TeamTreeAlgo>(s, r).m_value.emplace(std::as_const(*first.m_value));
                } else if (m_root != 0) {
                    team_peer<TeamTreeAlgo>(s, m_root).m_value.emplace(std::move(*first.m_value));
                }
            } catch (...) {
                error = std::current_exception();
            }
            if (error) {
                s.break_round_error(std::move(error), wakers);
                return;
            }
        }
        s.complete_success(self, wakers);
    }

    // Gives each rank its prefix. With an expensive op, outside the lock, holding a
    // claim on every other member's future. Returns false if the round broke.
    bool down_sweep(TeamNode& self, TeamState& s, TeamLock& lock, TeamWakers& wakers) {
        if constexpr (!kCheap) {
            for (TeamNode* node : s.members) s.claim(self, *node);
            lock.unlock();
        }
        // With an expensive op, the round may break meanwhile: checked under the lock
        // before each combine.
        auto keep_going = [&]() -> bool {
            if constexpr (kCheap) {
                return true;
            } else {
                lock.lock();
                const bool ok = !s.broken && !s.round_broken;
                lock.unlock();
                return ok;
            }
        };
        bool misused = false;
        std::exception_ptr error = team_call_op(misused, [&] {
            auto at = [&](std::size_t r) -> TeamTreeAlgo& { return team_peer<TeamTreeAlgo>(s, r); };
            // The prefix of the whole tree: rank 0's init, or nothing.
            if (m_kind == TeamCollective::ExclusiveScan)
                at(0).m_result.emplace(std::move(*at(0).m_init));
            else
                at(0).m_result.reset();
            // Top-down, so a left child's prefix (its parent's, in the same rank) is
            // final before its level is reached.
            for (int level = s.height; level >= 1; --level) {
                const std::size_t step = std::size_t{1} << level;
                const std::size_t half = step >> 1;
                for (std::size_t p = 0; p + half < s.size; p += step) {
                    if (!keep_going()) return;
                    TeamTreeAlgo& left  = at(p);
                    TeamTreeAlgo& right = at(p + half);
                    if (left.m_result)
                        right.m_result.emplace(std::invoke(m_op, std::as_const(*left.m_result),
                                                           std::move(*right.m_saved)));
                    else
                        right.m_result.emplace(std::move(*right.m_saved));
                }
            }
            // Rank r's inclusive result is rank r + 1's exclusive prefix; the last
            // rank's is the total.
            if (m_kind == TeamCollective::InclusiveScan) {
                for (std::size_t r = 0; r + 1 < s.size; ++r)
                    at(r).m_result.emplace(std::move(*at(r + 1).m_result));
                at(s.size - 1).m_result.emplace(std::move(*at(0).m_value));
            }
        });
        if constexpr (!kCheap) lock.lock();
        if (misused)
            s.fail_usage(self.rank, self.round, kTeamNoRank,
                         "is_broken() was called from inside an op", wakers);
        else if (error)
            s.break_round_error(std::move(error), wakers);
        if constexpr (!kCheap) {
            for (TeamNode* node : s.members) s.release(self, *node, wakers);
        }
        return !s.broken && !s.round_broken;
    }

    TeamCollective   m_kind;
    std::optional<T> m_value;    ///< The contribution, then a subtree's total, then the result.
    std::optional<T> m_saved;    ///< Scan: the left total of the node split at this rank.
    std::optional<T> m_result;   ///< Scan.
    std::optional<T> m_init;     ///< Exclusive scan; rank 0's is used.
    Op               m_op;
    std::size_t      m_root;
};

/// broadcast, gather and scatter: the last arrival moves or copies the values.
template<typename T, TeamCollective Kind>
class TeamRootedAlgo {
public:
    using Output = std::conditional_t<Kind == TeamCollective::Gather,
                                      std::optional<std::vector<T>>, T>;
    using Input  = std::conditional_t<Kind == TeamCollective::Scatter, std::vector<T>, T>;

    TeamRootedAlgo(Input input, std::size_t root) : m_root(root) {
        if constexpr (Kind == TeamCollective::Scatter)
            m_values = std::move(input);
        else
            m_value.emplace(std::move(input));
    }

    TeamSignature signature() const noexcept {
        TeamSignature sig;
        sig.kind = Kind;
        sig.type = &team_type_tag<T>;
        sig.root = m_root;
        return sig;
    }

    const char* check(std::size_t k, std::size_t rank) const noexcept {
        if (m_root >= k) return "root out of range";
        if constexpr (Kind == TeamCollective::Scatter) {
            if (rank == m_root && m_values.size() != k)
                return "scatter: the root must pass size() values";
            if (rank != m_root && !m_values.empty())
                return "scatter: only the root passes values";
        }
        return nullptr;
    }

    void prepare(std::size_t k, std::size_t rank) {
        if constexpr (Kind == TeamCollective::Gather) {
            if (rank == m_root) {
                m_gathered.emplace();
                m_gathered->reserve(k);
            }
        }
    }

    void begin(TeamNode& self, TeamState& s, TeamLock&, TeamWakers& wakers) {
        if (s.arrived < s.size) return;
        auto at = [&](std::size_t r) -> TeamRootedAlgo& { return team_peer<TeamRootedAlgo>(s, r); };
        TeamRootedAlgo& root = at(m_root);
        std::exception_ptr error;
        try {
            for (std::size_t r = 0; r < s.size; ++r) {
                if constexpr (Kind == TeamCollective::Broadcast) {
                    if (r != m_root) at(r).m_value.emplace(std::as_const(*root.m_value));
                } else if constexpr (Kind == TeamCollective::Gather) {
                    root.m_gathered->push_back(std::move(*at(r).m_value));
                } else {
                    at(r).m_value.emplace(std::move(root.m_values[r]));
                }
            }
        } catch (...) {
            error = std::current_exception();
        }
        if (error) {
            s.break_round_error(std::move(error), wakers);
            return;
        }
        s.complete_success(self, wakers);
    }

    void resume(TeamNode&, TeamState&, TeamLock&, TeamWakers&) {}

    Output take(std::size_t rank) {
        if constexpr (Kind == TeamCollective::Gather) {
            if (rank != m_root) return std::nullopt;
            return std::move(m_gathered);
        } else {
            return std::move(*m_value);
        }
    }

private:
    std::optional<T>              m_value;
    std::vector<T>                m_values;     ///< Scatter.
    std::optional<std::vector<T>> m_gathered;   ///< Gather, at the root.
    std::size_t                   m_root;
};

/// all_gather: the last arrival copies every value into every other member's output.
template<typename T, bool IntoVector>
class TeamAllGatherAlgo {
public:
    using Output = std::conditional_t<IntoVector, std::vector<T>, void>;

    explicit TeamAllGatherAlgo(T value, std::span<T> out = {})
        : m_value(std::move(value)), m_out(out) {}

    TeamSignature signature() const noexcept {
        TeamSignature sig;
        sig.kind = IntoVector ? TeamCollective::AllGatherVector : TeamCollective::AllGather;
        sig.type = &team_type_tag<T>;
        return sig;
    }

    const char* check(std::size_t k, std::size_t) const noexcept {
        if (!IntoVector && m_out.size() != k) return "all_gather: out must have size() elements";
        return nullptr;
    }

    void prepare(std::size_t k, std::size_t) {
        if constexpr (IntoVector) m_vector.reserve(k);
    }

    void begin(TeamNode& self, TeamState& s, TeamLock&, TeamWakers& wakers) {
        if (s.arrived < s.size) return;
        auto at = [&](std::size_t r) -> TeamAllGatherAlgo& { return team_peer<TeamAllGatherAlgo>(s, r); };
        std::exception_ptr error;
        try {
            // Member m's own value is moved last of all its uses: members before m copy
            // it from m's value, members after m from m's output.
            for (std::size_t m = 0; m < s.size; ++m) {
                TeamAllGatherAlgo& dst = at(m);
                for (std::size_t r = 0; r < s.size; ++r) {
                    TeamAllGatherAlgo& src = at(r);
                    if (r == m)
                        dst.put(r, std::move(*src.m_value));
                    else if (r < m)
                        dst.put(r, std::as_const(src.slot(r)));
                    else
                        dst.put(r, std::as_const(*src.m_value));
                }
            }
        } catch (...) {
            error = std::current_exception();
        }
        if (error) {
            s.break_round_error(std::move(error), wakers);
            return;
        }
        s.complete_success(self, wakers);
    }

    void resume(TeamNode&, TeamState&, TeamLock&, TeamWakers&) {}

    Output take(std::size_t) {
        if constexpr (IntoVector) return std::move(m_vector);
    }

private:
    template<typename V>
    void put(std::size_t r, V&& v) {
        if constexpr (IntoVector)
            m_vector.push_back(std::forward<V>(v));
        else
            m_out[r] = std::forward<V>(v);
    }

    T& slot(std::size_t r) {
        if constexpr (IntoVector)
            return m_vector[r];
        else
            return m_out[r];
    }

    std::optional<T> m_value;
    std::span<T>     m_out;
    std::vector<T>   m_vector;
};

/**
 * all_reduce_elementwise. Phase 1: every member registers its `in`. Phase 2: each
 * combines its range of `out` across every member's `in`, outside the lock, holding a
 * claim on every other member's future. See doc/design/team.md, "Elementwise".
 */
template<typename T, typename Op>
class TeamElementwiseAlgo {
public:
    using Output = void;

    TeamElementwiseAlgo(std::span<const T> in, std::span<T> out, Op op)
        : m_in(in), m_out(out), m_op(std::move(op)) {}

    TeamSignature signature() const noexcept {
        TeamSignature sig;
        sig.kind     = TeamCollective::Elementwise;
        sig.type     = &team_type_tag<T>;
        sig.op       = &team_type_tag<Op>;
        sig.cheap    = team_op_is_cheap<T, Op>;
        sig.out      = m_out.data();
        sig.out_size = m_out.size();
        return sig;
    }

    const char* check(std::size_t, std::size_t) const noexcept {
        if (m_in.size() != m_out.size())
            return "all_reduce_elementwise: in and out differ in length";
        const std::less<const T*> before;
        const T* in_end  = m_in.data() + m_in.size();
        const T* out_end = m_out.data() + m_out.size();
        if (!m_in.empty() && before(m_in.data(), out_end) && before(m_out.data(), in_end))
            return "all_reduce_elementwise: in overlaps out";
        return nullptr;
    }

    void prepare(std::size_t, std::size_t) {}

    void begin(TeamNode& self, TeamState& s, TeamLock& lock, TeamWakers& wakers) {
        if (s.arrived < s.size) return;
        s.phase2 = true;
        for (TeamNode* node : s.members)
            if (node != &self) wakers.add(std::move(node->waker));
        run_slice(self, s, lock, wakers);
    }

    void resume(TeamNode& self, TeamState& s, TeamLock& lock, TeamWakers& wakers) {
        if (s.phase2 && !m_slice_done) run_slice(self, s, lock, wakers);
    }

private:
    void run_slice(TeamNode& self, TeamState& s, TeamLock& lock, TeamWakers& wakers) {
        for (TeamNode* node : s.members) s.claim(self, *node);
        lock.unlock();
        // RACE: the other members' `in` and s.members are read outside the lock. The
        // claims keep every other future registered and its `in` unchanged; members
        // write only their own disjoint range of `out`.
        // PERF: each element walks the tree through s.members. A slice could gather
        // the k input pointers once, if traces show the walk.
        bool misused = false;
        std::exception_ptr error = team_call_op(misused, [&] {
            const auto [begin, end] = team_range(self.rank, s.size, m_out.size());
            for (std::size_t i = begin; i < end; ++i) {
                if (s.size == 1)
                    m_out[i] = input(s, 0, i);
                else
                    m_out[i] = eval(s, s.height, 0, i);
            }
        });
        lock.lock();
        if (misused)
            s.fail_usage(self.rank, self.round, kTeamNoRank,
                         "is_broken() was called from inside an op", wakers);
        else if (error)
            s.break_round_error(std::move(error), wakers);
        for (TeamNode* node : s.members) s.release(self, *node, wakers);
        if (s.broken || s.round_broken) return;
        m_slice_done = true;
        if (++s.slices_done == s.size) s.complete_success(self, wakers);
    }

    const T& input(TeamState& s, std::size_t rank, std::size_t i) const {
        return team_peer<TeamElementwiseAlgo>(s, rank).m_in[i];
    }

    // Node (level, r) for element i; it covers at least two ranks. A leaf is passed as
    // the input element itself, so nothing is copied.
    T eval(TeamState& s, int level, std::size_t r, std::size_t i) {
        std::size_t half = std::size_t{1} << (level - 1);
        while (r + half >= s.size) {   // no right child: passed up unchanged
            --level;
            half >>= 1;
        }
        const std::size_t q = r + half;
        // The left child covers `half` ranks; the right one min(half, k - q).
        if (half == 1)
            return T(std::invoke(m_op, input(s, r, i), input(s, q, i)));
        if (q + 1 == s.size)
            return T(std::invoke(m_op, eval(s, level - 1, r, i), input(s, q, i)));
        return T(std::invoke(m_op, eval(s, level - 1, r, i), eval(s, level - 1, q, i)));
    }

    std::span<const T> m_in;
    std::span<T>       m_out;
    Op                 m_op;
    bool               m_slice_done = false;   ///< GUARDED BY the team's mutex.
};

} // namespace detail

// ---------------------------------------------------------------------------
// The futures, by collective
// ---------------------------------------------------------------------------

using TeamSyncFuture = detail::TeamFuture<detail::TeamSyncAlgo>;

template<typename T, typename Op>
using TeamAllReduceFuture =
    detail::TeamFuture<detail::TeamTreeAlgo<T, Op, detail::TeamTreeShape::AllReduce>>;

template<typename T, typename Op>
using TeamReduceFuture =
    detail::TeamFuture<detail::TeamTreeAlgo<T, Op, detail::TeamTreeShape::Reduce>>;

/// exclusive_scan and inclusive_scan; mixing the two in a round is a mismatch.
template<typename T, typename Op>
using TeamScanFuture =
    detail::TeamFuture<detail::TeamTreeAlgo<T, Op, detail::TeamTreeShape::Scan>>;

template<typename T>
using TeamBroadcastFuture =
    detail::TeamFuture<detail::TeamRootedAlgo<T, detail::TeamCollective::Broadcast>>;

template<typename T>
using TeamGatherFuture =
    detail::TeamFuture<detail::TeamRootedAlgo<T, detail::TeamCollective::Gather>>;

template<typename T>
using TeamScatterFuture =
    detail::TeamFuture<detail::TeamRootedAlgo<T, detail::TeamCollective::Scatter>>;

template<typename T>
using TeamAllGatherFuture = detail::TeamFuture<detail::TeamAllGatherAlgo<T, false>>;

template<typename T>
using TeamAllGatherVectorFuture = detail::TeamFuture<detail::TeamAllGatherAlgo<T, true>>;

template<typename T, typename Op>
using TeamElementwiseFuture = detail::TeamFuture<detail::TeamElementwiseAlgo<T, Op>>;

// ---------------------------------------------------------------------------
// TeamMember
// ---------------------------------------------------------------------------

/**
 * @brief One member of a team made by `make_team()`: its rank, and the collectives it
 * runs with the others.
 *
 * Move-only. Pass it by value into the task that uses it: its destructor leaves the
 * team, which is how the others find out that the task has ended. Every member calls
 * the same collectives in the same order, each awaited before the next.
 *
 * That is the supported pattern. Others, such as a member stored where it outlives its
 * task or passed on by reference, aren't prevented, but the lifetime rules they rely
 * on are then the caller's to keep: the team catches the misuse it can, not all of it.
 * A moved-from member can only be destroyed or assigned to; anything else throws or
 * fails with TeamUsageError.
 *
 * ```cpp
 * coro::Coro<void> part(coro::TeamMember me, Table& table) {
 *     const auto [begin, end] = me.range(table.rows());
 *     const std::size_t total = co_await me.all_reduce(count(table, begin, end));
 *     ...
 * }
 * ```
 *
 * See doc/design/team.md.
 */
class TeamMember {
public:
    TeamMember(TeamMember&& other) noexcept
        : m_state(std::move(other.m_state))
        , m_rank(other.m_rank)
        , m_size(other.m_size)
        , m_rounds(other.m_rounds) {
        other.reset();
    }

    /// Leaves this handle's team first, as the destructor does.
    TeamMember& operator=(TeamMember&& other) noexcept {
        if (this != &other) {
            leave();
            m_state  = std::move(other.m_state);
            m_rank   = other.m_rank;
            m_size   = other.m_size;
            m_rounds = other.m_rounds;
            other.reset();
        }
        return *this;
    }

    TeamMember(const TeamMember&)            = delete;
    TeamMember& operator=(const TeamMember&) = delete;

    /// Leaves the team, which breaks it: rounds already complete keep their results,
    /// every other round fails with TeamBroken. Does nothing on a moved-from handle.
    ~TeamMember() { leave(); }

    // rank(), size(), range() and is_broken() throw TeamUsageError on a moved-from
    // handle.

    /// 0 .. size() - 1.
    std::size_t rank() const {
        check_held("rank() called on a moved-from TeamMember");
        return m_rank;
    }

    /// The number of members, k.
    std::size_t size() const {
        check_held("size() called on a moved-from TeamMember");
        return m_size;
    }

    /// @brief [begin, end) of this member's even share of `n` items. The first `n % k`
    /// ranks get one item more; with `n < k` the last ranks get an empty range.
    std::pair<std::size_t, std::size_t> range(std::size_t n) const {
        check_held("range() called on a moved-from TeamMember");
        return detail::team_range(m_rank, m_size, n);
    }

    /// @brief True once the team is broken for good (a member has left, or a check
    /// failed). A broken round doesn't count.
    ///
    /// Must not be called from inside an op: it returns true, and the team breaks
    /// with TeamUsageError once the op returns.
    bool is_broken() const {
        if (detail::t_team_in_op) {
            detail::t_team_op_misused = true;
            return true;
        }
        check_held("is_broken() called on a moved-from TeamMember");
        std::lock_guard lock(m_state->mutex);
        return m_state->broken;
    }

    // --- Collectives ------------------------------------------------------------
    // Constructing the future joins this member to the next round. Each is
    // Cancellable, and has `tag(i)` for the round's signature. Reductions and scans
    // default op to std::plus<>.

    /// @brief Waits until every member has called sync() as many times as this one.
    [[nodiscard]] TeamSyncFuture sync() { return start(detail::TeamSyncAlgo{}); }

    /// @brief v0 ⊕ ... ⊕ v(k-1), to every member.
    template<typename T, typename Op = std::plus<>>
        requires std::copy_constructible<T> && detail::TeamReduceOp<Op, T>
    [[nodiscard]] TeamAllReduceFuture<T, Op> all_reduce(T value, Op op = {}) {
        using Algo = detail::TeamTreeAlgo<T, Op, detail::TeamTreeShape::AllReduce>;
        return start(Algo(detail::TeamCollective::AllReduce, std::move(value), std::move(op)));
    }

    /// @brief The value passed by rank `root`, to every member.
    template<typename T>
        requires std::copy_constructible<T>
    [[nodiscard]] TeamBroadcastFuture<T> broadcast(T value, std::size_t root) {
        using Algo = detail::TeamRootedAlgo<T, detail::TeamCollective::Broadcast>;
        return start(Algo(std::move(value), root));
    }

    /// @brief Every member's value, in rank order, written to `out` (size() elements).
    template<typename T>
        requires std::copy_constructible<T> && std::is_copy_assignable_v<T>
    [[nodiscard]] TeamAllGatherFuture<T> all_gather(T value, std::span<T> out) {
        return start(detail::TeamAllGatherAlgo<T, false>(std::move(value), out));
    }

    /// @brief Every member's value, in rank order, into a new vector.
    template<typename T>
        requires std::copy_constructible<T>
    [[nodiscard]] TeamAllGatherVectorFuture<T> all_gather(T value) {
        return start(detail::TeamAllGatherAlgo<T, true>(std::move(value)));
    }

    /// @brief True if any member passed true.
    [[nodiscard]] TeamAllReduceFuture<bool, std::logical_or<>> any(bool value) {
        return all_reduce(value, std::logical_or<>{});
    }

    /// @brief True if every member passed true.
    [[nodiscard]] TeamAllReduceFuture<bool, std::logical_and<>> all(bool value) {
        return all_reduce(value, std::logical_and<>{});
    }

    /// @brief v0 ⊕ ... ⊕ v(k-1) at rank `root`; std::nullopt at the others.
    template<typename T, typename Op = std::plus<>>
        requires std::move_constructible<T> && detail::TeamReduceOp<Op, T>
    [[nodiscard]] TeamReduceFuture<T, Op> reduce(T value, std::size_t root, Op op = {}) {
        using Algo = detail::TeamTreeAlgo<T, Op, detail::TeamTreeShape::Reduce>;
        return start(Algo(detail::TeamCollective::Reduce, std::move(value), std::move(op), root));
    }

    /// @brief Every member's value, in rank order, at rank `root`; std::nullopt at the
    /// others.
    template<typename T>
        requires std::move_constructible<T>
    [[nodiscard]] TeamGatherFuture<T> gather(T value, std::size_t root) {
        using Algo = detail::TeamRootedAlgo<T, detail::TeamCollective::Gather>;
        return start(Algo(std::move(value), root));
    }

    /// @brief Rank r gets values[r] from rank `root`, which passes size() values. The
    /// others pass an empty vector.
    template<typename T>
        requires std::move_constructible<T>
    [[nodiscard]] TeamScatterFuture<T> scatter(std::vector<T> values, std::size_t root) {
        using Algo = detail::TeamRootedAlgo<T, detail::TeamCollective::Scatter>;
        return start(Algo(std::move(values), root));
    }

    /// @brief Rank r gets init ⊕ v0 ⊕ ... ⊕ v(r-1); rank 0 gets init. Rank 0's init
    /// is used.
    template<typename T, typename Op = std::plus<>>
        requires std::move_constructible<T> && detail::TeamScanOp<Op, T>
    [[nodiscard]] TeamScanFuture<T, Op> exclusive_scan(T value, T init, Op op = {}) {
        using Algo = detail::TeamTreeAlgo<T, Op, detail::TeamTreeShape::Scan>;
        return start(Algo(detail::TeamCollective::ExclusiveScan, std::move(value), std::move(op),
                          detail::kTeamNoRank, std::optional<T>(std::move(init))));
    }

    /// @brief Rank r gets v0 ⊕ ... ⊕ vr.
    template<typename T, typename Op = std::plus<>>
        requires std::move_constructible<T> && detail::TeamScanOp<Op, T>
    [[nodiscard]] TeamScanFuture<T, Op> inclusive_scan(T value, Op op = {}) {
        using Algo = detail::TeamTreeAlgo<T, Op, detail::TeamTreeShape::Scan>;
        return start(Algo(detail::TeamCollective::InclusiveScan, std::move(value), std::move(op)));
    }

    /// @brief out[i] = in0[i] ⊕ ... ⊕ in(k-1)[i]. Every member passes the same `out`
    /// and an `in` of the same length that doesn't overlap it; each combines its
    /// range(out.size()). Resolves when all of `out` is written and no member reads
    /// `in` any more.
    template<typename T, typename Op = std::plus<>>
        requires std::is_copy_assignable_v<T> && detail::TeamElementwiseOp<Op, T>
    [[nodiscard]] TeamElementwiseFuture<T, Op> all_reduce_elementwise(
        std::span<const T> in, std::span<T> out, Op op = {}) {
        return start(detail::TeamElementwiseAlgo<T, Op>(in, out, std::move(op)));
    }

private:
    friend std::vector<TeamMember> make_team(std::size_t k);

    TeamMember(detail::Rc<detail::TeamState> state, std::size_t rank, std::size_t size) noexcept
        : m_state(std::move(state)), m_rank(rank), m_size(size) {}

    // The constructor of a future takes the round number and nothing else. On a
    // moved-from handle the future fails at its first poll.
    template<typename Algo>
    detail::TeamFuture<Algo> start(Algo algo) {
        const std::uint64_t round = m_state ? m_rounds++ : 0;
        return detail::TeamFuture<Algo>(m_state, m_rank, round, std::move(algo));
    }

    void check_held(const char* why) const {
        if (!m_state) throw TeamUsageError(why, 0, detail::kTeamNoRank);
    }

    // Leaves nothing of the old rank behind for a moved-from handle to report.
    void reset() noexcept {
        m_rank   = detail::kTeamNoRank;
        m_size   = 0;
        m_rounds = 0;
    }

    void leave() noexcept {
        if (!m_state) return;
        detail::TeamWakers wakers;
        {
            std::lock_guard lock(m_state->mutex);
            m_state->break_team(m_rank, m_rounds, wakers);
        }
        wakers.wake_all();
        m_state = nullptr;
    }

    detail::Rc<detail::TeamState> m_state;   ///< Null when moved from.
    std::size_t                   m_rank   = detail::kTeamNoRank;
    std::size_t                   m_size   = 0;
    std::uint64_t                 m_rounds = 0;   ///< Rounds this member has joined.
};

/// @brief Creates a team of `k` members, ranks 0 .. k-1, one handle per rank.
/// Throws std::invalid_argument if `k` is 0.
[[nodiscard]] inline std::vector<TeamMember> make_team(std::size_t k) {
    if (k == 0) throw std::invalid_argument("coro::make_team() needs k >= 1");
    auto state = detail::make_rc<detail::TeamState>(k);
    std::vector<TeamMember> team;
    team.reserve(k);
    for (std::size_t r = 0; r < k; ++r) team.push_back(TeamMember(state, r, k));
    return team;
}

} // namespace coro
