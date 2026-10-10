#!/usr/bin/env bash
# Runs ci/run-emulator-tests.sh, the CI emulator job, inside a platform's CI image
# (docker/Dockerfile.<platform>) on this machine. .github/workflows/pico_emulator.yml
# runs that same script in that same image, so a pass here is a pass there. This file
# is only the local stand-in for what GitHub does for a CI job: start the container
# and give it the source.
#
# Usage: [CORO_PLATFORM=<platform>] docker/run-emulator-tests.sh none|ubsan
#
# The platform is CORO_PLATFORM, or the primary one (docker/common.sh) if that is not
# set. Its image must have the ARM toolchain, the Pico SDK and Node.js. Uses the image
# built on this machine (docker/build-image.sh), building it first if it is missing.
# Set CORO_IMAGE to run in another image instead.
#
# Mounts the live repo at /workspace read-only. All writes are routed into
# named volumes instead, mounted over the specific paths that need to be
# writable — test/build (compiled output) and the Conan cache — so nothing
# the container does can touch files on the host at all, sidestepping the
# bind-mount ownership question entirely rather than working around it. (CI has no
# need of this: its checkout is thrown away with the runner.)
set -euo pipefail

SANITIZER="${1:?usage: $0 none|ubsan}"
case "$SANITIZER" in
    none|ubsan) ;;
    *) echo "error: SANITIZER must be 'none' or 'ubsan', got '$SANITIZER'" >&2; exit 1 ;;
esac

# Sets REPO_ROOT, IMAGE and VOLUME_PREFIX; builds the image if this machine does not have it.
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"
use_image

# A volume mounted inside the read-only tree needs its mount point to exist already:
# docker cannot create it there. An empty, git-ignored directory on the host.
mkdir -p "$REPO_ROOT/test/build"

docker run --rm -i \
    -v "$REPO_ROOT:/workspace:ro" \
    -v "$VOLUME_PREFIX-emulator-build-$SANITIZER:/workspace/test/build" \
    -v "$VOLUME_PREFIX-conan-cache:/root/.conan2/p" \
    "$IMAGE" \
    /workspace/ci/run-emulator-tests.sh "$SANITIZER"
