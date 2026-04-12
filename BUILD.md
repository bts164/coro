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
