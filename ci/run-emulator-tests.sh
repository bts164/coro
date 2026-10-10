#!/usr/bin/env bash
# Builds the on-target test firmware for the RP2040 and runs it in the emulator. This
# is the CI emulator job (.github/workflows/pico_emulator.yml), which runs it in the
# primary platform's CI image.
#
# Usage: ci/run-emulator-tests.sh none|ubsan
#
# Written to run inside a CI image (docker/Dockerfile.<platform>) that has the ARM
# toolchain, the Pico SDK and Node.js: it uses the image's Conan profiles and changes
# the global git configuration. To run it that way on your own machine, use
# docker/run-emulator-tests.sh.
set -xeuo pipefail

SANITIZER="${1:?usage: $0 none|ubsan}"
case "$SANITIZER" in
    none|ubsan) ;;
    *) echo "error: SANITIZER must be 'none' or 'ubsan', got '$SANITIZER'" >&2; exit 1 ;;
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

cd test
mkdir -p build
conan build . -pr:h rp2040_pico_w -pr:b default --build=missing -f json > build/graph.json

# --work-dir: run_on_target.py installs the emulator (npm) and downloads the boot
# ROM the first time, by default next to itself. docker/run-emulator-tests.sh mounts
# the source tree read-only, so both go in the build folder, where they are kept for
# the next run.
./target/run_on_target.py --emulator --work-dir build/emulator \
    build/baremetal/Release/coro_target_*.uf2
