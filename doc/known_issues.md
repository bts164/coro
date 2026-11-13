# Known Issues

Problems that are not bugs in this library but that you may run into while using it:
compiler defects and toolchain differences. Each entry gives the symptom as you would
see it, what triggers it, and how to get past it.

If you hit something that is not listed here, add it.

| ID | Symptom | Affects |
|---|---|---|
| [KI.1](#ki1-gcc-13-crashes-on-a-braced-list-inside-a-co_await-expression) | `internal compiler error: in build_special_member_call` at the closing brace of a coroutine | GCC 13 |
| [KI.2](#ki2-gcc-before-162-never-destroys-a-tuple-protocol-structured-binding-in-a-coroutine-frame) | LeakSanitizer reports a leak of whatever a structured binding held in a coroutine; nothing else fails | GCC up to 16.1; fixed in 16.2 |

---

## KI.1 — GCC 13 crashes on a braced list inside a `co_await` expression

**Workaround:** build the container in a statement of its own, then `co_await`.

```cpp
// Crashes GCC 13:
co_await wait_all({a, b, c});          // a, b, c are std::shared_ptr

// Works everywhere:
std::vector<std::shared_ptr<T>> items{a, b, c};
co_await wait_all(std::move(items));
```

### Symptom

The compiler itself crashes. The error points at the closing brace of a coroutine, not
at the line that causes it:

```text
test_runtime_shutdown.cpp: In function ‘coro::Coro<Handles> make_handles(...)’:
test_runtime_shutdown.cpp:757:1: internal compiler error: in build_special_member_call, at cp/call.cc:11096
  757 | }
      | ^
...
0xbe55a8 morph_fn_to_coro(tree_node*, tree_node**, tree_node**)
```

`morph_fn_to_coro` in the backtrace says the crash is in GCC's coroutine transformation.

### Trigger

A braced list of objects with a non-trivial copy constructor or destructor
(`std::shared_ptr`, `std::string`, ...) written inside the same expression as a
`co_await`, typically as a function argument that becomes a `std::initializer_list`.
The list's hidden backing array is a temporary that has to live across the suspension,
and GCC 13 fails while moving it into the coroutine frame.

The same braced list in a statement with no `co_await` is fine, including in a
coroutine.

### Affected compilers

| Compiler | Result |
|---|---|
| GCC 13 (Ubuntu 24.04, the sanitizer Docker image) | Crashes |
| GCC 15.2 | Compiles |

Versions in between have not been tried.

!!! note "NOTE: The crash may be reported later than you expect"
    A lambda coroutine inside a template (a gtest `TYPED_TEST` body, for instance) is
    not compiled until the template is instantiated, at the end of the file. The first
    crash reported is then the first *non-template* coroutine with the pattern, even if
    others appear earlier in the source. Fix every occurrence, not just the one named.

!!! note "NOTE: How this was diagnosed"
    The trigger was identified from the backtrace and the code of the function named in
    the error, and confirmed by the fix: applying the workaround above, and nothing
    else, to the four places that used the pattern made the file compile on GCC 13. It
    has not been reduced to a minimal test case. If the workaround does not clear a
    crash with this signature, look next at other class-type temporaries in a
    `co_await` or `co_return` expression of the same function, and move them into named
    locals the same way.

---

## KI.2 — GCC before 16.2 never destroys a tuple-protocol structured binding in a coroutine frame

**Fixed in GCC 16.2.** Every GCC tried up to 16.1 has it; 16.2 compiles the same code
correctly. It is a GCC defect only: no Clang or MSVC tried has it. coro keeps the
workaround in its own API, since it supports earlier GCCs.

**Workaround** on an earlier GCC: bind by reference to a named object, or bind a plain
aggregate. Everything coro hands back for a binding is already a plain aggregate
(`ChannelPair`, `JoinOutput`, `BufResult`, `UdpDatagram`, `TeamRange`), so the workaround is
only needed for other types: `std::pair`, `std::tuple`, `std::array`,
`std::ranges::subrange` and any other type with a tuple protocol, including in a range-for
(guideline
[GCC.1](guidelines.md#gcc1-dont-let-a-structured-binding-through-the-tuple-protocol-live-across-a-suspension)).

```cpp
std::pair<std::string, std::vector<int>> load_table();

// Leaks on GCC before 16.2: the hidden std::pair is in the frame across the co_await.
auto [name, rows] = load_table();
co_await publish(name);
use(rows);

// Works everywhere: the pair is a named local; the binding only refers to it.
auto table = load_table();
auto& [name, rows] = table;
co_await publish(name);
use(rows);

// Leaks on GCC before 16.2: each element is copied into a hidden pair in the frame.
for (auto [name, station] : stations)
    co_await report(name, station);

// Works everywhere: the binding refers to the map's own element.
for (const auto& [name, station] : stations)
    co_await report(name, station);
```

### Symptom

Nothing fails at run time. Whatever the binding held is never freed, so LeakSanitizer
reports it at exit as a **direct** leak with nothing else leaked alongside it: the object
holding the last reference was in a coroutine frame that was freed without that object's
destructor running. For a channel, the leak is its shared state, allocated by the channel
factory:

```text
Direct leak of 112 byte(s) in 1 object(s) allocated from:
    ...
    #10 ... coro::oneshot_channel<...>() .../coro/sync/oneshot.h
    #11 ... in <the coroutine that bound the channel> ...
```

A side effect can be visible too: a channel's sender held that way was never destroyed, so
its receiver never saw the channel close. coro's results were changed to plain aggregates
for this reason (below).

With `-Wall` and optimization, GCC also warns `'<anonymous>' may be used uninitialized
[-Wmaybe-uninitialized]` at the closing brace of every affected coroutine, pointing at the
binding. That is the one compile-time clue, so don't silence it in coroutine code. It errs on
the safe side: it also warns for a binding before a `co_await` that could suspend but
doesn't, which doesn't leak.

### Trigger

All of:

- a structured binding that goes through the **tuple protocol**: a `std::pair`, a
  `std::tuple`, a `std::array`, a `std::ranges::subrange` (a `std::complex` in C++26), or any
  other type with `std::tuple_size` and `get<I>`;
- **by value** (`auto [a, b] = e;`, or `for (auto [a, b] : range)`), so the binding has a
  hidden object of its own;
- of something with a **destructor that does something**, as a member or an element;
- inside a **coroutine**, still in scope at a `co_await` or `co_yield` that **really
  suspends**, so the hidden object is placed in the coroutine frame;
- built with **optimization** (`-O1` and above), except for a type whose `get<I>` returns by
  value, as `subrange`'s does: then each bound name refers to a temporary of its own, and
  those leak at `-O0` too.

Not affected: a plain aggregate bound member by member; a binding by reference to a named
object, or a range-for by reference (`for (const auto& [a, b] : range)`); a binding of
trivially destructible values only, such as a `subrange` of pointers; a binding whose scope ends before the suspension; a coroutine that doesn't suspend
after the binding (including `co_await std::suspend_never{}`). The promise type, the awaiter,
`await_transform` and `final_suspend` made no difference in any combination tried.

### Affected compilers

From `test/compiler/structured_binding_leak.cpp`, which needs only `<coroutine>`. Build and
run it to check any other compiler or set of flags:

| Compiler | Flags | Result |
|---|---|---|
| GCC 15.2.1 20260123 (Rocky 8 gcc-toolset-15) | `-O0` | Only `std::ranges::subrange` leaks (its `get<I>` returns by value) |
| GCC 15.2.1 20260123 (Rocky 8 gcc-toolset-15) | `-O1`, `-O2`, `-O3`, with or without `-fsanitize=address,undefined` | Every tuple-protocol case that crosses a real suspension leaks: `std::pair`, `std::tuple`, a type of its own, a range-for by value, `std::array` and `std::ranges::subrange` |
| GCC 16.1 and the earlier versions tried (Compiler Explorer) | `-O2` | Leaks |
| GCC 16.2 (Compiler Explorer) | `-O2` | No leaks |
| Clang 21.1.8 | `-O2` | No leaks |
| MSVC 19.28 (VS 2019 16.8), 19.44 (VS 2022 17.14), 19.51 (VS 2026 18.6), on Compiler Explorer | `/std:c++latest /permissive- /EHsc /W4`, with `/O2` and with `/Od` | No leaks |

!!! note "NOTE: What coro changed"
    Every result coro hands back for a structured binding used to be a `std::pair` or a
    `std::tuple`, and leaked when bound this way: the channel factories' ends, `join()`'s
    results, and the buffers from stream, file and UDP reads and writes. Each is now a plain
    aggregate: `ChannelPair` (`first`, `second`), `JoinOutput` (`first` ... `tenth`, so at most
    ten branches unless you add more, below; `coro::get<I>` for index access), `BufResult` (`size`, `buf`), `UdpDatagram` (`size`, `buf`, `sender`) and
    `TeamRange` (`begin`, `end`). Structured bindings of them are unchanged; `std::get<I>` on a
    `join()` result becomes `coro::get<I>`, and `.first`/`.second` on read results become
    `.size`/`.buf`. The tests check each is an
    aggregate without a tuple protocol (`test/tuple_protocol.h`), and
    `test/sync/test_channel_pair.cpp` and `test_join.cpp` check at run time that bound results
    are destroyed. `LwipCallbackResult` keeps its `std::tuple`, but only for trivially
    destructible arguments, which a `static_assert` enforces: they have no destructor to miss.

!!! note "NOTE: Why coro's own tests didn't catch it for a long time"
    Almost every channel test bound `auto [tx, rx] = ...` in a `Coro<void>` body run by
    `block_on`, and in that shape GCC 15 happened to destroy the binding correctly in the
    one case checked under coro (with `co_await coro::sleep_for(...)`). The same binding in a
    `CoroStream` across a `co_yield` leaked. Why the `Coro` case differed isn't known: in a
    minimal coroutine type, `co_await` leaks just as `co_yield` does. So don't rely on the
    kind of suspension; `test/sync/test_channel_pair.cpp` now covers both.

!!! note "NOTE: How this was diagnosed"
    An AddressSanitizer build of an application using coro reported two direct leaks at
    shutdown, each the shared state of a channel created with `auto [tx, rx] = ...` in a
    `CoroStream`. A
    standalone coroutine with no coro code reproduced it, and the cases above narrowed it:
    a type of our own with `tuple_size` and `get` leaks while the same type without them
    doesn't, and the destructor of the bound object is never called at all (not called and
    then optimized into nothing). No GCC bug report matching it was found; GCC 16.2 fixed it.

### More than ten branches in a join()

`JoinOutput`, what `join()` resolves with, is a plain aggregate with one member per branch,
so coro has to spell out each branch count, and defines 1 to 10. A `join()` of more fails to
compile with a `static_assert` that points here. Either nest a `join()`, or define the
specialization for your count yourself, in the same form: one member per branch, in branch
order, declared before the `join()` that needs it. `join()` builds it with a braced list, so
it needs nothing else. Any member names will do; this one carries on from coro's `first` ...
`tenth`:

```cpp
#include <coro/sync/join.h>

namespace coro {

// What a join() of 12 branches resolves with.
template<typename T0, typename T1, typename T2, typename T3, typename T4, typename T5,
         typename T6, typename T7, typename T8, typename T9, typename T10, typename T11>
struct JoinOutput<T0, T1, T2, T3, T4, T5, T6, T7, T8, T9, T10, T11> {
    T0  first;
    T1  second;
    T2  third;
    T3  fourth;
    T4  fifth;
    T5  sixth;
    T6  seventh;
    T7  eighth;
    T8  ninth;
    T9  tenth;
    T10 eleventh;
    T11 twelfth;
};

} // namespace coro
```

Don't give it a `std::tuple_size` specialization: that would make a structured binding of it
go through the tuple protocol, and leak again. `auto [a, b, ...] = co_await join(...)` works
as it is. `coro::get<I>` covers only coro's own specializations; for index access to yours,
for a `join()` of a pack, add an overload for it in namespace `coro`, as `join.h` defines
coro's.

---

!!! tip "TODO: A GCC coroutine defect seen with -fno-elide-constructors"
    Found by chance while testing KI.2, and unrelated to it. On GCC 15 with
    `-fno-elide-constructors`, a coroutine's return object appeared to be copied bitwise
    rather than through its move constructor, so a type that destroys its coroutine handle
    in its destructor destroyed the frame twice and crashed. Not yet investigated: confirm
    the trigger and the affected versions, then write it up as an entry of its own.
