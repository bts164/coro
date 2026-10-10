#!/usr/bin/env bash
# Builds a platform's CI image on this machine, from docker/Dockerfile.<platform> and
# docker/profiles. The run-*.sh and build-*.sh scripts here use it, and build it
# themselves if it is missing, so this is only needed to build ahead of time or to
# test a change to the image before pushing.
#
# Usage: [CORO_PLATFORM=<platform>] docker/build-image.sh [--print-name | --list]
#
#   --print-name   print the image's name (coro-ci-<platform>:<hash>) and build nothing
#   --list         print the platforms there are, the primary one first, and build
#                  nothing
#
# The platform is CORO_PLATFORM, or the primary one (docker/common.sh) if that is not
# set.
#
# The image is never pushed from here. CI publishes its own copy
# (.github/workflows/ci_image.yml).
set -euo pipefail

source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

case "${1:-}" in
    --print-name) echo "$LOCAL_IMAGE"; exit 0 ;;
    --list) echo "$PRIMARY_PLATFORM"; list_platforms | grep -vx "$PRIMARY_PLATFORM" || true; exit 0 ;;
    "") ;;
    *) echo "usage: $0 [--print-name | --list]" >&2; exit 1 ;;
esac

build_local_image
echo "built $LOCAL_IMAGE" >&2
