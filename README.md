# coro

A C++20 coroutines library for multi-threaded asynchronous task synchronization and I/O,
heavily inspired by Rust's async model and the [Tokio](https://tokio.rs) runtime.

Coroutines let you write concurrent code that reads like sequential code — suspend at
`co_await`, resume when the event fires, no callback hell, no manual state machines.

## Requirements

- C++23
- CMake
- [Conan](https://conan.io) (recommended for dependency management)

```bash
conan install . --build=missing -s:h build_type=Release
cmake --preset conan-release
cd build/Release && make
```

## Documentation

- [Introduction](doc/index.md)
- [Getting Started Guide](doc/getting_started.md)
- [Library Usage Guidelines](doc/guidelines.md)

### Internal Design Details

- [Futures and Streams](doc/future_and_stream.md)
- [Tasks, Wakers, and Context](doc/waker_and_context_propagation.md)
- [Tasks, Executors, and Runtime](doc/task_and_executor.md)
- [Executor Design](doc/executor_design.md) · [Work-Stealing Scheduler](doc/work_stealing_executor.md)
- I/O: [I/O Coroutines](doc/io_coroutine.md) · [libuv Integration](doc/libuv_integration.md) · [WebSocket Stream](doc/websocket_stream.md) · [PollStreams](doc/poll_streams.md)
- Synchronization: [Coroutine Scope](doc/coroutine_scope.md) · [JoinSet](doc/join_set.md) · [Channels](doc/channels.md) · [Select](doc/select.md)
- [Module Structure](doc/module_structure.md)

### Examples

- [Examples](doc/examples.md)

### Versioning and Releases

coro follows [Semantic Versioning](https://semver.org). Released versions are annotated
git tags on `master` in the form `vX.Y.Z` — a tag is the sole trigger for a release, and
there are no separate, persistent release branches. (Release-candidate tags are the one
exception to the "on `master`" part — see below.)

The Conan recipe derives its version directly from git via `set_version()` rather than a
hand-maintained version string:

- On an exact `vX.Y.Z` release tag, the package version is the clean `X.Y.Z`.
- On an exact `vX.Y.Z-rc.N` release-candidate tag — which can be cut on any branch, not
  just `master`, to get a testable build before merging — the package version is the
  clean `X.Y.Z-rc.N`. See [Release candidates](#release-candidates) below.
- On any other commit — on any branch — the version is a SemVer
  [prerelease](https://semver.org/#spec-item-9) identifier derived from `git describe`:
  the next unreleased version (patch-bumped by default; see
  [Signaling a minor/major dev-build bump](#signaling-a-minormajor-dev-build-bump)),
  the commit count and hash since the nearest reachable tag, and optional branch-name /
  dirty-state metadata. See
  [How the version is derived on non-tagged commits](#how-the-version-is-derived-on-non-tagged-commits)
  for the exact format in each case.

This means every commit — on `master` or any other branch — is a valid, individually
addressable Conan package; there is no separate "nightly" or "bleeding edge" branch to
maintain. Consumers who reference an exact version (`coro/X.Y.Z-dev.N+gSHA`) can always
pull a specific prerelease build. Consumers who use a
[version range](https://docs.conan.io/2/tutorial/versioning/version_ranges.html)
(e.g. `coro/[>=1.0.0]`) never see prereleases unintentionally — Conan excludes prerelease
versions from range resolution by default, and a consumer must opt in explicitly
(`resolve_prereleases=True`) to pull one that way.

#### Developing against an unreleased coro

For day-to-day development on a consumer alongside coro itself, the simplest and
recommended approach is [editable mode](https://docs.conan.io/2/tutorial/developing_packages/editable_packages.html):

```bash
cd coro && conan editable add .
```

This points the `coro/*` reference straight at your local working tree — the consumer
builds directly against whatever you currently have checked out, with no packaging,
tagging, or version-range/prerelease configuration involved at all. It's the right
default whenever you have a local coro checkout to edit. (The `.dirty` handling in
`set_version()` below still applies while editable, so `package_id_non_embed_mode =
"patch_mode"` continues to force rebuilds as you edit, even without a real tag.)

The prerelease/version-range machinery below is for the cases editable mode can't cover —
CI, or anyone building a consumer without a local coro checkout to point at.

To opt in without editing any recipe, set the `core.version_ranges:resolve_prereleases`
conf, either directly on the command line:

```bash
conan install . -c core.version_ranges:resolve_prereleases=True
```

or as a reusable, composable profile fragment:

```ini
# profiles/allow-prereleases
[conf]
core.version_ranges:resolve_prereleases=True
```

```bash
conan install . -pr default -pr allow-prereleases
```

This conf is global — it affects prerelease resolution for every version range in the
graph, not just coro's — but since it only changes behavior for packages that actually
have prerelease versions available, it has no effect on dependencies that never publish
one.

For stricter per-package control — allowing prereleases for coro specifically while
keeping every other dependency pinned to a release version, even if one of them also
happens to publish prereleases — use a
[lockfile](https://docs.conan.io/2/tutorial/versioning/lockfiles.html) with a partial
override instead of the global conf:

1. Create a lockfile normally (prereleases off), pinning every dependency, including
   coro, to a resolved release version:

   ```bash
   conan lock create . --lockfile-out=conan.lock
   ```

2. Remove coro's entry from the lockfile's `requires` list (Conan lockfiles are JSON;
   there is no dedicated `conan lock remove` subcommand, so this is a manual edit).

3. Re-install with `--lockfile-partial` and prereleases enabled — every other dependency
   stays pinned to its locked release version, immune to the conf; only coro, having been
   removed from the lock, resolves fresh and picks up the latest prerelease:

   ```bash
   conan install . --lockfile=conan.lock --lockfile-partial \
       -c core.version_ranges:resolve_prereleases=True
   ```

#### No ABI stability across releases, including patches

coro does not guarantee binary compatibility between any two versions, patch releases
included. SemVer's usual patch-release connotation — "drop in the new binary without
recompiling" — doesn't hold for a template/header-heavy C++ library: consumers compile
directly against coro's headers, so there is no stable ABI boundary to preserve in the
first place, regardless of how carefully a given release is scoped. The version number
tracks *source/API* compatibility only — will existing valid consumer code keep compiling
and behaving the same — not binary interchangeability. A patch release may still change
header-visible implementation details; only changes to the observable API surface
(signatures, semantics) warrant a minor or major bump.

Consumers who link coro as a shared library (`coro`'s default) never get a stale cached
binary across a patch bump silently: the `coro` recipe sets
`package_id_non_embed_mode = "patch_mode"`, so any version change — patch included —
changes the `package_id` of anything depending on it and forces a rebuild. (This is
distinct from Conan's global default of `minor_mode` for this scenario, which would
otherwise treat patch bumps as binary-compatible and skip the rebuild — see
[`package_id_embed_mode` / `package_id_non_embed_mode`](https://docs.conan.io/2/reference/conanfile/attributes.html#package-id-embed-non-embed-python-unknown-mode-build-mode).
Consumers that embed coro statically or header-only already get this via Conan's
`full_mode` default for embedded dependencies.)

#### How the version is derived on non-tagged commits

Only exact-tagged commits get a clean version (`X.Y.Z`, or `X.Y.Z-rc.N` — see
[Release candidates](#release-candidates)). Everything else — every ordinary commit on
any branch, and any backport branch cut from an older release tag — derives its version
automatically from git history via the recipe's `set_version()`, using
`git describe --tags --long --dirty`:

- If the nearest reachable tag is a plain `vX.Y.Z` release tag, one component is bumped
  by one and a prerelease identifier is appended: `X.Y.(Z+1)-dev.N+gSHA` by default,
  where `N` is the commit count since that tag and `gSHA` is the short commit hash. Some
  bump (rather than reusing the last tag's number as-is) is required for correct SemVer
  precedence — a prerelease of `X.Y.Z` sorts *before* the plain release `X.Y.Z`, so
  reusing the last tag's number verbatim would make an in-progress build sort behind a
  version already shipped. Bumping first guarantees every prerelease sorts after the
  last real release, regardless of which component the next actual release turns out to
  bump — patch is only the default; see
  [Signaling a minor/major dev-build bump](#signaling-a-minormajor-dev-build-bump) for
  how to bump minor or major instead.
- If the nearest reachable tag is a `vX.Y.Z-rc.N` release-candidate tag, the patch is
  **not** bumped again — the rc tag already names the pending version — and a `dev.M`
  identifier is appended to it instead: `X.Y.Z-rc.N.dev.M+gSHA`, where `M` is the commit
  count since the rc tag. This still sorts correctly: SemVer gives a prerelease with more
  dot-separated fields higher precedence than a prefix-equal one with fewer, so
  `X.Y.Z-rc.N.dev.M` always sorts after `X.Y.Z-rc.N` itself.
- If the working tree has uncommitted changes, `.dirty` is appended to the identifier
  so two different uncommitted edits never collide on the same version string — this
  matters most for editable-mode local development, where an unchanged version despite
  changed headers would defeat `package_id_non_embed_mode = "patch_mode"` above.
- If a branch name can be resolved (see below), it's appended to the build-metadata
  component (the part after `+`) of either format above — `X.Y.(Z+1)-dev.N+gSHA.branchname`
  or `X.Y.Z-rc.N.dev.M+gSHA.branchname` — so a version string alone hints at where a build
  came from, without needing to look up `gSHA` in git first.

Because `git describe` walks commit ancestry rather than global tag chronology, backport
branches need no special-casing: branching from `v0.1.5`, committing a fix, and tagging
`v0.1.6` there computes correctly even if a newer `v0.2.0` already exists elsewhere —
it's simply not an ancestor of that branch.

This does mean `set_version()` requires: (a) at least one reachable tag to exist at all
(a one-time bootstrap requirement — the very first tag has to be created manually), and
(b) enough git history to actually be present — a shallow clone (the default in many CI
checkout actions, and possible locally too) can leave no tag reachable at all, in which
case `set_version()` fails loudly rather than guessing a fallback version.

Conan also re-evaluates the recipe later against its own cache-exported copy (e.g. when a
consumer resolves `coro/<version>` from the cache), which never has `.git` —
`exports_sources` only copies source files, not repository metadata. The recipe's
`export()` method covers this: it runs right after `set_version()`'s first (real-tree)
success and persists the computed version into `conandata.yml`'s `scm_version` key via
[`update_conandata()`](https://docs.conan.io/2/reference/tools/files/basic.html#update-conandata),
which — unlike `exports_sources` — is always copied into the cache. `set_version()` checks
for that key *before* attempting `git describe` (rather than trying git first and falling
back on failure), so every later cache-based evaluation reads the version straight back
out of there instead of spawning an always-doomed `git` subprocess first.

#### Signaling a minor/major dev-build bump

Bumping the patch component is only a *safe default*, not a claim about compatibility —
it guarantees correct ordering (a dev build always sorts after the last release and
before whatever comes next, whichever component that next release ends up bumping) but
says nothing about what the pending change actually is. A dev build that already
contains a breaking API change still reports itself as `X.Y.(Z+1)-dev.N+gSHA`, which
looks minor/patch-compatible with the last release. This is consistent with SemVer
itself — a prerelease's core version number was never meant to be a compatibility
promise about its own content ([spec item 9](https://semver.org/#spec-item-9)) — but it's
a real trap for anyone who's opted a version range into `resolve_prereleases=True`
(see above): a range like `coro/[~1.2]` would happily match a `1.2.4-dev.N` build that's
actually heading toward a breaking `2.0.0`.

To signal the correct component, add a tracked `conandata.yml` at the repo root:

```yaml
next_bump: minor  # or "major"; omit the key, or set "patch", for the default
```

`set_version()` reads `next_bump` (defaulting to `"patch"` when the key or the file is
absent) and bumps that component instead: `minor` produces `X.(Y+1).0-dev.N+gSHA`,
`major` produces `(X+1).0.0-dev.N+gSHA`. It's only consulted on this branch — exact tags
(release or rc) and the rc-tag dev-build branch above ignore it entirely, since those
already name their target version unambiguously.

This is deliberately a manual, low-ceremony signal rather than something derived
automatically (e.g. by scanning commit messages for a Conventional Commits
`BREAKING CHANGE:`/`feat:` marker): set it once when starting work you already know is
minor/major-worthy, and reset it to `patch` (or remove the key) once the real tag lands.
There's no enforcement if you forget — the worst case is that dev builds keep bumping the
wrong component until someone notices, which only affects the version *string* of
unreleased, not-yet-tagged builds. That's judged not worth solving with a commit hook or
CI check: the blast radius is cosmetic and self-corrects at the next real tag.

`conandata.yml` is also where `export()` persists the computed `scm_version` fallback
(see above) — `update_conandata()` merges rather than overwrites, and only ever writes to
the *exported cache copy*, never back into this tracked file, so the two uses coexist
without conflict.

#### Release candidates

Cutting a release candidate is a deliberate, named checkpoint — the same ceremony as
cutting a final release tag, just earlier and on any branch:

```bash
git tag v1.2.3-rc.1
```

The numeric identifier is dot-separated (`rc.1`, not `rc1`) so it compares numerically
per [SemVer's prerelease precedence rules](https://semver.org/#spec-item-11) —
`rc.2` sorts before `rc.10` — the same reason the `dev.N` identifier above is
dot-separated too. An exact rc tag produces a clean `X.Y.Z-rc.N` version with no
build metadata, identical in spirit to a final release tag: by the time something is
stable enough to name and hand to testers, it's no longer "an experimental build that
needs tracing back to a branch," so there's nothing useful for branch/commit metadata to
add. Bump `N` (`v1.2.3-rc.2`, `v1.2.3-rc.3`, ...) each time a new candidate is cut for
the same target release; there's no automatic rule for *when* to cut one — that's a
judgment call, not derived from commit count.

Once testing passes, merge the branch back and tag the final `v1.2.3` release as usual;
the existing exact-tag handling picks it up with no rc-specific logic involved.

Ordinary commits on the branch between rc tags still get a version automatically — see
the `X.Y.Z-rc.N.dev.M+gSHA` case above.

#### Metadata: branch name and dirty state

The `+gSHA` build-metadata component on non-tagged versions can carry two more
dot-separated fields, each added only when resolvable — the whole component is
diagnostic/informational and never affects version precedence or Conan resolution
([SemVer spec item 10](https://semver.org/#spec-item-10)):

- **Branch name.** Resolved in order: the CI-provided ref (`GITHUB_HEAD_REF` for PR
  builds, falling back to `GITHUB_REF_NAME` for direct branch builds — coro is hosted on
  GitHub, so no other CI provider needs to be special-cased), then `git branch
  --show-current` for local builds. Sanitized to SemVer's build-metadata charset
  (`[0-9A-Za-z-]`, so e.g. `feature/cool-thing` becomes `feature-cool-thing`) and
  truncated to 12 characters. If neither source resolves — a detached-HEAD checkout of a
  bare commit with no CI context — the field is simply omitted rather than guessed.
  `gSHA` alone already makes every build fully traceable via `git log`/`git branch
  --contains`; the branch name is a convenience on top of that, not a second source of
  truth, so it's fine for it to be best-effort rather than required.
- **Dirty state.** `.dirty` is appended when the working tree has uncommitted changes,
  as already covered above.

Exact tags (`vX.Y.Z` and `vX.Y.Z-rc.N`) never carry this metadata — a named, tagged
checkpoint doesn't need it, the tag itself is the identifying information.

#### Embedding the version in the built binary

The version is also made available to the C++ build itself (e.g. for a `--version`
output), but not via a required Conan-only CLI flag — the root `CMakeLists.txt` stays
agnostic to whether `find_package()` is resolving dependencies through Conan, system
packages, or anything else, and that same policy extends to the version. The conanfile's
`generate()` passes `self.version` through as the `CORO_VERSION` cache variable;
`CMakeLists.txt` configures `cmake/coro_version.h.in` into a generated
`include/coro/version.h` (installed alongside the rest of the package), falling back to
`"unknown"` if `CORO_VERSION` isn't defined — so a plain `cmake` configure never
hard-fails just because Conan wasn't involved in producing it:

```cpp
#include <coro/version.h>
std::puts(CORO_VERSION);  // "0.1.1-dev.3+gabc123.mybranch", or "unknown" outside Conan
```

#### Non-Conan builds

Building coro without Conan is not currently a supported workflow, but the CMake files
are kept Conan-agnostic deliberately (plain `find_package()` calls throughout) so that
door isn't closed off. Directly cloning the git repo and building the source outside of
Conan is not expected to work — there is no path that starts from a raw clone. Instead,
`conan install --deploy=...` is the one underlying mechanism, fully resolving and
materializing coro *and* its transitive dependencies as a self-contained, already-built
folder (including any generated version/config files) — used two ways:

- Run by a GitHub release job against an exact release tag and published as a tarball,
  for consumers who'd rather just download a finished, non-Conan copy of a specific
  version than run Conan themselves.
- Run locally by a consumer who wants a non-Conan copy of a version that doesn't have a
  published release tarball, or who has some other specific reason not to use one of
  the published tarballs.

Either way, the deploy step only ever runs where Conan (and coro's actual dependencies)
are available — what comes out the other side is Conan-free, not the process producing it.

### Roadmap

- [Roadmap and Future Plans](doc/roadmap.md)
