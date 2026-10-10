# docker/

The images that CI runs in, one per platform, and the scripts that run a CI job in
one on your own machine.

A CI job is a script in [`ci/`](../ci), run inside a platform's image. GitHub starts
that container itself. Each `run-*.sh` script here does the same thing locally for
one job: it starts the container, gives it the source, and runs that job's `ci/`
script. So a local run and a CI run are the same commands in the same image.

| CI job | Its commands | Run it locally with |
|---|---|---|
| `tests.yml` | `ci/run-tests.sh` | `docker/run-tests.sh` |
| `pico_emulator.yml` | `ci/run-emulator-tests.sh` | `docker/run-emulator-tests.sh` |

CI never calls the `run-*.sh` scripts. It does call `build-image.sh`, to name and
publish the images.

You need Docker and nothing else: the compilers, Conan, the Pico SDK and the Conan
profiles are all in the image.

## Quick start

From the repository root:

```sh
docker/run-tests.sh none               # desktop tests, plain build
docker/run-tests.sh asan               # the same, under AddressSanitizer + UBSan
docker/run-tests.sh tsan               # the same, under ThreadSanitizer
docker/run-emulator-tests.sh none      # Pico test firmware in the RP2040 emulator
docker/run-emulator-tests.sh ubsan     # the same, built with UBSan
docker/build-examples.sh               # build every desktop example
```

Those run on the primary platform, Ubuntu 24.04. To run on another, name it:

```sh
CORO_PLATFORM=rocky10 docker/run-tests.sh none
```

The first run on a platform builds its image, which takes a while and about 4 GB of
disk. Later runs reuse it.

## What is here

| File | What it is |
|---|---|
| `Dockerfile.ubuntu2404` | The primary platform's image: Ubuntu 24.04, GCC 13, the ARM toolchain, Conan, CMake, the Pico SDK and `picotool`. |
| `Dockerfile.rocky10` | Rocky Linux 10. Not finished: it does not build yet, and no workflow uses it. |
| `profiles/<platform>` | That platform's Conan profile: desktop builds, and the build-machine side of Pico builds. Copied into its image as the `default` profile. |
| `profiles/rp2040_pico_w` | Conan host profile for the RP2040 on a Pico W board. The same in every image. |
| `build-image.sh` | Builds a platform's image on this machine. |
| `run-tests.sh` | Runs `ci/run-tests.sh` in a container: builds the library and its tests, plain or with a sanitizer, and runs them. CI: `tests.yml`. |
| `run-emulator-tests.sh` | Runs `ci/run-emulator-tests.sh` in a container: builds the on-target test firmware and runs it in the emulator. CI: `pico_emulator.yml`. |
| `build-examples.sh` | Builds the examples under `examples/io` and `examples/grpc` in a container. Local only: CI has no such job, and there is no `ci/` script behind it. |
| `common.sh` | Sourced by the scripts above. Picks the platform, names its image and builds it when it is missing. Not run on its own. |

## Platforms

A platform is a file here named `Dockerfile.<platform>`. Every script runs on one
platform at a time:

| `CORO_PLATFORM` | A script runs on |
|---|---|
| not set (the default) | The primary platform, `PRIMARY_PLATFORM` in `common.sh`. |
| set to a platform's name | That platform. |

`docker/build-image.sh --list` prints the platforms, the primary one first.

The primary platform is the one that gets the expensive jobs. CI runs:

| Job | Primary platform | Every other platform |
|---|---|---|
| `ci/run-tests.sh asan` | yes | no |
| `ci/run-tests.sh tsan` | yes | no |
| `ci/run-tests.sh none` | no | yes |
| `ci/run-emulator-tests.sh ubsan` | yes | no |

The plain run on the other platforms shows that the library builds and passes there.
`ci/run-emulator-tests.sh none` is not run by CI at all: the UBSan build runs the same
tests and checks more. It is there to run locally.
The firmware is cross-compiled, so one platform is enough for the emulator jobs.
Locally there is no such limit: any script runs on any platform whose image has what
the script needs.

### Adding a platform

1. Add `profiles/<platform>`, the Conan profile for that platform's own compiler.
2. Add `Dockerfile.<platform>`. `docker/` is the build context. For `run-tests.sh` the
   image needs a C++ compiler, Git, Python, Conan and CMake 3.31 or later, and it
   copies `profiles/<platform>` to `/root/.conan2/profiles/default`.
3. Run `CORO_PLATFORM=<platform> docker/run-tests.sh none` until it passes.
4. Name the platform in both lists in `.github/workflows/tests.yml`: the one the
   images are published from, and the one the test jobs are made from, with
   `sanitizer: none`.

To change the primary platform, change `PRIMARY_PLATFORM` in `common.sh` and move the
sanitizer rows in `tests.yml` to it. Its image must be able to run the emulator job.

## The image

A platform's image is named `coro-ci-<platform>:<hash>`. The hash covers
`Dockerfile.<platform>`, `profiles/<platform>` and the profiles that belong to no
platform (`profiles/rp2040_pico_w`), so:

- You never type the name. The scripts work it out.
- Editing the Dockerfile or a profile gives a new name. The next script you run
  finds no image by that name and builds one, so a change to the image is tested
  before you push it.
- An old image is never used by mistake after such an edit.

To build ahead of time, or to see the name:

```sh
docker/build-image.sh                  # build it
docker/build-image.sh --print-name     # print coro-ci-ubuntu2404:<hash>, build nothing
CORO_PLATFORM=rocky10 docker/build-image.sh
```

Old images stay until you remove them. `docker image ls 'coro-ci-*'` lists them and
`docker image rm coro-ci-<platform>:<hash>` removes one.

### Local and published images

CI publishes its own copy of each image it uses to the GitHub container registry
(`.github/workflows/ci_image.yml`), under the same name and hash, and runs its jobs
in that copy. The scripts here do not use it unless told to:

| `CORO_IMAGE` | A script runs in |
|---|---|
| not set (the default) | `coro-ci-<platform>:<hash>`, built on this machine. Nothing is pulled. |
| set to an image name | That image. Nothing is built; Docker pulls it if it is not here. |

To run in the published image instead of building one:

```sh
CORO_IMAGE=ghcr.io/<owner>/<repository>/$(docker/build-image.sh --print-name) \
    docker/run-emulator-tests.sh none
```

Nothing here pushes an image. Only CI does.

## How the scripts run

Each script starts a container from the image and runs its commands in it (for the
two `run-*.sh` scripts, the job's `ci/` script), with:

- **The repository mounted read-only at `/workspace`.** Nothing a run does can change
  your working tree, and the files it makes are not left owned by root.
- **Named volumes for everything written.** They persist between runs, so a second
  run rebuilds only what changed.

Every volume's name starts with `coro-<platform>-`, so no two platforms share one:

| Volume | Holds | Used by |
|---|---|---|
| `coro-<platform>-test-build-none`, `-asan`, `-tsan` | `test/build` for that variant | `run-tests.sh` |
| `coro-<platform>-emulator-build-none`, `-ubsan` | `test/build` for that variant, plus the emulator's npm packages and boot ROM | `run-emulator-tests.sh` |
| `coro-<platform>-examples-builds` | One build folder per example | `build-examples.sh` |
| `coro-<platform>-conan-cache` | The Conan package cache | all three |

To start a job from scratch, remove its volumes:

```sh
docker volume rm coro-ubuntu2404-emulator-build-ubsan coro-ubuntu2404-conan-cache
```

`docker volume ls --filter name=coro-` lists them all.

The scripts create an empty `test/build` directory in your tree if there is none.
Docker needs it as a mount point; it is git-ignored.

None of this applies in CI. There the checkout is writable and thrown away with the
runner, so the job builds straight into it, and nothing is kept between runs.

## Things that go wrong

- **ThreadSanitizer aborts every test with "unexpected memory mapping".** The host
  kernel randomises more address bits than ThreadSanitizer accepts. It is a setting
  of the host, not the container. `run-tests.sh tsan` warns when it sees
  it; the fix, until the next reboot, is `sudo sysctl vm.mmap_rnd_bits=28` on the
  host.
- **A build fails in a way that a profile or Dockerfile change should have fixed.**
  A failed configure can be left in a volume. Remove the job's build volume, and the
  Conan cache volume if a dependency is involved.
- **The first emulator run needs the network.** It installs the emulator with `npm`
  and downloads the RP2040 boot ROM. Both are then kept in the build volume.

## Changing the image

1. Edit a `Dockerfile.<platform>` or a profile.
2. Run the script for the job you care about. It builds the new image and runs in it.
3. Push. CI sees a hash it has no image for, builds and publishes one, and runs the
   tests in it.

A change to `profiles/rp2040_pico_w` renames every platform's image, because they
all copy it. A change to a platform's own profile renames only that platform's.

## Changing a job

Edit its script in `ci/`. Anything a job needs belongs there or in the image, not in
the workflow file or the script here, so that a local run and a CI run cannot differ.
The one exception is the ThreadSanitizer kernel setting, which belongs to the host:
the workflow sets it, and `run-tests.sh` warns about it.

A new job is a new script in `ci/`, a workflow that runs it in the image, and a
script here to run it locally, written like the two that exist.
