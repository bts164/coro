// A compiler check, not a unit test: does this compiler destroy a structured binding's hidden
// object when that object lives in a coroutine frame? GCC up to 16.1 doesn't when the binding
// goes through the tuple protocol (std::tuple, std::pair, or any type with tuple_size and get),
// the defect described in doc/known_issues.md, KI.2; GCC 16.2 fixed it. It needs no coro: only
// <coroutine>.
//
// Build and run it with the compiler and flags in question, for example:
//
//   g++ -std=c++23 -O2 test/compiler/structured_binding_leak.cpp -o sbl && ./sbl
//   clang++ -std=c++23 -O2 test/compiler/structured_binding_leak.cpp -o sbl && ./sbl
//   cl /std:c++latest /permissive- /EHsc /W4 /O2 test\compiler\structured_binding_leak.cpp && structured_binding_leak
//
// It prints one row per kind of binding and one column per way the coroutine suspends, then a
// few other bindings through the tuple protocol (a range-for, std::array, std::ranges::subrange),
// each "ok" or "LEAK", and exits 1 if any case leaks. Not in the test suite, as it is expected to
// fail on an affected compiler. Results so far, on x86-64:
//
//   GCC 15.2.1 (Rocky 8 gcc-toolset-15)  -O0: only std::ranges::subrange leaks.
//                                         -O1 to -O3, with or without ASan/UBSan: every
//                                         tuple-protocol case that crosses a real suspension
//                                         leaks (12 cases); the range-for by reference doesn't.
//   GCC 16.1 and earlier (Compiler        -O2: leaks.
//     Explorer)
//   GCC 16.2 (Compiler Explorer)          -O2: no leaks.
//   Clang 21                              -O2: no leaks.
//   MSVC 19.28, 19.44, 19.51 (Compiler    /O2 and /Od: no leaks.
//     Explorer)
//
// subrange leaks even at -O0 because its get<I>() returns by value: each bound name is then a
// reference to a temporary of its own, and GCC 15 doesn't destroy those at any level. With
// optimization, the hidden object holding the subrange leaks too.
//
// On an affected GCC, -Wall also warns "'<anonymous>' may be used uninitialized" at the
// closing brace of every coroutine that leaks, naming the binding's hidden object: a cheap way
// to find affected code elsewhere. It errs on the safe side, warning for the await_never cases
// too, whose co_await could suspend but doesn't. It doesn't warn at -O0, so it misses the
// subrange case there.
//
// The plain aggregate, bound member by member, never leaks: that is why everything coro hands
// back for a binding is one (ChannelPair, JoinOutput, BufResult, UdpDatagram, TeamRange)
// rather than a std::pair or std::tuple. Nor does binding by reference to a named object
// (`auto p = make(); auto& [a, b] = p;`, or `for (const auto& [k, v] : map)`), the workaround
// for other types.
//
// Don't build it with -fno-elide-constructors on GCC 15: that hits a different GCC defect,
// where a coroutine's return object is copied bitwise rather than moved, so Gen's destructor
// destroys the same frame twice and the program crashes.

#include <array>
#include <coroutine>
#include <cstddef>
#include <cstdio>
#include <map>
#include <memory>
#include <ranges>
#include <tuple>
#include <utility>

namespace {

// --- The four kinds of binding ---

// A plain aggregate: bound member by member, not through the tuple protocol.
template <typename T1, typename T2>
struct PlainPair {
    T1 first;
    T2 second;
};

// The same, with the tuple protocol defined for it, as std::pair has.
template <typename T1, typename T2>
struct TuplePair {
    T1 first;
    T2 second;
};

// get() for TuplePair, found by argument-dependent lookup as the tuple protocol allows.
template <std::size_t I, typename T1, typename T2>
auto& get(TuplePair<T1, T2>& p) noexcept {
    if constexpr (I == 0) return p.first;
    else return p.second;
}
template <std::size_t I, typename T1, typename T2>
const auto& get(const TuplePair<T1, T2>& p) noexcept {
    if constexpr (I == 0) return p.first;
    else return p.second;
}
template <std::size_t I, typename T1, typename T2>
auto&& get(TuplePair<T1, T2>&& p) noexcept {
    if constexpr (I == 0) return std::move(p.first);
    else return std::move(p.second);
}

} // namespace

template <typename T1, typename T2>
struct std::tuple_size<TuplePair<T1, T2>> : std::integral_constant<std::size_t, 2> {};
template <typename T1, typename T2>
struct std::tuple_element<0, TuplePair<T1, T2>> { using type = T1; };
template <typename T1, typename T2>
struct std::tuple_element<1, TuplePair<T1, T2>> { using type = T2; };

namespace {

// --- A minimal coroutine type ---

struct Gen {
    struct promise_type {
        Gen get_return_object() { return Gen(std::coroutine_handle<promise_type>::from_promise(*this)); }
        std::suspend_always initial_suspend() noexcept { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }
        std::suspend_always yield_value(int) { return {}; }
        void return_void() {}
        void unhandled_exception() {}
    };
    std::coroutine_handle<promise_type> h;

    explicit Gen(std::coroutine_handle<promise_type> handle) : h(handle) {}
    Gen(Gen&& o) noexcept : h(std::exchange(o.h, {})) {}
    Gen(const Gen&) = delete;
    Gen& operator=(const Gen&) = delete;
    Gen& operator=(Gen&&) = delete;
    ~Gen() {
        if (h) h.destroy();
    }
};

// An ordinary local in each coroutine, as a control: counts the ones not destroyed.
int canaries = 0;
struct Canary {
    Canary() { ++canaries; }
    ~Canary() { --canaries; }
};

// --- The ways a coroutine can hold the binding, each run for every kind of binding ---

template <template <typename...> typename Pair>
struct Cases {
    using P = Pair<std::shared_ptr<int>, std::shared_ptr<int>>;

    // Both halves share one object, so its use count after the coroutine is gone is the
    // number of halves never destroyed.
    static P make(std::weak_ptr<int>& w) {
        auto p = std::make_shared<int>(1);
        w = p;
        return P{p, p};
    }

    // No suspension after the binding: it needn't live in the frame.
    static Gen no_suspend(std::weak_ptr<int>& w) {
        Canary c;
        auto [a, b] = make(w);
        (void)*a;
        (void)*b;
        co_return;
    }
    // co_await of an awaiter that never suspends.
    static Gen await_never(std::weak_ptr<int>& w) {
        Canary c;
        auto [a, b] = make(w);
        co_await std::suspend_never{};
        (void)*a;
        (void)*b;
    }
    // A real suspension through co_await, run to the end.
    static Gen await_always(std::weak_ptr<int>& w) {
        Canary c;
        auto [a, b] = make(w);
        co_await std::suspend_always{};
        (void)*a;
        (void)*b;
    }
    // A real suspension through co_yield, run to the end.
    static Gen yield(std::weak_ptr<int>& w) {
        Canary c;
        auto [a, b] = make(w);
        co_yield *a;
        (void)*b;
    }
    // Suspended at a co_yield and destroyed there, never resumed: as a stream abandoned
    // part-way.
    static Gen yield_abandoned(std::weak_ptr<int>& w) {
        Canary c;
        auto [a, b] = make(w);
        co_yield *a;
        (void)*b;
    }
    // The workaround: bind by reference to a named object, across a co_yield.
    static Gen by_reference(std::weak_ptr<int>& w) {
        Canary c;
        auto pair = make(w);
        auto& [a, b] = pair;
        co_yield *a;
        (void)*b;
    }
};

// --- Other bindings through the tuple protocol, each across a real suspension ---

// A range-for binding by value copies each element into a hidden object: here a std::pair, one
// per iteration. The map is an ordinary local, destroyed normally, so only the copy can leak.
Gen range_for_by_value(std::weak_ptr<int>& w) {
    Canary c;
    std::map<int, std::shared_ptr<int>> m;
    auto p = std::make_shared<int>(1);
    w = p;
    m.emplace(1, std::move(p));
    for (auto [k, v] : m) {
        co_await std::suspend_always{};
        (void)k;
        (void)*v;
    }
}

// The usual way to write it, and the workaround: the binding refers to the map's own element.
Gen range_for_by_reference(std::weak_ptr<int>& w) {
    Canary c;
    std::map<int, std::shared_ptr<int>> m;
    auto p = std::make_shared<int>(1);
    w = p;
    m.emplace(1, std::move(p));
    for (const auto& [k, v] : m) {
        co_await std::suspend_always{};
        (void)k;
        (void)*v;
    }
}

Gen array(std::weak_ptr<int>& w) {
    Canary c;
    auto p = std::make_shared<int>(1);
    w = p;
    auto [a, b] = std::array<std::shared_ptr<int>, 2>{p, p};
    p.reset();
    co_await std::suspend_always{};
    (void)*a;
    (void)*b;
}

// An iterator that owns something, as some do; a subrange of plain pointers has nothing to leak.
struct OwningIterator {
    using value_type = int;
    using difference_type = std::ptrdiff_t;
    std::shared_ptr<int> owner;
    int* p = nullptr;
    int& operator*() const { return *p; }
    OwningIterator& operator++() {
        ++p;
        return *this;
    }
    OwningIterator operator++(int) {
        OwningIterator before = *this;
        ++p;
        return before;
    }
    bool operator==(const OwningIterator& o) const { return p == o.p; }
};

int element = 0;

Gen subrange(std::weak_ptr<int>& w) {
    Canary c;
    auto p = std::make_shared<int>(1);
    w = p;
    auto [b, e] = std::ranges::subrange(OwningIterator{p, &element}, OwningIterator{p, &element + 1});
    p.reset();
    co_await std::suspend_always{};
    (void)*b;
    (void)(b == e);
}

// Runs `f` (to the end, or resumed once and abandoned), then destroys it. Returns whether
// anything it held outlived it.
bool leaks(Gen (*f)(std::weak_ptr<int>&), bool to_end) {
    std::weak_ptr<int> w;
    canaries = 0;
    {
        Gen g = f(w);
        g.h.resume();
        if (to_end)
            while (!g.h.done()) g.h.resume();
    }
    if (canaries != 0) std::printf("(an ordinary local leaked too: the control failed) ");
    return w.use_count() != 0;
}

int leaking_cases = 0;

const char* result(bool leaked) {
    if (leaked) ++leaking_cases;
    return leaked ? "LEAK" : "ok";
}

template <template <typename...> typename Pair>
void row(const char* name) {
    using C = Cases<Pair>;
    std::printf("%-11s %-10s %-11s %-12s %-6s %-15s %s\n", name, result(leaks(C::no_suspend, true)),
                result(leaks(C::await_never, true)), result(leaks(C::await_always, true)),
                result(leaks(C::yield, true)), result(leaks(C::yield_abandoned, false)),
                result(leaks(C::by_reference, true)));
}

void other(const char* name, Gen (*f)(std::weak_ptr<int>&)) {
    std::printf("%-23s %s\n", name, result(leaks(f, true)));
}

} // namespace

int main() {
#if defined(__clang__)
    std::printf("Clang %s", __clang_version__);
#elif defined(__GNUC__)
    std::printf("GCC %s", __VERSION__);
#elif defined(_MSC_VER)
    std::printf("MSVC %d", _MSC_FULL_VER);
#else
    std::printf("unknown compiler");
#endif
#if defined(__OPTIMIZE__)
    std::printf(", optimized\n\n");
#elif defined(_MSC_VER)
    // MSVC defines no macro for its optimization level.
    std::printf(", optimization not reported\n\n");
#else
    std::printf(", not optimized\n\n");
#endif
    std::printf("%-11s %-10s %-11s %-12s %-6s %-15s %s\n", "binding", "no_suspend", "await_never", "await_always",
                "yield", "yield_abandoned", "by_reference");
    row<std::tuple>("std::tuple");
    row<std::pair>("std::pair");
    row<TuplePair>("TuplePair");
    row<PlainPair>("PlainPair");
    std::printf("\n%-23s %s\n", "other binding", "await_always");
    other("range-for by value", range_for_by_value);
    other("range-for by reference", range_for_by_reference);
    other("std::array", array);
    other("std::ranges::subrange", subrange);
    std::printf("\n%d case(s) leaked\n", leaking_cases);
    return leaking_cases == 0 ? 0 : 1;
}
