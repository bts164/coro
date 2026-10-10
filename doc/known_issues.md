# Known Issues

Problems that are not bugs in this library but that you may run into while using it:
compiler defects and toolchain differences. Each entry gives the symptom as you would
see it, what triggers it, and how to get past it.

If you hit something that is not listed here, add it.

| ID | Symptom | Affects |
|---|---|---|
| [KI.1](#ki1-gcc-13-crashes-on-a-braced-list-inside-a-co_await-expression) | `internal compiler error: in build_special_member_call` at the closing brace of a coroutine | GCC 13 |

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
