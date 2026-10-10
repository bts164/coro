#!/usr/bin/env bash
# Runs ci/run-tests.sh, the CI test job, inside a platform's CI image
# (docker/Dockerfile.<platform>) on this machine. .github/workflows/tests.yml runs
# that same script in that same image, so a pass here is a pass there. This file is
# only the local stand-in for what GitHub does for a CI job: start the container and
# give it the source.
#
# Usage: [CORO_PLATFORM=<platform>] docker/run-tests.sh none|asan|tsan
#
#   none   a plain build
#   asan   AddressSanitizer and UndefinedBehaviorSanitizer
#   tsan   ThreadSanitizer
#
# The platform is CORO_PLATFORM, or the primary one (docker/common.sh) if that is not
# set. Uses the image built on this machine (docker/build-image.sh), building it first
# if it is missing. Set CORO_IMAGE to run in another image instead.
#
# Mounts the live repo at /workspace read-only. All writes are routed into
# named volumes instead, mounted over the specific paths that need to be
# writable — test/build (compiled output) and the Conan cache — so nothing
# the container does can touch files on the host at all, sidestepping the
# bind-mount ownership question entirely rather than working around it. (CI has no
# need of this: its checkout is thrown away with the runner.)
set -euo pipefail

SANITIZER="${1:?usage: $0 none|asan|tsan}"
case "$SANITIZER" in
    none|asan|tsan) ;;
    *) echo "error: SANITIZER must be 'none', 'asan' or 'tsan', got '$SANITIZER'" >&2; exit 1 ;;
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

# Sets REPO_ROOT, IMAGE and VOLUME_PREFIX; builds the image if this machine does not have it.
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"
use_image

# A volume mounted inside the read-only tree needs its mount point to exist already:
# docker cannot create it there. An empty, git-ignored directory on the host.
mkdir -p "$REPO_ROOT/test/build"

docker run --rm -i \
    -v "$REPO_ROOT:/workspace:ro" \
    -v "$VOLUME_PREFIX-test-build-$SANITIZER:/workspace/test/build" \
    -v "$VOLUME_PREFIX-conan-cache:/root/.conan2/p" \
    "$IMAGE" \
    /workspace/ci/run-tests.sh "$SANITIZER"
