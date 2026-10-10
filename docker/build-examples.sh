#!/usr/bin/env bash
# Compiles every example under examples/ inside a platform's CI image
# (docker/Dockerfile.<platform>, see docker/build-image.sh), to catch example build
# breakage. CI does not build the examples, and this script has no counterpart in
# ci/. Examples are not run, only built — except
# where examples/grpc/README.md documents an interactive client/server smoke
# test, which is intentionally left to the developer.
#
# examples/pico is out of scope here: it cross-compiles against the Pico SDK
# (arm-none-eabi-gcc), which has no equivalent in this desktop image.
#
# Usage: [CORO_PLATFORM=<platform>] docker/build-examples.sh
#
# The platform is CORO_PLATFORM, or the primary one (docker/common.sh) if that is not
# set.
#
# Mounts the live repo at /workspace read-only, same as run-tests.sh.
# examples/io and examples/grpc are conanfile.py recipes (same shape as
# test/conanfile.py), built with `conan install` + `conan build .` — no manual
# cmake invocation or CMakeUserPresets.json involved, so the user_presets=
# conf disabled in the default profile is irrelevant here. Each recipe's
# --output-folder points into /builds, a single named volume holding one
# subfolder per project, since the source tree itself is read-only.
set -euo pipefail

# Sets REPO_ROOT, IMAGE and VOLUME_PREFIX; builds the image if this machine does not have it.
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"
use_image

docker run --rm -i \
    -v "$REPO_ROOT:/workspace:ro" \
    -v "$VOLUME_PREFIX-examples-builds:/builds" \
    -v "$VOLUME_PREFIX-conan-cache:/root/.conan2/p" \
    "$IMAGE" \
    bash -s <<'EOF'
        set -xeuo pipefail

        # /workspace is bind-mounted from the host, so it is owned by the
        # host user, not whatever UID this container runs as -- git refuses
        # to touch a repo it does not own ("dubious ownership") unless told
        # otherwise. The `git describe` inside conan_version.py, used to
        # derive both conanfile.py and test/conanfile.py package versions,
        # hits this same check, so it has to be silenced before any conan
        # command below runs, not just before an explicit git command.
        git config --global --add safe.directory /workspace

        # 1. Export coro with default options into the cache so
        # examples/io and examples/grpc — both real downstream consumers via
        # find_package(coro CONFIG REQUIRED) — can resolve it below.
        cd /workspace
        conan export .

        # 2. examples/io: conanfile.py recipe, same shape as test/conanfile.py.
        pushd /workspace/examples/io
        conan build . --build=missing --output-folder=/builds/io
        popd

        # 3. examples/grpc: conanfile.py recipe — consumes coro/0.1.0 from the
        # cache populated in step 1, plus grpc and xtensor (built from source
        # here if no prebuilt binary matches this profile — can be slow on a
        # cold cache).
        pushd /workspace/examples/grpc
        conan build . --build=missing --output-folder=/builds/grpc
        popd
EOF
