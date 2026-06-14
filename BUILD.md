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

`ENABLE_ASAN` and `ENABLE_TSAN` are mutually exclusive.

```bash
# AddressSanitizer + LeakSanitizer + UBSan
conan install . --build=missing -s:h build_type=Debug
cmake --preset conan-debug -DENABLE_ASAN=ON
cmake --build --preset conan-debug
ctest --preset conan-debug

# ThreadSanitizer
cmake --preset conan-debug -DENABLE_TSAN=ON
cmake --build --preset conan-debug
ctest --preset conan-debug
```

### Recommended environment variables when running sanitizer builds

Set these before running `ctest` or a single test binary. Without
`halt_on_error=1`, a low-level error common to every test repeats across the
entire suite and makes the output unreadably long.

**AddressSanitizer + LeakSanitizer + UBSan (`ENABLE_ASAN=ON`)**

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

**ThreadSanitizer (`ENABLE_TSAN=ON`)**

```bash
export TSAN_OPTIONS=halt_on_error=1:abort_on_error=1:second_deadlock_stack=1
ctest --preset conan-debug --stop-on-failure
```

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
