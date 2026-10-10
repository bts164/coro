#!/usr/bin/env bash
# Builds the library and its tests and runs them. This is the CI test job
# (.github/workflows/tests.yml), which runs it in a platform's CI image.
#
# Usage: ci/run-tests.sh none|asan|tsan
#
#   none   a plain build
#   asan   AddressSanitizer and UndefinedBehaviorSanitizer
#   tsan   ThreadSanitizer
#
# Written to run inside a CI image (docker/Dockerfile.<platform>): it uses the
# image's default Conan profile and changes the global git configuration. To run it
# that way on your own machine, use docker/run-tests.sh.
set -xeuo pipefail

SANITIZER="${1:?usage: $0 none|asan|tsan}"
case "$SANITIZER" in
    none|asan|tsan) ;;
    *) echo "error: SANITIZER must be 'none', 'asan' or 'tsan', got '$SANITIZER'" >&2; exit 1 ;;
esac
export CORO_SANITIZE=$SANITIZER

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# The checkout is owned by whoever made it (the host user, or the CI runner's), not
# by the UID this container runs as -- git refuses to touch a repo it does not own
# ("dubious ownership") unless told otherwise. The `git describe` inside
# conan_version.py, used to derive both conanfile.py and test/conanfile.py package
# versions, hits this same check, so it has to be silenced before any conan
# command below runs, not just before an explicit git command.
git config --global --add safe.directory "$REPO_ROOT"

# coro itself must land in the Conan cache (not just have its deps
# installed) since test/conanfile.py consumes it as a real
# requirement (coro/[0.1.0]) via find_package(coro CONFIG REQUIRED)
# — plain `conan install .` only resolves *its* dependencies, it
# does not make the coro package itself available to test/.
cd "$REPO_ROOT"
conan export .

# libunicorn is a bespoke pinned-build recipe local to this repo (see
# test/libunicorn/conanfile.py), not a ConanCenter package -- it has
# to be exported the same way coro itself is, or test/'s conan
# install below cannot resolve the libunicorn/2.1.4 requirement at
# all in a fresh cache.
conan export test/libunicorn

cd test
mkdir -p build
conan install . --build=missing -f json > build/graph.json
conan build . --build=missing

# halt_on_error=1 is essential here — without it the first
# sanitizer hit repeats across the whole suite instead of stopping,
# per BUILD.md.
if [ "$SANITIZER" = asan ]; then
    export ASAN_OPTIONS=halt_on_error=1:abort_on_error=1:detect_leaks=1:check_initialization_order=1:strict_string_checks=1:detect_stack_use_after_return=1
    export UBSAN_OPTIONS=halt_on_error=1:abort_on_error=1:print_stacktrace=1
elif [ "$SANITIZER" = tsan ]; then
    export TSAN_OPTIONS=halt_on_error=1:abort_on_error=1:second_deadlock_stack=1
fi

cd build/Release && ctest --stop-on-failure -V
