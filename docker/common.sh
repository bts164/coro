# Sourced by the scripts in this directory; not run on its own.
#
# Picks the platform, names its CI image and makes sure there is one to run. Sets:
#   REPO_ROOT       the repository root
#   PLATFORM        the platform the script runs on, e.g. ubuntu2404
#   IMAGE           the image a script should `docker run` (set by use_image)
#   VOLUME_PREFIX   what a script's named volumes start with, so that no two
#                   platforms share a build folder or a Conan cache
#
# A platform is a file here named Dockerfile.<platform>. CORO_PLATFORM chooses one;
# left unset, it is PRIMARY_PLATFORM.
#
# The image's tag is a hash of what the image is built from, so a change to the
# Dockerfile or a profile gives a new name and an old image is never used by mistake.
#
# CORO_IMAGE, if set, names the image to use instead, and nothing is built: for
# running in the image CI published, say. Left unset, the name has no registry in it,
# so docker never pulls it: it is only ever the image built on this machine.

DOCKER_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$DOCKER_DIR/.." && pwd)"

# The platform a script runs on when CORO_PLATFORM is not set. It is also the one CI
# runs the sanitizer and emulator jobs on; the other platforms get one plain run of
# the tests (.github/workflows/tests.yml).
PRIMARY_PLATFORM=ubuntu2404

PLATFORM="${CORO_PLATFORM:-$PRIMARY_PLATFORM}"
IMAGE_DOCKERFILE="Dockerfile.$PLATFORM"

list_platforms() {
    (cd "$DOCKER_DIR" && ls Dockerfile.* | sed 's/^Dockerfile\.//')
}

if [ ! -f "$DOCKER_DIR/$IMAGE_DOCKERFILE" ]; then
    echo "error: no platform '$PLATFORM' (no docker/$IMAGE_DOCKERFILE). Platforms:" >&2
    list_platforms | sed 's/^/    /' >&2
    exit 1
fi

VOLUME_PREFIX="coro-$PLATFORM"

# Hashes the names and contents of the files the image is built from: the platform's
# Dockerfile, its own profile (profiles/<platform>, the image's default profile) and
# the profiles that are not any platform's, which every image gets. Another platform's
# profile is left out, so editing it does not rename this platform's image.
image_hash() {
    (cd "$DOCKER_DIR" &&
        {
            echo "$IMAGE_DOCKERFILE"
            for profile in profiles/*; do
                name="${profile#profiles/}"
                if [ "$name" = "$PLATFORM" ] || [ ! -f "Dockerfile.$name" ]; then
                    echo "$profile"
                fi
            done
        } | LC_ALL=C sort | xargs sha256sum | sha256sum | cut -c1-12)
}

LOCAL_IMAGE="coro-ci-$PLATFORM:$(image_hash)"

build_local_image() {
    # docker/ is the build context: the image copies nothing from the rest of the tree.
    docker build -t "$LOCAL_IMAGE" -f "$DOCKER_DIR/$IMAGE_DOCKERFILE" "$DOCKER_DIR"
}

# Sets IMAGE, building the local image first if this machine does not have it.
use_image() {
    if [ -n "${CORO_IMAGE:-}" ]; then
        IMAGE="$CORO_IMAGE"
        return
    fi
    IMAGE="$LOCAL_IMAGE"
    if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
        echo "no local image $IMAGE; building it (docker/build-image.sh)" >&2
        build_local_image
    fi
}
