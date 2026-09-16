#!/usr/bin/env bash
# Dry-runs the asan/tsan CI job (see .github/workflows/tests.yml) inside the
# Ubuntu 24.04 / GCC 13 image built from docker/Dockerfile.ubuntu2404 — lets
# you catch build/test failures locally before pushing.
#
# Usage: docker/run-sanitizer-build.sh asan|tsan
#
# Mounts the live repo at /workspace read-only. All writes are routed into
# named volumes instead, mounted over the specific paths that need to be
# writable — test/build (compiled output) and the Conan cache — so nothing
# the container does can touch files on the host at all, sidestepping the
# bind-mount ownership question entirely rather than working around it.
set -euo pipefail

SANITIZER="${1:?usage: $0 asan|tsan}"
case "$SANITIZER" in
    asan|tsan) ;;
    *) echo "error: SANITIZER must be 'asan' or 'tsan', got '$SANITIZER'" >&2; exit 1 ;;
esac

if [ "$SANITIZER" = tsan ]; then
    # vm.mmap_rnd_bits is a global, non-namespaced kernel setting -- it can't
    # be fixed with a docker run flag or anything inside the container, and
    # a stock Ubuntu 24.04 host's default (32) is higher than ThreadSanitizer's
    # runtime expects (<=28), so it aborts every test with "FATAL:
    # ThreadSanitizer: unexpected memory mapping" before any of our code even
    # runs. This only weakens ASLR entropy, and only until next reboot, but
    # it's a host-wide security-relevant setting -- deliberately just warned
    # about here rather than silently sudo'd.
    rnd_bits="$(cat /proc/sys/vm/mmap_rnd_bits 2>/dev/null || echo unknown)"
    if [ "$rnd_bits" != unknown ] && [ "$rnd_bits" -gt 28 ] 2>/dev/null; then
        echo "warning: /proc/sys/vm/mmap_rnd_bits is $rnd_bits (>28) -- ThreadSanitizer" >&2
        echo "will abort every test with 'unexpected memory mapping'. Fix (until next" >&2
        echo "reboot) by running on the HOST, not in the container:" >&2
        echo "    sudo sysctl vm.mmap_rnd_bits=28" >&2
    fi
fi

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE_TAG=coro-sanitizer-test

docker build -t "$IMAGE_TAG" -f "$REPO_ROOT/docker/Dockerfile.ubuntu2404" "$REPO_ROOT"

docker run --rm -i \
    -v "$REPO_ROOT:/workspace:ro" \
    -v coro-sanitizer-test-build-$SANITIZER:/workspace/test/build \
    -v coro-sanitizer-test-conan-cache:/root/.conan2/p \
    -e SANITIZER="$SANITIZER" \
    "$IMAGE_TAG" \
    bash -s <<'EOF'
        set -xeuo pipefail
        export CORO_SANITIZE=$SANITIZER

        # /workspace is bind-mounted from the host, so it is owned by the
        # host user, not whatever UID this container runs as -- git refuses
        # to touch a repo it does not own ("dubious ownership") unless told
        # otherwise. The `git describe` inside conan_version.py, used to
        # derive both conanfile.py and test/conanfile.py package versions,
        # hits this same check, so it has to be silenced before any conan
        # command below runs, not just before an explicit git command.
        git config --global --add safe.directory /workspace

        # coro itself must land in the Conan cache (not just have its deps
        # installed) since test/conanfile.py consumes it as a real
        # requirement (coro/[0.1.0]) via find_package(coro CONFIG REQUIRED)
        # — plain `conan install .` only resolves *its* dependencies, it
        # does not make the coro package itself available to test/.
        cd /workspace
        conan export .

        # libunicorn is a bespoke pinned-build recipe local to this repo (see
        # test/libunicorn/conanfile.py), not a ConanCenter package -- it has
        # to be exported the same way coro itself is, or test/'s conan
        # install below cannot resolve the libunicorn/2.1.4 requirement at
        # all in a fresh cache.
        conan export test/libunicorn

        cd test
        conan install . --build=missing -f json > build/graph.json
        conan build . --build=missing

        # halt_on_error=1 is essential here — without it the first
        # sanitizer hit repeats across the whole suite instead of stopping,
        # per BUILD.md.
        if [ "$SANITIZER" = asan ]; then
            export ASAN_OPTIONS=halt_on_error=1:abort_on_error=1:detect_leaks=1:check_initialization_order=1:strict_string_checks=1:detect_stack_use_after_return=1
            export UBSAN_OPTIONS=halt_on_error=1:abort_on_error=1:print_stacktrace=1
        else
            export TSAN_OPTIONS=halt_on_error=1:abort_on_error=1:second_deadlock_stack=1
        fi

        cd build/Release && ctest --stop-on-failure -V
EOF
