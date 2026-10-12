# Build Instructions

## Quick Start

```bash
cd misc/coro

# Install all build dependencies
conan install . --build=missing

# Build everything
cmake --preset conan-release
cmake --build --preset conan-release

# Run all tests
ctest --preset conan-release
```

## CMakeLists.txt Configuration

Edit `CMakeLists.txt` to enable/disable tests and examples:

```cmake
# Uncomment to build tests
enable_testing()
add_subdirectory(test)

# Uncomment to build examples (including PcieDecoder tests)
add_subdirectory(examples/io)
```

## Running Specific Tests

```bash
# Run all tests
ctest --preset conan-release

# Run only unit tests
ctest --preset conan-release --exclude-regex "PcieDecoder"

# Run only PcieDecoder example tests
ctest --preset conan-release -R PcieDecoder
```

## Sanitizer Builds

A sanitizer build is a Conan profile, not an option of the package. The profile passes
`-fsanitize` to every package built with it: `coro`, the `test/` package, their
dependencies and an application. Neither recipe and none of the desktop CMake files
look at the sanitizer.

`docker/profiles` has the profiles CI uses. Each is named after a platform's own
profile, which it adds to:

| Profile | Built with |
|---|---|
| `asan` | AddressSanitizer, LeakSanitizer and UBSan |
| `tsan` | ThreadSanitizer |
| `ubsan` | UBSan in trap mode, for a Pico (below) |

A sanitizer profile has two parts. This is `tsan`:

```ini
[settings]
compiler.sanitizer=Thread

[conf]
tools.build:cflags+=["-fsanitize=thread", "-g"]
tools.build:cxxflags+=["-fsanitize=thread", "-g"]
tools.build:exelinkflags+=["-fsanitize=thread"]
tools.build:sharedlinkflags+=["-fsanitize=thread"]
```

The `[conf]` lines are the sanitizer. The setting adds no flags: it makes the sanitizer
part of every package ID, so a sanitizer build and a plain one never share a binary in
the Conan cache. Conan has no such setting of its own, so define it once per Conan
home, and copy the profiles there too:

```bash
conan config install docker/settings_user.yml
conan config install docker/profiles/asan -tf profiles
conan config install docker/profiles/tsan -tf profiles
```

Then name one after your own host profile:

```bash
conan build . -pr:h default -pr:h tsan -pr:b default --build=missing
```

The first such build compiles every dependency again, with the sanitizer.

A profile without the setting and the flags builds plain, and needs no change to work
with coro.

### The whole program must be built with the same sanitizer

Use the same profile for an application that links coro. An executable built without
`-fsanitize=thread` that loads a coro built with it crashes at start-up inside the
sanitizer's runtime, with no report.

### On a Pico

ASan and TSan need a runtime library, shadow memory and threads, which a
microcontroller does not have; run them on the desktop build, whose `test_pico_suite`
covers the Pico configuration of the library.

UBSan on a Pico is UBSan in trap mode (`-fsanitize-trap=undefined`, GCC 13 or later).
It needs no runtime: undefined behaviour executes an undefined instruction and the
firmware stops in a hard fault. Nothing is printed. Only coro's own sources are
instrumented, not the Pico SDK or lwIP, so the `ubsan` profile does not pass compiler
flags to every package. It sets a CMake variable, `CORO_SANITIZE_OPTIONS`, which
coro's Pico build puts on its own sources (`cmake/Sanitize.cmake`). An application
linking a coro package built this way needs no extra flags.

### Recommended environment variables when running sanitizer builds

Set these before running `ctest` or a single test binary. Without
`halt_on_error=1`, a low-level error common to every test repeats across the
entire suite and makes the output unreadably long.

**AddressSanitizer + LeakSanitizer + UBSan (`Address`, `AddressUndefinedBehavior`)**

```bash
export ASAN_OPTIONS=halt_on_error=1:abort_on_error=1:detect_leaks=1:check_initialization_order=1:strict_string_checks=1:detect_stack_use_after_return=1
export UBSAN_OPTIONS=halt_on_error=1:abort_on_error=1:print_stacktrace=1
ctest --preset conan-debug --stop-on-failure   # -x for short
```

| Variable / Option | Effect |
|---|---|
| `ASAN_OPTIONS=halt_on_error=1` | Stop on the first ASAN error instead of continuing through all tests |
| `abort_on_error=1` | Call `abort()` instead of `_exit()` — generates a core dump; lets gdb/lldb stop at the exact failure point |
| `detect_leaks=1` | Enable LeakSanitizer (on by default on Linux, off on macOS) |
| `check_initialization_order=1` | Detect bugs caused by global initializer ordering |
| `strict_string_checks=1` | Stricter boundary checking on `strlen`, `strcpy`, etc. |
| `detect_stack_use_after_return=1` | Catch stack variable references that outlive their stack frame |
| `UBSAN_OPTIONS=print_stacktrace=1` | Print a full stack trace on every UBSan violation |

**ThreadSanitizer (`Thread`)**

```bash
export TSAN_OPTIONS=halt_on_error=1:abort_on_error=1:second_deadlock_stack=1:suppressions=$PWD/ci/sanitizers/tsan.supp
ctest --preset conan-debug --stop-on-failure
```

`ci/sanitizers/tsan.supp` lists the reports in third-party code that are known and
believed harmless, with the reasoning for each. Dependencies are instrumented too, so
an application that links coro and runs under ThreadSanitizer meets the same reports
and needs the same file.

| Variable / Option | Effect |
|---|---|
| `halt_on_error=1` | Stop on the first race or deadlock report |
| `abort_on_error=1` | Call `abort()` for core dump / debugger catch |
| `second_deadlock_stack=1` | Print both lock acquisition stacks on deadlock reports |

## Dependencies

All dependencies are in `conanfile.txt`:

### Runtime Dependencies (exported with library)
- `libuv/1.47.0` - I/O event loop
- `libwebsockets/4.3.5` - WebSocket support

### Build-Only Dependencies (not exported)
- `gtest/1.14.0` - Google Test framework (for tests/examples only)

**Note:** gtest is in the main conanfile for convenience during development, but when packaging the coro library for distribution, you'd use a separate `conanfile.py` with proper `requires()` and `build_requires()` sections to distinguish runtime vs build-time dependencies.

## Library Packaging (Future)

When creating a Conan package of the coro library for others to consume, you would:

1. Create `conanfile.py` (instead of conanfile.txt)
2. Move gtest to `build_requires()` section
3. Only `requires()` would list libuv and libwebsockets
4. Users who `conan install coro` would only get runtime dependencies

Example conanfile.py structure:
```python
class CoroConan(ConanFile):
    name = "coro"
    requires = ["libuv/1.47.0", "libwebsockets/4.3.5"]

    def build_requirements(self):
        self.test_requires("gtest/1.14.0")  # Only for building tests
```
