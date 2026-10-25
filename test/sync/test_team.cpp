#include <gtest/gtest.h>
#include "executor_traits.h"
#include <coro/sync/team.h>
#include <coro/sync/select.h>
#include <coro/sync/sleep.h>
#include <coro/sync/timeout.h>
#include <coro/task/join_set.h>
#include <coro/task/yield_now.h>
#include <coro/runtime/runtime.h>
#include <coro/coro.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#ifndef CORO_PICO
#include <chrono>
#include <thread>
#endif

using namespace coro;

static_assert(Future<TeamSyncFuture>);
static_assert(Cancellable<TeamSyncFuture>);
static_assert(Cancellable<TeamAllReduceFuture<int, std::plus<>>>);
static_assert(Cancellable<TeamElementwiseFuture<float, std::plus<>>>);

namespace {

// Counts wake() calls. Used to poll futures by hand, without a runtime.
class CountingWaker : public detail::Waker {
public:
    void wake() override { ++wakes; }
    detail::Rc<detail::Waker> clone() override { return detail::make_rc<CountingWaker>(); }
    int wakes = 0;
};

template<typename Fut>
using Outcome = std::optional<PollResult<typename Fut::OutputType>>;

// A team whose members are polled by hand, each with its own waker.
struct Hand {
    explicit Hand(std::size_t k) : team(make_team(k)) {
        for (std::size_t r = 0; r < k; ++r) wakers.push_back(detail::make_rc<CountingWaker>());
    }

    std::size_t size() const { return team.size(); }

    template<typename Fut>
    PollResult<typename Fut::OutputType> poll(std::size_t r, Fut& f) {
        detail::Context ctx(wakers[r]);
        return f.poll(ctx);
    }

    // Polls a future and stores what it finished with.
    template<typename Fut>
    bool step(std::size_t r, Fut& f, Outcome<Fut>& out) {
        auto result = poll(r, f);
        if (result.isPending()) return false;
        out.emplace(std::move(result));
        return true;
    }

    int wakes(std::size_t r) const { return wakers[r]->wakes; }

    std::vector<TeamMember>                    team;
    std::vector<detail::Rc<CountingWaker>>     wakers;
};

// One future per member, made by make(member, rank). Not resized after: the futures
// can't move once polled.
template<typename Make>
auto make_round(Hand& h, Make make) {
    using Fut = decltype(make(h.team[0], std::size_t{0}));
    std::vector<Fut> fs;
    fs.reserve(h.size());
    for (std::size_t r = 0; r < h.size(); ++r) fs.push_back(make(h.team[r], r));
    return fs;
}

// Polls each future in `order`, then each woken one again, until none is woken.
template<typename Fut>
std::vector<Outcome<Fut>> run_round(Hand& h, std::vector<Fut>& fs,
                                    const std::vector<std::size_t>& order) {
    std::vector<Outcome<Fut>> out(fs.size());
    std::vector<int>          seen(fs.size());
    auto poll_one = [&](std::size_t r) {
        seen[r] = h.wakes(r);
        h.step(r, fs[r], out[r]);
    };
    for (std::size_t r : order) poll_one(r);
    for (bool progress = true; progress;) {
        progress = false;
        for (std::size_t r : order) {
            if (!out[r] && h.wakes(r) > seen[r]) {
                poll_one(r);
                progress = true;
            }
        }
    }
    return out;
}

std::vector<std::size_t> forward(std::size_t k) {
    std::vector<std::size_t> order(k);
    for (std::size_t r = 0; r < k; ++r) order[r] = r;
    return order;
}

// Forward, reverse and two shuffles.
std::vector<std::vector<std::size_t>> orders(std::size_t k) {
    std::vector<std::vector<std::size_t>> all;
    all.push_back(forward(k));
    all.push_back(forward(k));
    std::reverse(all.back().begin(), all.back().end());
    for (unsigned seed : {1u, 2u}) {
        all.push_back(forward(k));
        std::mt19937 rng(seed);
        std::shuffle(all.back().begin(), all.back().end(), rng);
    }
    return all;
}

// "ready", "dropped", "pending", the team error's name, or what() of another error.
template<typename R>
std::string outcome(const std::optional<PollResult<R>>& r) {
    if (!r) return "pending";
    if (r->isDropped()) return "dropped";
    if (r->isReady()) return "ready";
    try {
        std::rethrow_exception(r->error());
    } catch (const TeamUsageError&) {
        return "TeamUsageError";
    } catch (const TeamRoundBroken&) {
        return "TeamRoundBroken";
    } catch (const TeamBroken&) {
        return "TeamBroken";
    } catch (const std::exception& e) {
        return e.what();
    }
}

template<typename E, typename R>
std::optional<E> error_as(const std::optional<PollResult<R>>& r) {
    if (!r || !r->isError()) return std::nullopt;
    try {
        std::rethrow_exception(r->error());
    } catch (const E& e) {
        return e;
    } catch (...) {
        return std::nullopt;
    }
}

// The team's fixed tree, evaluated serially: node (l, r) combines (l-1, r) with
// (l-1, r + 2^(l-1)) when that exists.
template<typename T, typename Op>
T tree_reduce(std::vector<T> v, Op op) {
    for (std::size_t step = 1; step < v.size(); step *= 2)
        for (std::size_t r = 0; r + step < v.size(); r += 2 * step)
            v[r] = op(std::move(v[r]), std::move(v[r + step]));
    return std::move(v[0]);
}

std::string letter(std::size_t r) { return std::string(1, static_cast<char>('a' + r)); }

std::string letters(std::size_t begin, std::size_t end) {
    std::string s;
    for (std::size_t r = begin; r < end; ++r) s += letter(r);
    return s;
}

// Counts its copies (construction and assignment); moves are free.
struct Counted {
    static inline int copies = 0;
    int v = 0;
    Counted() = default;
    explicit Counted(int value) : v(value) {}
    Counted(const Counted& o) : v(o.v) { ++copies; }
    Counted(Counted&&) noexcept = default;
    Counted& operator=(const Counted& o) { v = o.v; ++copies; return *this; }
    Counted& operator=(Counted&&) noexcept = default;
};

struct AddCounted {
    Counted operator()(Counted&& a, Counted&& b) const { a.v += b.v; return std::move(a); }
};

struct ScanAddCounted {
    Counted operator()(const Counted& a, Counted&& b) const { b.v += a.v; return std::move(b); }
};

struct AddPtr {
    std::unique_ptr<int> operator()(std::unique_ptr<int> a, std::unique_ptr<int> b) const {
        *a += *b;
        return a;
    }
};

struct ScanAddPtr {
    std::unique_ptr<int> operator()(const std::unique_ptr<int>& a, std::unique_ptr<int> b) const {
        *b += *a;
        return b;
    }
};

// Calls `hook` from inside the combine, outside the lock when wrapped in expensive_op.
struct HookOp {
    static inline std::function<void()> hook;
    int operator()(int a, int b) const {
        if (hook) {
            auto h = std::move(hook);
            hook = nullptr;
            h();
        }
        return a + b;
    }
};

// Throws when it combines ranks 2 and 3 (values 2 and 3): a node below the root.
struct ThrowAt23 {
    int operator()(int a, int b) const {
        if (a == 2 && b == 3) throw std::runtime_error("boom");
        return a + b;
    }
};

struct ReadyIntFuture {
    using OutputType = int;
    PollResult<int> poll(detail::Context&) { return PollResult<int>(1); }
};

struct LvalueOnly {
    int operator()(int&, int&) const { return 0; }
};
struct ReturnsFuture {
    ReadyIntFuture operator()(int, int) const { return {}; }
};

template<typename T, typename Op>
concept CanAllReduce = requires(TeamMember& m, T v, Op op) { m.all_reduce(std::move(v), std::move(op)); };
template<typename T, typename Op>
concept CanReduce = requires(TeamMember& m, T v, Op op) { m.reduce(std::move(v), 0, std::move(op)); };
template<typename T, typename Op>
concept CanScan = requires(TeamMember& m, T v, Op op) { m.inclusive_scan(std::move(v), std::move(op)); };
template<typename F>
concept CanWrapCheap = requires(F f) { cheap_op(std::move(f)); };

struct Pod64 { char b[64]; };
struct Pod65 { char b[65]; };

}  // namespace

// ---------------------------------------------------------------------------
// Compile-time checks
// ---------------------------------------------------------------------------

// The default mode: cheap for small trivially copyable types.
static_assert(detail::team_op_is_cheap<int, std::plus<>>);
static_assert(detail::team_op_is_cheap<Pod64, std::plus<>>);
static_assert(!detail::team_op_is_cheap<Pod65, std::plus<>>);
static_assert(!detail::team_op_is_cheap<std::array<float, 4096>, std::plus<>>);
static_assert(!detail::team_op_is_cheap<std::string, std::plus<>>);
static_assert(detail::team_op_is_cheap<std::string, CheapOp<std::plus<>>>);
static_assert(!detail::team_op_is_cheap<int, ExpensiveOp<std::plus<>>>);

// What must not compile.
static_assert(CanAllReduce<int, std::plus<>>);
static_assert(!CanAllReduce<int, LvalueOnly>);
static_assert(!CanAllReduce<int, ReturnsFuture>);
static_assert(!CanAllReduce<std::unique_ptr<int>, AddPtr>);   // needs a copyable T
static_assert(CanReduce<std::unique_ptr<int>, AddPtr>);
static_assert(CanScan<std::unique_ptr<int>, ScanAddPtr>);
static_assert(!CanScan<std::unique_ptr<int>, AddPtr>);        // left operand is const T&
static_assert(CanWrapCheap<std::plus<>>);
static_assert(!CanWrapCheap<ExpensiveOp<std::plus<>>>);
static_assert(!CanWrapCheap<CheapOp<std::plus<>>>);

// ---------------------------------------------------------------------------
// make_team()
// ---------------------------------------------------------------------------

TEST(TeamTest, MakeTeamGivesEachRankOnce) {
    for (std::size_t k : {1u, 2u, 3u, 5u, 8u}) {
        auto team = make_team(k);
        ASSERT_EQ(team.size(), k);
        for (std::size_t r = 0; r < k; ++r) {
            EXPECT_EQ(team[r].rank(), r);
            EXPECT_EQ(team[r].size(), k);
            EXPECT_FALSE(team[r].is_broken());
        }
    }
    EXPECT_THROW((void)make_team(0), std::invalid_argument);
}

TEST(TeamTest, RangeCoversEveryItemOnce) {
    for (std::size_t k : {1u, 2u, 3u, 4u, 7u}) {
        const auto team = make_team(k);
        for (std::size_t n : {0u, 1u, 2u, 3u, 5u, 10u, 64u, 101u}) {
            std::size_t next = 0;
            for (std::size_t r = 0; r < k; ++r) {
                const auto [begin, end] = team[r].range(n);
                EXPECT_EQ(begin, next) << "k=" << k << " n=" << n << " r=" << r;
                EXPECT_EQ(end - begin, n / k + (r < n % k ? 1 : 0));
                next = end;
            }
            EXPECT_EQ(next, n);
        }
    }
}

// ---------------------------------------------------------------------------
// Results, polled by hand
// ---------------------------------------------------------------------------

TEST(TeamTest, SizeOneIsReadyAtOnce) {
    Hand h(1);
    auto& m = h.team[0];
    {
        auto f = m.sync();
        EXPECT_TRUE(h.poll(0, f).isReady());
    }
    {
        auto f = m.all_reduce(std::string("a"));
        EXPECT_EQ(h.poll(0, f).value(), "a");
    }
    {
        auto f = m.broadcast(7, 0);
        EXPECT_EQ(h.poll(0, f).value(), 7);
    }
    {
        auto f = m.all_gather(std::string("x"));
        EXPECT_EQ(h.poll(0, f).value(), std::vector<std::string>{"x"});
    }
    {
        auto f = m.reduce(3, 0);
        EXPECT_EQ(h.poll(0, f).value(), std::optional<int>(3));
    }
    {
        auto f = m.gather(4, 0);
        EXPECT_EQ(h.poll(0, f).value(), std::optional<std::vector<int>>(std::vector<int>{4}));
    }
    {
        auto f = m.scatter(std::vector<int>{9}, 0);
        EXPECT_EQ(h.poll(0, f).value(), 9);
    }
    {
        auto f = m.exclusive_scan(5, 100);
        EXPECT_EQ(h.poll(0, f).value(), 100);
    }
    {
        auto f = m.inclusive_scan(5);
        EXPECT_EQ(h.poll(0, f).value(), 5);
    }
    {
        std::vector<int> in{1, 2, 3}, out(3);
        auto f = m.all_reduce_elementwise(std::span<const int>(in), std::span<int>(out));
        EXPECT_TRUE(h.poll(0, f).isReady());
        EXPECT_EQ(out, in);
    }
    EXPECT_EQ(h.wakes(0), 0);
}

// String concatenation isn't commutative: the results prove the order.
TEST(TeamTest, AllReduceConcatenatesInRankOrder) {
    for (std::size_t k : {2u, 3u, 5u, 6u, 7u, 8u}) {
        for (const auto& order : orders(k)) {
            for (int mode = 0; mode < 3; ++mode) {
                Hand h(k);
                std::vector<std::string> results(k);
                auto check = [&](auto fs) {
                    auto out = run_round(h, fs, order);
                    for (std::size_t r = 0; r < k; ++r) {
                        ASSERT_EQ(outcome(out[r]), "ready") << "k=" << k << " r=" << r;
                        EXPECT_EQ(out[r]->value(), letters(0, k)) << "k=" << k << " mode=" << mode;
                    }
                };
                if (mode == 0)
                    check(make_round(h, [](TeamMember& m, std::size_t r) { return m.all_reduce(letter(r)); }));
                else if (mode == 1)
                    check(make_round(h, [](TeamMember& m, std::size_t r) {
                        return m.all_reduce(letter(r), cheap_op(std::plus<>{}));
                    }));
                else
                    check(make_round(h, [](TeamMember& m, std::size_t r) {
                        return m.all_reduce(letter(r), expensive_op(std::plus<>{}));
                    }));
            }
        }
    }
}

TEST(TeamTest, ReduceBroadcastGatherScatterFromEveryRoot) {
    constexpr std::size_t k = 5;
    for (std::size_t root = 0; root < k; ++root) {
        {
            Hand h(k);
            auto fs  = make_round(h, [&](TeamMember& m, std::size_t r) { return m.reduce(letter(r), root); });
            auto out = run_round(h, fs, forward(k));
            for (std::size_t r = 0; r < k; ++r) {
                ASSERT_EQ(outcome(out[r]), "ready");
                if (r == root)
                    EXPECT_EQ(out[r]->value(), std::optional<std::string>(letters(0, k)));
                else
                    EXPECT_FALSE(out[r]->value().has_value());
            }
        }
        {
            Hand h(k);
            auto fs = make_round(h, [&](TeamMember& m, std::size_t r) {
                return m.broadcast(r == root ? std::string("root") : std::string("other"), root);
            });
            auto out = run_round(h, fs, orders(k)[3]);
            for (std::size_t r = 0; r < k; ++r) EXPECT_EQ(out[r]->value(), "root");
        }
        {
            Hand h(k);
            auto fs  = make_round(h, [&](TeamMember& m, std::size_t r) { return m.gather(letter(r), root); });
            auto out = run_round(h, fs, orders(k)[1]);
            for (std::size_t r = 0; r < k; ++r) {
                if (r != root) {
                    EXPECT_FALSE(out[r]->value().has_value());
                    continue;
                }
                const auto& all = *out[r]->value();
                ASSERT_EQ(all.size(), k);
                for (std::size_t i = 0; i < k; ++i) EXPECT_EQ(all[i], letter(i));
            }
        }
        {
            Hand h(k);
            auto fs = make_round(h, [&](TeamMember& m, std::size_t r) {
                std::vector<std::string> values;
                if (r == root)
                    for (std::size_t i = 0; i < k; ++i) values.push_back(letter(i));
                return m.scatter(std::move(values), root);
            });
            auto out = run_round(h, fs, orders(k)[2]);
            for (std::size_t r = 0; r < k; ++r) EXPECT_EQ(out[r]->value(), letter(r));
        }
    }
}

TEST(TeamTest, ScansGivePrefixesInRankOrder) {
    for (std::size_t k : {2u, 3u, 5u, 7u, 8u}) {
        for (const auto& order : orders(k)) {
            {
                Hand h(k);
                auto fs = make_round(h, [](TeamMember& m, std::size_t r) {
                    return m.exclusive_scan(letter(r), r == 0 ? std::string("init:") : std::string("ignored"));
                });
                auto out = run_round(h, fs, order);
                for (std::size_t r = 0; r < k; ++r)
                    EXPECT_EQ(out[r]->value(), "init:" + letters(0, r)) << "k=" << k << " r=" << r;
            }
            {
                Hand h(k);
                auto fs = make_round(h, [](TeamMember& m, std::size_t r) {
                    return m.inclusive_scan(letter(r), expensive_op(std::plus<>{}));
                });
                auto out = run_round(h, fs, order);
                for (std::size_t r = 0; r < k; ++r)
                    EXPECT_EQ(out[r]->value(), letters(0, r + 1)) << "k=" << k << " r=" << r;
            }
            {
                Hand h(k);
                auto fs  = make_round(h, [](TeamMember& m, std::size_t r) { return m.inclusive_scan(int(r + 1)); });
                auto out = run_round(h, fs, order);
                for (std::size_t r = 0; r < k; ++r)
                    EXPECT_EQ(out[r]->value(), int((r + 1) * (r + 2) / 2));
            }
        }
    }
}

TEST(TeamTest, AllGatherIntoSpanAndVector) {
    constexpr std::size_t k = 4;
    for (const auto& order : orders(k)) {
        {
            Hand h(k);
            std::vector<std::vector<std::string>> outs(k, std::vector<std::string>(k));
            auto fs = make_round(h, [&](TeamMember& m, std::size_t r) {
                return m.all_gather(letter(r), std::span<std::string>(outs[r]));
            });
            auto out = run_round(h, fs, order);
            for (std::size_t r = 0; r < k; ++r) {
                ASSERT_EQ(outcome(out[r]), "ready");
                for (std::size_t i = 0; i < k; ++i) EXPECT_EQ(outs[r][i], letter(i));
            }
        }
        {
            Hand h(k);
            auto fs  = make_round(h, [](TeamMember& m, std::size_t r) { return m.all_gather(letter(r)); });
            auto out = run_round(h, fs, order);
            for (std::size_t r = 0; r < k; ++r) {
                const auto& all = out[r]->value();
                ASSERT_EQ(all.size(), k);
                for (std::size_t i = 0; i < k; ++i) EXPECT_EQ(all[i], letter(i));
            }
        }
    }
}

TEST(TeamTest, AllReduceElementwise) {
    for (std::size_t k : {2u, 3u, 5u}) {
        for (std::size_t n : {std::size_t{1}, k, std::size_t{11}}) {
            for (const auto& order : orders(k)) {
                Hand h(k);
                std::vector<std::vector<std::string>> ins(k);
                for (std::size_t r = 0; r < k; ++r)
                    for (std::size_t i = 0; i < n; ++i) ins[r].push_back(letter(r) + std::to_string(i));
                std::vector<std::string> out(n);
                auto fs = make_round(h, [&](TeamMember& m, std::size_t r) {
                    return m.all_reduce_elementwise(std::span<const std::string>(ins[r]),
                                                    std::span<std::string>(out));
                });
                auto res = run_round(h, fs, order);
                for (std::size_t r = 0; r < k; ++r) ASSERT_EQ(outcome(res[r]), "ready");
                // Done reading `in`: reusing it changes nothing.
                for (auto& in : ins) in.assign(n, "reused");
                for (std::size_t i = 0; i < n; ++i) {
                    std::string expected;
                    for (std::size_t r = 0; r < k; ++r) expected += letter(r) + std::to_string(i);
                    EXPECT_EQ(out[i], expected) << "k=" << k << " n=" << n << " i=" << i;
                }
            }
        }
    }
}

// Values whose sum depends on the grouping.
TEST(TeamTest, FloatingPointIsDeterministic) {
    const std::vector<double> values{1e16, 1.0, -1e16, 1.0, 3.0, 1e-3, -2.5, 7.0};
    for (std::size_t k : {3u, 5u, 8u}) {
        const std::vector<double> mine(values.begin(), values.begin() + k);
        const double expected = tree_reduce(mine, std::plus<>{});
        std::optional<std::vector<double>> first_scan;
        for (const auto& order : orders(k)) {
            for (bool expensive : {false, true}) {
                Hand h(k);
                if (expensive) {
                    auto fs = make_round(h, [&](TeamMember& m, std::size_t r) {
                        return m.all_reduce(mine[r], expensive_op(std::plus<>{}));
                    });
                    auto out = run_round(h, fs, order);
                    for (std::size_t r = 0; r < k; ++r) EXPECT_EQ(out[r]->value(), expected);
                } else {
                    auto fs  = make_round(h, [&](TeamMember& m, std::size_t r) { return m.all_reduce(mine[r]); });
                    auto out = run_round(h, fs, order);
                    for (std::size_t r = 0; r < k; ++r) EXPECT_EQ(out[r]->value(), expected);
                }
                Hand hs(k);
                std::vector<double> scan(k);
                if (expensive) {
                    auto fs = make_round(hs, [&](TeamMember& m, std::size_t r) {
                        return m.inclusive_scan(mine[r], expensive_op(std::plus<>{}));
                    });
                    auto out = run_round(hs, fs, order);
                    for (std::size_t r = 0; r < k; ++r) scan[r] = out[r]->value();
                } else {
                    auto fs  = make_round(hs, [&](TeamMember& m, std::size_t r) { return m.inclusive_scan(mine[r]); });
                    auto out = run_round(hs, fs, order);
                    for (std::size_t r = 0; r < k; ++r) scan[r] = out[r]->value();
                }
                if (!first_scan) first_scan = scan;
                EXPECT_EQ(scan, *first_scan) << "k=" << k;
                EXPECT_EQ(scan[k - 1], expected);
            }
        }
    }
}

TEST(TeamTest, DefaultOpIsPlus) {
    Hand h(3);
    auto fs  = make_round(h, [](TeamMember& m, std::size_t r) { return m.reduce(int(r + 1), 2); });
    auto out = run_round(h, fs, forward(3));
    EXPECT_EQ(out[2]->value(), std::optional<int>(6));
}

TEST(TeamTest, PlusIntAgainstDefaultIsUsageError) {
    Hand h(2);
    auto f0 = h.team[0].all_reduce(1);
    auto f1 = h.team[1].all_reduce(1, std::plus<int>{});
    Outcome<decltype(f0)> o0;
    Outcome<decltype(f1)> o1;
    h.step(0, f0, o0);
    h.step(1, f1, o1);
    if (!o0) h.step(0, f0, o0);
    EXPECT_EQ(outcome(o0), "TeamUsageError");
    EXPECT_EQ(outcome(o1), "TeamUsageError");
}

TEST(TeamTest, CopiesMatchTheTable) {
    constexpr std::size_t k = 4;
    for (bool expensive : {false, true}) {
        auto expect_copies = [&](auto make, int expected, const char* what) {
            Hand h(k);
            auto fs = make_round(h, make);
            Counted::copies = 0;
            auto out = run_round(h, fs, orders(k)[2]);
            for (std::size_t r = 0; r < k; ++r) ASSERT_EQ(outcome(out[r]), "ready") << what;
            EXPECT_EQ(Counted::copies, expected) << what << (expensive ? " expensive" : " cheap");
        };
        if (expensive) {
            expect_copies([](TeamMember& m, std::size_t r) { return m.all_reduce(Counted(int(r)), expensive_op(AddCounted{})); },
                          int(k - 1), "all_reduce");
            expect_copies([](TeamMember& m, std::size_t r) { return m.reduce(Counted(int(r)), 1, expensive_op(AddCounted{})); },
                          0, "reduce");
            expect_copies([](TeamMember& m, std::size_t r) { return m.inclusive_scan(Counted(int(r)), expensive_op(ScanAddCounted{})); },
                          0, "inclusive_scan");
            expect_copies([](TeamMember& m, std::size_t r) {
                              return m.exclusive_scan(Counted(int(r)), Counted(0), expensive_op(ScanAddCounted{}));
                          },
                          0, "exclusive_scan");
        } else {
            expect_copies([](TeamMember& m, std::size_t r) { return m.all_reduce(Counted(int(r)), cheap_op(AddCounted{})); },
                          int(k - 1), "all_reduce");
            expect_copies([](TeamMember& m, std::size_t r) { return m.reduce(Counted(int(r)), 1, cheap_op(AddCounted{})); },
                          0, "reduce");
            expect_copies([](TeamMember& m, std::size_t r) { return m.inclusive_scan(Counted(int(r)), cheap_op(ScanAddCounted{})); },
                          0, "inclusive_scan");
            expect_copies([](TeamMember& m, std::size_t r) {
                              return m.exclusive_scan(Counted(int(r)), Counted(0), cheap_op(ScanAddCounted{}));
                          },
                          0, "exclusive_scan");
        }
    }
    auto expect_copies = [&](auto make, int expected, const char* what) {
        Hand h(k);
        auto fs = make_round(h, make);
        Counted::copies = 0;
        auto out = run_round(h, fs, orders(k)[3]);
        for (std::size_t r = 0; r < k; ++r) ASSERT_EQ(outcome(out[r]), "ready") << what;
        EXPECT_EQ(Counted::copies, expected) << what;
    };
    expect_copies([](TeamMember& m, std::size_t r) { return m.broadcast(Counted(int(r)), 2); }, int(k - 1), "broadcast");
    expect_copies([](TeamMember& m, std::size_t r) { return m.all_gather(Counted(int(r))); }, int(k * (k - 1)),
                  "all_gather");
    expect_copies([](TeamMember& m, std::size_t r) { return m.gather(Counted(int(r)), 1); }, 0, "gather");
    expect_copies([](TeamMember& m, std::size_t r) {
                      std::vector<Counted> values;
                      if (r == 3)
                          for (std::size_t i = 0; i < k; ++i) values.emplace_back(int(i));
                      return m.scatter(std::move(values), 3);
                  },
                  0, "scatter");
}

// Every member but the one that completes the round is woken exactly once.
TEST(TeamTest, OneWakePerWaitingMember) {
    for (std::size_t k : {2u, 3u, 5u, 8u}) {
        for (const auto& order : orders(k)) {
            for (bool expensive : {false, true}) {
                Hand h(k);
                if (expensive) {
                    auto fs = make_round(h, [](TeamMember& m, std::size_t r) {
                        return m.all_reduce(int(r), expensive_op(std::plus<>{}));
                    });
                    run_round(h, fs, order);
                } else {
                    auto fs = make_round(h, [](TeamMember& m, std::size_t r) { return m.exclusive_scan(int(r), 0); });
                    run_round(h, fs, order);
                }
                int total = 0;
                for (std::size_t r = 0; r < k; ++r) {
                    EXPECT_LE(h.wakes(r), 1) << "k=" << k << " r=" << r;
                    total += h.wakes(r);
                }
                EXPECT_EQ(total, int(k - 1)) << "k=" << k;
            }
        }
    }
}

TEST(TeamTest, MoveOnlyValues) {
    constexpr std::size_t k = 5;
    auto ptr = [](std::size_t r) { return std::make_unique<int>(int(r + 1)); };
    {
        Hand h(k);
        auto fs  = make_round(h, [&](TeamMember& m, std::size_t r) { return m.reduce(ptr(r), 4, AddPtr{}); });
        auto out = run_round(h, fs, orders(k)[2]);
        EXPECT_EQ(**out[4]->value(), 15);
    }
    {
        Hand h(k);
        auto fs  = make_round(h, [&](TeamMember& m, std::size_t r) { return m.gather(ptr(r), 0); });
        auto out = run_round(h, fs, forward(k));
        const auto& all = *out[0]->value();
        for (std::size_t i = 0; i < k; ++i) EXPECT_EQ(*all[i], int(i + 1));
    }
    {
        Hand h(k);
        auto fs = make_round(h, [&](TeamMember& m, std::size_t r) {
            std::vector<std::unique_ptr<int>> values;
            if (r == 1)
                for (std::size_t i = 0; i < k; ++i) values.push_back(ptr(i));
            return m.scatter(std::move(values), 1);
        });
        auto out = run_round(h, fs, orders(k)[1]);
        for (std::size_t r = 0; r < k; ++r) EXPECT_EQ(*out[r]->value(), int(r + 1));
    }
    {
        Hand h(k);
        auto fs  = make_round(h, [&](TeamMember& m, std::size_t r) { return m.inclusive_scan(ptr(r), ScanAddPtr{}); });
        auto out = run_round(h, fs, orders(k)[3]);
        for (std::size_t r = 0; r < k; ++r) EXPECT_EQ(*out[r]->value(), int((r + 1) * (r + 2) / 2));
    }
    {
        Hand h(k);
        auto fs = make_round(h, [&](TeamMember& m, std::size_t) {
            return m.exclusive_scan(std::make_unique<int>(1), std::make_unique<int>(10), ScanAddPtr{});
        });
        auto out = run_round(h, fs, forward(k));
        for (std::size_t r = 0; r < k; ++r) EXPECT_EQ(*out[r]->value(), int(10 + r));
    }
}

TEST(TeamTest, SeveralCollectivesInARow) {
    constexpr std::size_t k = 6;
    Hand h(k);
    const auto all = orders(k);
    for (std::size_t round = 0; round < 12; ++round) {
        const auto& order = all[round % all.size()];
        if (round % 3 == 0) {
            auto fs  = make_round(h, [](TeamMember& m, std::size_t) { return m.sync(); });
            auto out = run_round(h, fs, order);
            for (std::size_t r = 0; r < k; ++r) EXPECT_EQ(outcome(out[r]), "ready");
        } else if (round % 3 == 1) {
            auto fs  = make_round(h, [&](TeamMember& m, std::size_t r) { return m.all_reduce(int(r * round)); });
            auto out = run_round(h, fs, order);
            for (std::size_t r = 0; r < k; ++r) EXPECT_EQ(out[r]->value(), int(15 * round));
        } else {
            auto fs  = make_round(h, [&](TeamMember& m, std::size_t r) { return m.any(r == round % k); });
            auto out = run_round(h, fs, order);
            for (std::size_t r = 0; r < k; ++r) EXPECT_TRUE(out[r]->value());
        }
    }
}

// ---------------------------------------------------------------------------
// Breaking the team
// ---------------------------------------------------------------------------

TEST(TeamTest, MemberThatLeavesBreaksTheTeam) {
    Hand h(3);
    auto f0 = h.team[0].all_reduce(1);
    Outcome<decltype(f0)> o0;
    h.step(0, f0, o0);
    { TeamMember gone = std::move(h.team[2]); }
    EXPECT_EQ(h.wakes(0), 1);
    h.step(0, f0, o0);
    auto e = error_as<TeamBroken>(o0);
    ASSERT_TRUE(e);
    EXPECT_EQ(e->rank(), 2u);
    EXPECT_EQ(e->rounds_joined(), 0u);
    EXPECT_EQ(e->round(), 0u);
    EXPECT_TRUE(h.team[1].is_broken());

    auto f1 = h.team[1].all_reduce(1);
    Outcome<decltype(f1)> o1;
    h.step(1, f1, o1);
    EXPECT_EQ(outcome(o1), "TeamBroken");
}

TEST(TeamTest, EveryMemberReturnsAfterTheLastRound) {
    for (const auto& order : orders(4)) {
        Hand h(4);
        auto fs  = make_round(h, [](TeamMember& m, std::size_t r) { return m.all_reduce(int(r)); });
        auto out = run_round(h, fs, order);
        // Leave in this order; every result is already in.
        for (std::size_t r : order) { TeamMember gone = std::move(h.team[r]); }
        for (std::size_t r = 0; r < 4; ++r) EXPECT_EQ(out[r]->value(), 6);
    }
}

TEST(TeamTest, LoopOneRoundShortGivesTeamBroken) {
    Hand h(2);
    for (int i = 0; i < 2; ++i) {
        auto fs  = make_round(h, [](TeamMember& m, std::size_t) { return m.all_reduce(1); });
        auto out = run_round(h, fs, forward(2));
        EXPECT_EQ(out[0]->value(), 2);
    }
    auto f0 = h.team[0].all_reduce(1);
    Outcome<decltype(f0)> o0;
    h.step(0, f0, o0);
    { TeamMember gone = std::move(h.team[1]); }
    h.step(0, f0, o0);
    auto e = error_as<TeamBroken>(o0);
    ASSERT_TRUE(e);
    EXPECT_EQ(e->rank(), 1u);
    // Rounds count from 0: a loop one short has joined round() rounds.
    EXPECT_EQ(e->round(), 2u);
    EXPECT_EQ(e->rounds_joined(), e->round());
}

TEST(TeamTest, LeavingRightAfterSuccessKeepsResults) {
    Hand h(3);
    auto fs = make_round(h, [](TeamMember& m, std::size_t r) { return m.all_reduce(int(r)); });
    std::vector<Outcome<std::remove_reference_t<decltype(fs[0])>>> out(3);
    h.step(0, fs[0], out[0]);
    h.step(1, fs[1], out[1]);
    ASSERT_TRUE(h.step(2, fs[2], out[2]));
    { TeamMember gone = std::move(h.team[2]); }
    h.step(0, fs[0], out[0]);
    h.step(1, fs[1], out[1]);
    for (std::size_t r = 0; r < 3; ++r) EXPECT_EQ(out[r]->value(), 3);

    auto g = h.team[0].sync();
    Outcome<decltype(g)> og;
    h.step(0, g, og);
    EXPECT_EQ(outcome(og), "TeamBroken");
}

TEST(TeamTest, HandleDestroyedMidRound) {
    Hand h(4);
    auto fs = make_round(h, [](TeamMember& m, std::size_t r) {
        return m.all_reduce(int(r), expensive_op(std::plus<>{}));
    });
    std::vector<Outcome<std::remove_reference_t<decltype(fs[0])>>> out(4);
    h.step(0, fs[0], out[0]);
    h.step(1, fs[1], out[1]);   // combines (0, 1)
    h.step(2, fs[2], out[2]);
    EXPECT_FALSE(out[0] || out[1] || out[2]);
    { TeamMember gone = std::move(h.team[3]); }   // fs[3] is never polled
    for (std::size_t r = 0; r < 3; ++r) h.step(r, fs[r], out[r]);
    for (std::size_t r = 0; r < 3; ++r) EXPECT_EQ(outcome(out[r]), "TeamBroken") << r;
}

TEST(TeamTest, HandleNeverGivenOutIsCaughtAtTheFirstCollective) {
    Hand h(3);
    h.team.pop_back();
    auto f = h.team[0].sync();
    Outcome<decltype(f)> o;
    ASSERT_TRUE(h.step(0, f, o));
    auto e = error_as<TeamBroken>(o);
    ASSERT_TRUE(e);
    EXPECT_EQ(e->rank(), 2u);
}

TEST(TeamTest, MovedFromHandle) {
    Hand h(2);
    // Destroying a moved-from handle breaks nothing.
    TeamMember tmp = std::move(h.team[1]);
    h.team[1]      = std::move(tmp);
    // A collective on a moved-from handle fails at once, without breaking the team.
    TeamMember keep = std::move(h.team[1]);
    auto bad = h.team[1].sync();
    Outcome<decltype(bad)> ob;
    ASSERT_TRUE(h.step(1, bad, ob));
    auto e = error_as<TeamUsageError>(ob);
    ASSERT_TRUE(e);
    EXPECT_EQ(e->rank(), SIZE_MAX);
    EXPECT_FALSE(h.team[0].is_broken());

    auto f0 = h.team[0].all_reduce(1);
    auto f1 = keep.all_reduce(2);
    Outcome<decltype(f0)> o0, o1;
    h.step(0, f0, o0);
    h.step(1, f1, o1);
    h.step(0, f0, o0);
    EXPECT_EQ(o0->value(), 3);
    EXPECT_EQ(o1->value(), 3);
}

TEST(TeamTest, MovedFromHandleAccessorsThrow) {
    std::vector<TeamMember> team = make_team(3);
    TeamMember keep = std::move(team[2]);
    // Every query throws, rather than answer with the rank the handle used to have.
    EXPECT_THROW((void)team[2].rank(), TeamUsageError);
    EXPECT_THROW((void)team[2].size(), TeamUsageError);
    EXPECT_THROW((void)team[2].range(10), TeamUsageError);
    EXPECT_THROW((void)team[2].is_broken(), TeamUsageError);
    try {
        (void)team[2].rank();
    } catch (const TeamUsageError& e) {
        EXPECT_EQ(e.rank(), SIZE_MAX);
        EXPECT_EQ(std::string(e.what()),
                  "coro::TeamUsageError: rank() called on a moved-from TeamMember");
    }
    // Nothing broke, and the handle that took over answers for rank 2.
    EXPECT_FALSE(team[0].is_broken());
    EXPECT_EQ(keep.rank(), 2u);
    EXPECT_EQ(keep.size(), 3u);
}

TEST(TeamTest, MovedFromHandleAssignedAgainWorks) {
    Hand h(2);
    TeamMember tmp = std::move(h.team[1]);
    EXPECT_THROW((void)h.team[1].rank(), TeamUsageError);
    // Assigning a member to the moved-from handle makes it usable again, rounds and all.
    h.team[1] = std::move(tmp);
    EXPECT_EQ(h.team[1].rank(), 1u);
    EXPECT_EQ(h.team[1].size(), 2u);
    EXPECT_EQ(h.team[1].range(4), (std::pair<std::size_t, std::size_t>{2, 4}));
    EXPECT_FALSE(h.team[1].is_broken());
    EXPECT_THROW((void)tmp.rank(), TeamUsageError);

    auto f0 = h.team[0].all_reduce(1);
    auto f1 = h.team[1].all_reduce(2);
    Outcome<decltype(f0)> o0, o1;
    h.step(0, f0, o0);
    h.step(1, f1, o1);
    h.step(0, f0, o0);
    EXPECT_EQ(o0->value(), 3);
    EXPECT_EQ(o1->value(), 3);
}

// ---------------------------------------------------------------------------
// Breaking a round
// ---------------------------------------------------------------------------

TEST(TeamTest, CancelWhileWaitingBreaksTheRound) {
    Hand h(3);
    auto fs = make_round(h, [](TeamMember& m, std::size_t r) { return m.all_reduce(int(r)); });
    std::vector<Outcome<std::remove_reference_t<decltype(fs[0])>>> out(3);
    h.step(0, fs[0], out[0]);
    h.step(1, fs[1], out[1]);
    fs[1].cancel();
    EXPECT_EQ(h.wakes(0), 1);
    h.step(1, fs[1], out[1]);
    EXPECT_EQ(outcome(out[1]), "dropped");
    h.step(0, fs[0], out[0]);
    // Arriving after the cancel: fails at once.
    ASSERT_TRUE(h.step(2, fs[2], out[2]));
    for (std::size_t r : {0u, 2u}) {
        auto e = error_as<TeamRoundBroken>(out[r]);
        ASSERT_TRUE(e) << r;
        EXPECT_EQ(e->rank(), 1u);
    }
    auto next = make_round(h, [](TeamMember& m, std::size_t r) { return m.all_reduce(int(r)); });
    auto res  = run_round(h, next, orders(3)[1]);
    for (std::size_t r = 0; r < 3; ++r) EXPECT_EQ(res[r]->value(), 3);
}

TEST(TeamTest, CancelBeforeFirstPollIsStillAccountedFor) {
    Hand h(3);
    auto fs = make_round(h, [](TeamMember& m, std::size_t r) { return m.all_reduce(int(r)); });
    std::vector<Outcome<std::remove_reference_t<decltype(fs[0])>>> out(3);
    h.step(0, fs[0], out[0]);
    fs[1].cancel();
    ASSERT_TRUE(h.step(1, fs[1], out[1]));
    EXPECT_EQ(outcome(out[1]), "dropped");
    h.step(0, fs[0], out[0]);
    h.step(2, fs[2], out[2]);
    EXPECT_EQ(outcome(out[0]), "TeamRoundBroken");
    EXPECT_EQ(outcome(out[2]), "TeamRoundBroken");
    auto next = make_round(h, [](TeamMember& m, std::size_t r) { return m.all_reduce(int(r)); });
    auto res  = run_round(h, next, forward(3));
    for (std::size_t r = 0; r < 3; ++r) EXPECT_EQ(res[r]->value(), 3);
}

// Member 1 cancels every round. The next round can't begin until every member has
// checked in to the broken one, so no member gets ahead.
TEST(TeamTest, SeveralBrokenRoundsInARow) {
    Hand h(3);
    for (int round = 0; round < 4; ++round) {
        auto fs = make_round(h, [](TeamMember& m, std::size_t r) { return m.all_reduce(int(r)); });
        std::vector<Outcome<std::remove_reference_t<decltype(fs[0])>>> out(3);
        h.step(0, fs[0], out[0]);
        fs[1].cancel();
        h.step(1, fs[1], out[1]);
        EXPECT_EQ(outcome(out[1]), "dropped");
        h.step(0, fs[0], out[0]);
        EXPECT_EQ(outcome(out[0]), "TeamRoundBroken");
        // Members 0 and 1 are on to the next round; member 2 hasn't checked in yet.
        auto ahead = h.team[0].all_reduce(0);
        Outcome<decltype(ahead)> oa;
        EXPECT_FALSE(h.step(0, ahead, oa));
        h.step(2, fs[2], out[2]);
        EXPECT_EQ(outcome(out[2]), "TeamRoundBroken");
        EXPECT_GT(h.wakes(0), 0);
        // Finish the round `ahead` joined, then carry on.
        auto rest1 = h.team[1].all_reduce(1);
        auto rest2 = h.team[2].all_reduce(2);
        Outcome<decltype(ahead)> o1, o2;
        h.step(1, rest1, o1);
        h.step(2, rest2, o2);
        h.step(0, ahead, oa);
        if (!o1) h.step(1, rest1, o1);
        if (!o2) h.step(2, rest2, o2);
        EXPECT_EQ(oa->value(), 3);
        EXPECT_EQ(o1->value(), 3);
        EXPECT_EQ(o2->value(), 3);
    }
}

TEST(TeamTest, LateCancelOnlyDiscardsThatResult) {
    Hand h(2);
    auto fs = make_round(h, [](TeamMember& m, std::size_t r) { return m.all_reduce(int(r + 1)); });
    std::vector<Outcome<std::remove_reference_t<decltype(fs[0])>>> out(2);
    h.step(0, fs[0], out[0]);
    ASSERT_TRUE(h.step(1, fs[1], out[1]));   // completes the round
    fs[0].cancel();                           // gets the lock afterwards
    h.step(0, fs[0], out[0]);
    EXPECT_EQ(outcome(out[0]), "dropped");
    EXPECT_EQ(out[1]->value(), 3);

    // Member 0 can't carry on without it: it cancels its next collective.
    auto next = make_round(h, [](TeamMember& m, std::size_t r) { return m.all_reduce(int(r + 1)); });
    std::vector<Outcome<std::remove_reference_t<decltype(next[0])>>> nout(2);
    next[0].cancel();
    h.step(1, next[1], nout[1]);
    h.step(0, next[0], nout[0]);
    h.step(1, next[1], nout[1]);
    EXPECT_EQ(outcome(nout[0]), "dropped");
    auto e = error_as<TeamRoundBroken>(nout[1]);
    ASSERT_TRUE(e);
    EXPECT_EQ(e->rank(), 0u);

    auto last = make_round(h, [](TeamMember& m, std::size_t r) { return m.all_reduce(int(r + 1)); });
    auto res  = run_round(h, last, forward(2));
    EXPECT_EQ(res[0]->value(), 3);
    EXPECT_EQ(res[1]->value(), 3);
}

TEST(TeamTest, CheapCancelDropsOnTheNextPoll) {
    Hand h(2);
    auto f0 = h.team[0].all_reduce(1);
    Outcome<decltype(f0)> o0, o1;
    h.step(0, f0, o0);
    f0.cancel();
    ASSERT_TRUE(h.step(0, f0, o0));
    EXPECT_EQ(outcome(o0), "dropped");
    auto f1 = h.team[1].all_reduce(1);
    ASSERT_TRUE(h.step(1, f1, o1));
    EXPECT_EQ(outcome(o1), "TeamRoundBroken");
}

TEST(TeamTest, FutureDroppedUnpolledBreaksItsRound) {
    Hand h(3);
    auto f0 = h.team[0].all_reduce(1);
    Outcome<decltype(f0)> o0, o2;
    h.step(0, f0, o0);
    { auto dropped = h.team[1].all_reduce(1); }   // e.g. an exception before co_await
    h.step(0, f0, o0);
    auto e = error_as<TeamRoundBroken>(o0);
    ASSERT_TRUE(e);
    EXPECT_EQ(e->rank(), 1u);
    auto f2 = h.team[2].all_reduce(1);
    h.step(2, f2, o2);
    EXPECT_EQ(outcome(o2), "TeamRoundBroken");
    auto next = make_round(h, [](TeamMember& m, std::size_t) { return m.all_reduce(1); });
    auto res  = run_round(h, next, forward(3));
    for (std::size_t r = 0; r < 3; ++r) EXPECT_EQ(res[r]->value(), 3);
}

// Dropped unpolled while the round before is broken and not yet complete: the round
// starts broken when it begins.
TEST(TeamTest, FutureDroppedUnpolledBeforeItsRoundBegins) {
    Hand h(3);
    auto fs = make_round(h, [](TeamMember& m, std::size_t) { return m.all_reduce(1); });
    std::vector<Outcome<std::remove_reference_t<decltype(fs[0])>>> out(3);
    h.step(0, fs[0], out[0]);
    fs[1].cancel();
    h.step(1, fs[1], out[1]);
    h.step(0, fs[0], out[0]);
    ASSERT_EQ(outcome(out[0]), "TeamRoundBroken");
    { auto dropped = h.team[1].all_reduce(1); }   // round 1, which can't begin yet
    h.step(2, fs[2], out[2]);                      // round 0 complete
    auto g0 = h.team[0].all_reduce(1);
    auto g2 = h.team[2].all_reduce(1);
    Outcome<decltype(g0)> og0, og2;
    h.step(0, g0, og0);
    h.step(2, g2, og2);
    for (auto* o : {&og0, &og2}) {
        auto e = error_as<TeamRoundBroken>(*o);
        ASSERT_TRUE(e);
        EXPECT_EQ(e->rank(), 1u);
    }
    auto next = make_round(h, [](TeamMember& m, std::size_t) { return m.all_reduce(1); });
    auto res  = run_round(h, next, forward(3));
    for (std::size_t r = 0; r < 3; ++r) EXPECT_EQ(res[r]->value(), 3);
}

TEST(TeamTest, TwoFuturesDroppedUnpolledAheadIsUsageError) {
    Hand h(3);
    auto fs = make_round(h, [](TeamMember& m, std::size_t) { return m.all_reduce(1); });
    std::vector<Outcome<std::remove_reference_t<decltype(fs[0])>>> out(3);
    h.step(0, fs[0], out[0]);
    fs[1].cancel();
    h.step(1, fs[1], out[1]);
    { auto dropped = h.team[1].all_reduce(1); }   // round 1: owed
    { auto dropped = h.team[1].all_reduce(1); }   // round 2: too far ahead
    h.step(0, fs[0], out[0]);
    h.step(2, fs[2], out[2]);
    EXPECT_EQ(outcome(out[0]), "TeamUsageError");
    EXPECT_EQ(outcome(out[2]), "TeamUsageError");
}

TEST(TeamTest, DroppedUnpolledWhileTheLastIsUnfinishedIsUsageError) {
    Hand h(2);
    auto f0 = h.team[0].sync();
    Outcome<decltype(f0)> o0;
    h.step(0, f0, o0);
    { auto dropped = h.team[0].sync(); }
    h.step(0, f0, o0);
    auto e = error_as<TeamUsageError>(o0);
    ASSERT_TRUE(e);
    EXPECT_EQ(e->rank(), 0u);
    EXPECT_EQ(e->round(), 1u);
}

TEST(TeamTest, DestroyedPolledButUnfinishedBreaksTheTeam) {
    Hand h(3);
    auto f0 = h.team[0].all_reduce(1);
    Outcome<decltype(f0)> o0;
    h.step(0, f0, o0);
    {
        auto f1 = h.team[1].all_reduce(1);
        Outcome<decltype(f1)> o1;
        EXPECT_FALSE(h.step(1, f1, o1));
    }
    h.step(0, f0, o0);
    auto e = error_as<TeamUsageError>(o0);
    ASSERT_TRUE(e);
    EXPECT_EQ(e->rank(), 1u);
    EXPECT_TRUE(h.team[2].is_broken());
}

TEST(TeamTest, OpThatThrowsBreaksTheRound) {
    for (bool expensive : {false, true}) {
        for (const auto& order : orders(4)) {
            Hand h(4);
            std::vector<std::string> got(4);
            if (expensive) {
                auto fs  = make_round(h, [](TeamMember& m, std::size_t r) { return m.all_reduce(int(r), expensive_op(ThrowAt23{})); });
                auto out = run_round(h, fs, order);
                for (std::size_t r = 0; r < 4; ++r) got[r] = outcome(out[r]);
            } else {
                auto fs  = make_round(h, [](TeamMember& m, std::size_t r) { return m.all_reduce(int(r), cheap_op(ThrowAt23{})); });
                auto out = run_round(h, fs, order);
                for (std::size_t r = 0; r < 4; ++r) got[r] = outcome(out[r]);
            }
            for (std::size_t r = 0; r < 4; ++r) EXPECT_EQ(got[r], "boom") << r;
            auto next = make_round(h, [](TeamMember& m, std::size_t r) { return m.all_reduce(int(r)); });
            auto res  = run_round(h, next, order);
            for (std::size_t r = 0; r < 4; ++r) EXPECT_EQ(res[r]->value(), 6);
        }
    }
}

// The combine of (0, 1) runs in member 1's poll, outside the lock, holding a claim
// on member 0's value. Member 0 is cancelled from inside it.
TEST(TeamTest, CancelWhileClaimedWaitsForTheRelease) {
    Hand h(2);
    auto fs = make_round(h, [](TeamMember& m, std::size_t r) { return m.all_reduce(int(r), expensive_op(HookOp{})); });
    std::vector<Outcome<std::remove_reference_t<decltype(fs[0])>>> out(2);
    h.step(0, fs[0], out[0]);
    bool pending_while_claimed = false;
    int  wakes_while_claimed   = -1;
    HookOp::hook = [&] {
        fs[0].cancel();
        pending_while_claimed = !h.step(0, fs[0], out[0]);
        wakes_while_claimed   = h.wakes(0);
    };
    h.step(1, fs[1], out[1]);
    EXPECT_TRUE(pending_while_claimed);
    EXPECT_EQ(h.wakes(0), wakes_while_claimed + 1);   // woken by the claimer's release
    auto e = error_as<TeamRoundBroken>(out[1]);
    ASSERT_TRUE(e);
    EXPECT_EQ(e->rank(), 0u);
    h.step(0, fs[0], out[0]);
    EXPECT_EQ(outcome(out[0]), "dropped");
    auto next = make_round(h, [](TeamMember& m, std::size_t) { return m.all_reduce(1); });
    auto res  = run_round(h, next, forward(2));
    EXPECT_EQ(res[0]->value(), 2);
}

// Member 2 cancels while member 1 combines (0, 1): member 0's value is claimed, so it
// learns of the break only from the claimer.
TEST(TeamTest, ClaimedWaiterIsWokenByTheClaimer) {
    Hand h(3);
    auto fs = make_round(h, [](TeamMember& m, std::size_t r) { return m.all_reduce(int(r), expensive_op(HookOp{})); });
    std::vector<Outcome<std::remove_reference_t<decltype(fs[0])>>> out(3);
    h.step(0, fs[0], out[0]);
    h.step(2, fs[2], out[2]);
    int  wakes0_in_combine = -1;
    bool pending0          = false;
    HookOp::hook = [&] {
        fs[2].cancel();
        wakes0_in_combine = h.wakes(0);
        pending0          = !h.step(0, fs[0], out[0]);
    };
    h.step(1, fs[1], out[1]);
    EXPECT_EQ(wakes0_in_combine, 0);
    EXPECT_TRUE(pending0);
    EXPECT_EQ(h.wakes(0), 1);
    h.step(0, fs[0], out[0]);
    auto e = error_as<TeamRoundBroken>(out[0]);
    ASSERT_TRUE(e);
    EXPECT_EQ(e->rank(), 2u);
    h.step(2, fs[2], out[2]);
    EXPECT_EQ(outcome(out[2]), "dropped");
}

// ---------------------------------------------------------------------------
// Checked misuse
// ---------------------------------------------------------------------------

namespace {

// Member 0 begins with f0, member 1 with f1; both must fail with the same error.
template<typename F0, typename F1>
void expect_usage_error(Hand& h, F0& f0, F1& f1, std::size_t failing_rank) {
    Outcome<F0> o0;
    Outcome<F1> o1;
    h.step(0, f0, o0);
    h.step(1, f1, o1);
    if (!o0) h.step(0, f0, o0);
    ASSERT_EQ(outcome(o0), "TeamUsageError");
    ASSERT_EQ(outcome(o1), "TeamUsageError");
    EXPECT_EQ(o0->error(), o1->error());
    auto e = error_as<TeamUsageError>(o0);
    EXPECT_EQ(e->rank(), failing_rank);
    EXPECT_EQ(e->round(), 0u);
    EXPECT_NE(std::string(e->what()).find("round 0"), std::string::npos);
}

}  // namespace

TEST(TeamTest, MismatchedCollectives) {
    {
        Hand h(2);
        auto f0 = h.team[0].sync();
        auto f1 = h.team[1].all_reduce(1);
        expect_usage_error(h, f0, f1, 1);
    }
    {
        Hand h(2);
        auto f0 = h.team[0].all_reduce(1);
        auto f1 = h.team[1].all_reduce(1L);
        expect_usage_error(h, f0, f1, 1);
    }
    {
        Hand h(2);
        auto f0 = h.team[0].all_reduce(1, [](int a, int b) { return a + b; });
        auto f1 = h.team[1].all_reduce(1, [](int a, int b) { return a + b; });
        expect_usage_error(h, f0, f1, 1);
    }
    {
        Hand h(2);
        auto f0 = h.team[0].all_reduce(1);
        auto f1 = h.team[1].all_reduce(1, expensive_op(std::plus<>{}));
        expect_usage_error(h, f0, f1, 1);
    }
    {
        Hand h(2);
        auto f0 = h.team[0].broadcast(1, 0);
        auto f1 = h.team[1].broadcast(1, 1);
        expect_usage_error(h, f0, f1, 1);
    }
    {
        Hand h(2);
        auto f0 = h.team[0].exclusive_scan(1, 0);
        auto f1 = h.team[1].inclusive_scan(1);
        expect_usage_error(h, f0, f1, 1);
    }
    {
        Hand h(2);
        std::vector<int> in(4), out_a(4), out_b(4);
        auto f0 = h.team[0].all_reduce_elementwise(std::span<const int>(in), std::span<int>(out_a));
        auto f1 = h.team[1].all_reduce_elementwise(std::span<const int>(in), std::span<int>(out_b));
        expect_usage_error(h, f0, f1, 1);
    }
}

TEST(TeamTest, MismatchNamesTheSignatureSetter) {
    Hand h(2);
    auto f1 = h.team[1].sync();
    auto f0 = h.team[0].all_reduce(1);
    Outcome<decltype(f1)> o1;
    Outcome<decltype(f0)> o0;
    h.step(1, f1, o1);
    h.step(0, f0, o0);
    auto e = error_as<TeamUsageError>(o0);
    ASSERT_TRUE(e);
    EXPECT_EQ(e->rank(), 0u);
    EXPECT_EQ(e->other_rank(), 1u);
}

TEST(TeamTest, BadArguments) {
    {
        Hand h(2);
        auto f0 = h.team[0].broadcast(1, 5);
        auto f1 = h.team[1].broadcast(1, 5);
        expect_usage_error(h, f0, f1, 0);
    }
    {
        Hand h(2);
        auto f0 = h.team[0].reduce(1, 2);
        auto f1 = h.team[1].reduce(1, 2);
        expect_usage_error(h, f0, f1, 0);
    }
    {
        Hand h(2);
        auto f0 = h.team[0].scatter(std::vector<int>{}, 1);
        auto f1 = h.team[1].scatter(std::vector<int>{1}, 1);   // too few
        expect_usage_error(h, f0, f1, 1);
    }
    {
        Hand h(2);
        auto f0 = h.team[0].scatter(std::vector<int>{1, 2}, 1);   // not the root
        auto f1 = h.team[1].scatter(std::vector<int>{1, 2}, 1);
        Outcome<decltype(f0)> o0;
        ASSERT_TRUE(h.step(0, f0, o0));
        EXPECT_EQ(outcome(o0), "TeamUsageError");
        Outcome<decltype(f1)> o1;
        h.step(1, f1, o1);
        EXPECT_EQ(outcome(o1), "TeamUsageError");
    }
    {
        Hand h(2);
        std::vector<int> a(2), b(3);
        auto f0 = h.team[0].all_gather(1, std::span<int>(a));
        auto f1 = h.team[1].all_gather(1, std::span<int>(b));
        expect_usage_error(h, f0, f1, 1);
    }
    {
        Hand h(2);
        std::vector<int> in(4), short_in(3), out(4);
        auto f0 = h.team[0].all_reduce_elementwise(std::span<const int>(in), std::span<int>(out));
        auto f1 = h.team[1].all_reduce_elementwise(std::span<const int>(short_in), std::span<int>(out));
        expect_usage_error(h, f0, f1, 1);
    }
    {
        Hand h(2);
        std::vector<int> in(4), out(6);
        auto f0 = h.team[0].all_reduce_elementwise(std::span<const int>(in), std::span<int>(out.data(), 4));
        auto f1 = h.team[1].all_reduce_elementwise(std::span<const int>(out.data() + 2, 4),
                                                   std::span<int>(out.data(), 4));
        expect_usage_error(h, f0, f1, 1);
    }
}

TEST(TeamTest, SecondFutureWhileTheFirstIsUnfinished) {
    Hand h(2);
    auto f0 = h.team[0].sync();
    Outcome<decltype(f0)> o0;
    h.step(0, f0, o0);
    auto g0 = h.team[0].sync();
    Outcome<decltype(g0)> og;
    ASSERT_TRUE(h.step(0, g0, og));
    EXPECT_EQ(outcome(og), "TeamUsageError");
    h.step(0, f0, o0);
    EXPECT_EQ(outcome(o0), "TeamUsageError");
}

TEST(TeamTest, FinishedButNotDestroyedThenNext) {
    Hand h(2);
    auto f = make_round(h, [](TeamMember& m, std::size_t) { return m.sync(); });
    run_round(h, f, forward(2));
    auto g   = make_round(h, [](TeamMember& m, std::size_t r) { return m.all_reduce(int(r)); });
    auto res = run_round(h, g, forward(2));
    EXPECT_EQ(res[0]->value(), 1);
}

TEST(TeamTest, IsBrokenInsideAnOp) {
    for (bool expensive : {false, true}) {
        Hand h(2);
        bool returned = false;
        auto op = [&](int a, int b) {
            returned = h.team[0].is_broken();
            return a + b;
        };
        std::string o0, o1;
        if (expensive) {
            auto fs  = make_round(h, [&](TeamMember& m, std::size_t r) { return m.all_reduce(int(r), expensive_op(op)); });
            auto out = run_round(h, fs, forward(2));
            o0 = outcome(out[0]);
            o1 = outcome(out[1]);
        } else {
            auto fs  = make_round(h, [&](TeamMember& m, std::size_t r) { return m.all_reduce(int(r), cheap_op(op)); });
            auto out = run_round(h, fs, forward(2));
            o0 = outcome(out[0]);
            o1 = outcome(out[1]);
        }
        EXPECT_TRUE(returned);
        EXPECT_EQ(o0, "TeamUsageError");
        EXPECT_EQ(o1, "TeamUsageError");
    }
}

TEST(TeamTest, SkippedRoundBetweenDifferentCollectives) {
    Hand h(2);
    auto f0 = h.team[0].sync();
    auto f1 = h.team[1].all_reduce(1);   // skipped the sync
    expect_usage_error(h, f0, f1, 1);
}

TEST(TeamTest, TagsCatchASkipInALoop) {
    Hand h(2);
    for (std::uint64_t i = 0; i < 1; ++i) {
        auto fs  = make_round(h, [&](TeamMember& m, std::size_t) { return m.all_reduce(1).tag(i); });
        auto out = run_round(h, fs, forward(2));
        EXPECT_EQ(out[0]->value(), 2);
    }
    // Member 1 skips iteration 1.
    auto f0 = h.team[0].all_reduce(1).tag(1);
    auto f1 = h.team[1].all_reduce(1).tag(2);
    Outcome<decltype(f0)> o0, o1;
    h.step(0, f0, o0);
    h.step(1, f1, o1);
    h.step(0, f0, o0);
    auto e = error_as<TeamUsageError>(o1);
    ASSERT_TRUE(e);
    EXPECT_EQ(e->rank(), 1u);
    EXPECT_EQ(e->round(), 1u);
    EXPECT_EQ(outcome(o0), "TeamUsageError");
}

TEST(TeamTest, UntaggedAgainstTaggedIsUsageError) {
    Hand h(2);
    auto f0 = h.team[0].all_reduce(1).tag(0);
    auto f1 = h.team[1].all_reduce(1);
    expect_usage_error(h, f0, f1, 1);
}

TEST(TeamTest, MovedAfterFirstPollIsUsageError) {
    Hand h(2);
    auto f0 = h.team[0].all_reduce(1);
    Outcome<decltype(f0)> o0;
    h.step(0, f0, o0);
    auto moved = std::move(f0);
    Outcome<decltype(moved)> om;
    ASSERT_TRUE(h.step(0, moved, om));
    auto e = error_as<TeamUsageError>(om);
    ASSERT_TRUE(e);
    EXPECT_EQ(e->rank(), 0u);
    EXPECT_TRUE(h.team[1].is_broken());
}

TEST(TeamTest, TagAfterFirstPollIsUsageError) {
    Hand h(2);
    auto f0 = h.team[0].all_reduce(1);
    Outcome<decltype(f0)> o0;
    h.step(0, f0, o0);
    auto late = std::move(f0).tag(3);
    Outcome<decltype(late)> ol;
    ASSERT_TRUE(h.step(0, late, ol));
    EXPECT_EQ(outcome(ol), "TeamUsageError");
    EXPECT_TRUE(h.team[1].is_broken());
}

// ---------------------------------------------------------------------------
// On each executor
// ---------------------------------------------------------------------------

namespace {

struct MixedResults {
    std::atomic<int> wrong{0};
    std::atomic<int> errors{0};
};

// Every collective in turn, checked against what the serial evaluation gives.
Coro<void> mixed_member(TeamMember me, std::size_t rounds, MixedResults& res) {
    const std::size_t k = me.size();
    const std::size_t r = me.rank();
    try {
        for (std::size_t i = 0; i < rounds; ++i) {
            switch (i % 7) {
            case 0:
                if (co_await me.all_reduce(r + i) != k * i + k * (k - 1) / 2) res.wrong.fetch_add(1);
                break;
            case 1:
                if (co_await me.exclusive_scan(r + 1, std::size_t{0}) != r * (r + 1) / 2) res.wrong.fetch_add(1);
                break;
            case 2: {
                const std::size_t root = i % k;
                if (co_await me.broadcast(r == root ? i : std::size_t{0}, root) != i) res.wrong.fetch_add(1);
                break;
            }
            case 3:
                co_await me.sync();
                break;
            case 4:
                if (co_await me.all_reduce(letter(r), expensive_op(std::plus<>{})) != letters(0, k))
                    res.wrong.fetch_add(1);
                break;
            case 5: {
                auto all = co_await me.all_gather(r);
                for (std::size_t j = 0; j < k; ++j)
                    if (all[j] != j) res.wrong.fetch_add(1);
                break;
            }
            case 6:
                if (co_await me.inclusive_scan(letter(r)) != letters(0, r + 1)) res.wrong.fetch_add(1);
                break;
            }
        }
    } catch (...) {
        res.errors.fetch_add(1);
    }
}

Coro<void> run_mixed(std::vector<TeamMember>& team, std::size_t rounds, MixedResults& res) {
    JoinSet<void> set;
    for (auto& m : team) set.spawn(mixed_member(std::move(m), rounds, res));
    co_await set.drain();
}

// Member 2 throws while the others wait.
Coro<void> throwing_member(TeamMember me, std::array<std::string, 3>& got) {
    const std::size_t r = me.rank();
    if (r == 2) {
        co_await yield_now();
        throw std::runtime_error("member 2 failed");
    }
    try {
        (void)co_await me.all_reduce(1);
        got[r] = "ready";
    } catch (const TeamBroken& e) {
        got[r] = "TeamBroken " + std::to_string(e.rank());
    }
}

Coro<void> run_throwing(std::vector<TeamMember>& team, std::array<std::string, 3>& got,
                        std::string& reported) {
    JoinSet<void> set;
    for (auto& m : team) set.spawn(throwing_member(std::move(m), got));
    try {
        // Wait: cancelling the others could stop them before they see TeamBroken.
        co_await set.drain(OnError::Wait);
    } catch (const std::exception& e) {
        reported = e.what();
    }
}

}  // namespace

template<typename Traits>
class TeamRuntimeTest : public testing::Test {
protected:
    Traits traits;
};
TYPED_TEST_SUITE(TeamRuntimeTest, AllExecutors);

TYPED_TEST(TeamRuntimeTest, MixedCollectives) {
    for (std::size_t k : {1u, 3u, 4u}) {
        auto team = make_team(k);
        MixedResults res;
        this->traits.rt.block_on(run_mixed(team, 70, res));
        EXPECT_EQ(res.wrong, 0) << "k=" << k;
        EXPECT_EQ(res.errors, 0) << "k=" << k;
    }
}

TYPED_TEST(TeamRuntimeTest, MemberThatThrowsBreaksTheTeam) {
    auto team = make_team(3);
    std::array<std::string, 3> got;
    std::string reported;
    this->traits.rt.block_on(run_throwing(team, got, reported));
    EXPECT_EQ(got[0], "TeamBroken 2");
    EXPECT_EQ(got[1], "TeamBroken 2");
    EXPECT_EQ(reported, "member 2 failed");
}

#ifndef CORO_PICO

namespace {

// Member 1 gives up waiting for member 0, which is late; the next round succeeds.
Coro<void> timeout_member(TeamMember me, std::array<std::string, 3>& first, std::array<int, 3>& second) {
    const std::size_t r = me.rank();
    if (r == 0) co_await sleep_for(std::chrono::milliseconds(200));
    try {
        if (r == 1) {
            auto res = co_await timeout(std::chrono::milliseconds(20), me.all_reduce(1));
            first[r] = res.index() == 1 ? "timed out" : "ready";
        } else {
            (void)co_await me.all_reduce(1);
            first[r] = "ready";
        }
    } catch (const TeamRoundBroken& e) {
        first[r] = "TeamRoundBroken " + std::to_string(e.rank());
    }
    second[r] = co_await me.all_reduce(int(r));
}

Coro<void> run_timeout(std::vector<TeamMember>& team, std::array<std::string, 3>& first,
                       std::array<int, 3>& second) {
    JoinSet<void> set;
    for (auto& m : team) set.spawn(timeout_member(std::move(m), first, second));
    co_await set.drain();
}

}  // namespace

TYPED_TEST(TeamRuntimeTest, TimeoutBreaksOnlyItsRound) {
    auto team = make_team(3);
    std::array<std::string, 3> first;
    std::array<int, 3> second{};
    this->traits.rt.block_on(run_timeout(team, first, second));
    EXPECT_EQ(first[0], "TeamRoundBroken 1");
    EXPECT_EQ(first[1], "timed out");
    EXPECT_EQ(first[2], "TeamRoundBroken 1");
    for (int v : second) EXPECT_EQ(v, 3);
}

// ---------------------------------------------------------------------------
// Threads
// ---------------------------------------------------------------------------

// Member 0's future is destroyed, polled and unfinished, while member 1 combines its
// value on another thread: the destructor waits for the combine.
TEST(TeamThreadTest, DestroyedWhileClaimedWaitsForTheCombine) {
    Hand h(2);
    std::optional<TeamAllReduceFuture<int, ExpensiveOp<HookOp>>> f0(h.team[0].all_reduce(0, expensive_op(HookOp{})));
    auto f1 = h.team[1].all_reduce(1, expensive_op(HookOp{}));
    Outcome<std::remove_reference_t<decltype(*f0)>> o0;
    Outcome<decltype(f1)> o1;
    h.step(0, *f0, o0);

    std::atomic<bool> in_combine{false}, combine_done{false};
    HookOp::hook = [&] {
        in_combine = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        combine_done = true;
    };
    std::thread other([&] {
        auto waker = detail::make_rc<CountingWaker>();
        detail::Context ctx(waker);
        auto result = f1.poll(ctx);
        if (!result.isPending()) o1.emplace(std::move(result));
    });
    while (!in_combine) std::this_thread::yield();
    f0.reset();
    EXPECT_TRUE(combine_done);
    other.join();
    if (!o1) h.step(1, f1, o1);
    EXPECT_EQ(outcome(o1), "TeamUsageError");
}

// ---------------------------------------------------------------------------
// Stress
// ---------------------------------------------------------------------------

namespace {

struct SleepyPlus {
    double operator()(double a, double b) const {
        thread_local std::mt19937 rng(std::random_device{}());
        std::this_thread::sleep_for(std::chrono::microseconds(rng() % 50));
        return a + b;
    }
};

Coro<void> stress_member(TeamMember me, std::size_t rounds, const std::vector<double>& values,
                         double expected, MixedResults& res) {
    const std::size_t r = me.rank();
    try {
        for (std::size_t i = 0; i < rounds; ++i) {
            if (i % 2 == 0) {
                if (co_await me.all_reduce(values[r], expensive_op(SleepyPlus{})) != expected)
                    res.wrong.fetch_add(1);
            } else {
                if (co_await me.all_reduce(values[r]) != expected) res.wrong.fetch_add(1);
                if (co_await me.exclusive_scan(r + 1, std::size_t{0}) != r * (r + 1) / 2) res.wrong.fetch_add(1);
            }
        }
    } catch (...) {
        res.errors.fetch_add(1);
    }
}

Coro<void> run_stress(std::vector<TeamMember>& team, std::size_t rounds, const std::vector<double>& values,
                      double expected, MixedResults& res) {
    JoinSet<void> set;
    for (auto& m : team) set.spawn(stress_member(std::move(m), rounds, values, expected, res));
    co_await set.drain();
}

}  // namespace

TEST(TeamStressTest, WorkStealingManyRounds) {
    Runtime rt(std::size_t{4});
    const std::size_t k = 4;
    const std::vector<double> values{1e16, 1.0, -1e16, 1.0};
    const double expected = tree_reduce(values, std::plus<>{});
    {
        auto team = make_team(k);
        MixedResults res;
        rt.block_on(run_stress(team, 1000, values, expected, res));
        EXPECT_EQ(res.wrong, 0);
        EXPECT_EQ(res.errors, 0);
    }
    {
        auto team = make_team(k);
        MixedResults res;
        rt.block_on(run_mixed(team, 3000, res));
        EXPECT_EQ(res.wrong, 0);
        EXPECT_EQ(res.errors, 0);
    }
}

#endif  // !CORO_PICO
