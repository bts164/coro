# lwIP configuration ownership

## Problem

lwIP's headers resolve `lwipopts.h` via an unqualified `#include "lwipopts.h"`
— whichever include directory is searched first that contains a matching
file wins. Historically every Pico application in this repo provided its
own copy (`examples/pico/lwipopts.h`, `examples/pico/epaper/lwipopts.h` —
byte-identical) and had to remember to add it to `coro_pico`'s include path.
Forgetting it is a build error; subtly diverging it is worse, since
`lwipopts.h` affects the ABI of every translation unit that includes lwIP
headers, including the ones inside `coro_pico` itself.

This is also a real Conan binary-compatibility hazard, not just a CMake
ergonomics one: if `coro_pico`'s package ID doesn't change when
`lwipopts.h` changes, two applications with different lwIP configs could be
served the *same cached binary* from Conan's cache — built against a
config that doesn't match either of them.

## Design

**`coro` owns `lwipopts.h` exclusively.** Any package that needs lwIP must
get it transitively through `coro` — by linking `coro::pico` (or
`coro::pico_mqtt`, etc.) — never by declaring its own copy or its own
include path for one. `coro_pico_mqtt` is the existing proof this works:
it needs `LWIP_MQTT 1`, but gets there through its `coro_pico` dependency,
not by bringing its own `lwipopts.h`. A future `coro_pico_https` (or any
other lwIP-touching module) must follow the same rule. This is a documented
constraint, not something enforced generically across an arbitrary
dependency graph — a second, independent lwIP-using Conan package that
insists on its own config is an accepted incompatibility, not a problem
this design tries to solve.

**Feature toggles are coro options, not CMake-variable plumbing.** Content
that genuinely varies by feature (currently just `LWIP_MQTT`) is controlled
by a `coro` Conan option (`with_mqtt`), which:

- Drives `#cmakedefine01 LWIP_MQTT` in `src/io/lwip/lwipopts.h.in`, expanded
  via `configure_file()` in `cmake/platforms/pico.cmake` into
  `${CMAKE_CURRENT_BINARY_DIR}/coro_pico_lwipopts/lwipopts.h` (mirrored by
  the plain CMake variable `CORO_PICO_WITH_MQTT` for non-Conan / source
  builds, e.g. `examples/pico/CMakeLists.txt`).
- Automatically participates in Conan's package ID — an app built with
  `with_mqtt=True` gets a distinctly cached `coro` binary from one built
  without it, with no extra wiring needed.
- Is read by `cmake/platforms/pico_mqtt.cmake`, which `FATAL_ERROR`s if
  included before `pico.cmake` (so it can't end up pointed at a different
  `lwipopts.h` than `coro_pico` itself was compiled against).

This was deliberately *not* built as a generic per-knob `configure_file()`
parameterization (e.g. a CMake variable for every `#define` in the file) —
that would be solving tuning needs nobody has hit yet. `LWIP_MQTT` earned
its own toggle because a real, existing consumer (`coro_pico_mqtt`) needs
it; nothing else has.

**The full-file override remains for genuinely custom tuning.** Setting
`CORO_PICO_LWIPOPTS_DIR` to a directory containing a complete, hand-written
`lwipopts.h` bypasses generation entirely — for tuning beyond what coro's
own options cover (e.g. non-default `MEM_SIZE`/`TCP_WND` for a
memory-constrained board). `pico.cmake` and `pico_mqtt.cmake` both consume
the same resolved directory (`CORO_PICO_LWIPOPTS_INCLUDE_DIR`), so an
override always applies consistently across every coro-provided lwIP
target in one build — never just one of them.

## Future: packages split out of `coro` itself

If `coro_pico_mqtt` (or a future `coro_pico_https`) is ever split into its
*own* Conan package rather than living inside `coro`'s own CMakeLists.txt,
the options-as-package-ID mechanism alone isn't enough — Conan doesn't
stop two independently-versioned recipes from disagreeing about a shared
option's value. The pattern for that case is `validate()`:

```python
def validate(self):
    if not self.dependencies["coro"].options.with_mqtt:
        raise ConanInvalidConfiguration(
            "coro_pico_mqtt requires coro built with with_mqtt=True")
```

This raises a hard, early error rather than a silent link-against-the-wrong-
config failure. Not implemented yet because there is no split-out package
yet — `coro_pico_mqtt` still lives in `coro`'s own `cmake/platforms/`. Add
this `validate()` the day a `coro_pico_mqtt` *package* (separate
`conanfile.py`) is introduced.
