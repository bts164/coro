# Versioning and Releases

coro follows [Semantic Versioning](https://semver.org). A released version is an annotated
git tag of the form `vX.Y.Z`, and a release candidate is one of the form `vX.Y.Z-rc.N`.
There are no separate, persistent release branches.

A release tag records that a release happened; it does not start one. Both kinds of tag
are created by a manually dispatched release workflow, after its build and tests pass, at
the commit it tested. A tag that exists therefore always marks a commit that passed, and
a failed attempt leaves no tag and uses up no version number. See
[Cutting a release](#cutting-a-release).

What a release publishes is source. The GitHub release for a tag carries the source
archive of that commit, with the version recorded inside it. It builds with Conan or
without, and needs no git history. See [Source archives](#source-archives) and
[Non-Conan builds](#non-conan-builds).

The version is derived from git, not from a hand-maintained string:

- On an exact `vX.Y.Z` release tag, the version is the clean `X.Y.Z`.
- On an exact `vX.Y.Z-rc.N` release-candidate tag, the version is the clean `X.Y.Z-rc.N`.
- On any other commit — on any branch — the version is a SemVer
  [prerelease](https://semver.org/#spec-item-9) identifier derived from `git describe`:
  the next unreleased version (patch-bumped by default; see
  [Signaling a minor/major dev-build bump](#signaling-a-minormajor-dev-build-bump)),
  the commit count and hash since the nearest reachable tag, and optional branch-name /
  dirty-state metadata. See
  [How the version is derived on non-tagged commits](#how-the-version-is-derived-on-non-tagged-commits)
  for the exact format in each case.

The same string is the Conan package version and the `CORO_VERSION` compiled into the
library, so a version seen anywhere names the commit it was built from.

This means every commit — on `main` or any other branch — is a valid, individually
addressable Conan package; there is no separate "nightly" or "bleeding edge" branch to
maintain. Consumers who reference an exact version (`coro/X.Y.Z-dev.N+gSHA`) can always
pull a specific prerelease build. Consumers who use a
[version range](https://docs.conan.io/2/tutorial/versioning/version_ranges.html)
(e.g. `coro/[>=1.0.0]`) never see prereleases unintentionally — Conan excludes prerelease
versions from range resolution by default, and a consumer must opt in explicitly
(`resolve_prereleases=True`) to pull one that way.

## Pre-1.0: minor/patch discipline is relaxed for API additions

coro is currently `0.y.z`. Per [SemVer's own spec item 4](https://semver.org/#spec-item-4),
major version zero is for initial development, "anything MAY change at any time," and the
public API "SHOULD NOT be considered stable" — so, while at `0.y.z`, coro does not follow
the usual convention of a minor bump for every backward-compatible API addition. A change
that breaks existing consumer code will usually still get a minor bump, but this isn't a
hard rule at this stage — it's a case-by-case judgment call, and a purely additive change
(new function, new overload, new type) may just as reasonably ship as a patch bump instead.
This is not a departure from SemVer, it's the escape hatch the spec grants for this exact
stage — the intent is to avoid the minor-bump-per-commit treadmill that would otherwise
discourage cutting real releases while the API is still actively growing.

!!! note "NOTE: this reasoning stops applying at 1.0.0"
    Once coro tags `1.0.0`, this relaxation ends. Consumers pinning Conan version ranges
    (`coro/[>=1.0.0]`) will expect patch bumps to mean "safe, no new surface" from then on,
    so the usual minor-bump-for-additions discipline is expected to resume at that point,
    not fade out gradually.

## No ABI stability across releases, including patches

coro does not guarantee binary compatibility between any two versions, patch releases
included. SemVer's usual patch-release connotation — "drop in the new binary without
recompiling" — doesn't hold for a template/header-heavy C++ library: consumers compile
directly against coro's headers, so there is no stable ABI boundary to preserve in the
first place, regardless of how carefully a given release is scoped. The version number
tracks *source/API* compatibility only — will existing valid consumer code keep compiling
and behaving the same — not binary interchangeability. A patch release may still change
header-visible implementation details; only changes to the observable API surface
(signatures, semantics) warrant a minor or major bump.

Conan therefore rebuilds a consumer whenever the coro it requires changes at all. The
`coro` recipe sets `package_id_non_embed_mode = "full_mode"`, which puts coro's whole
version, its recipe revision and its own `package_id` into the `package_id` of anything
that requires it. A change to any of them makes the consumer's existing binary a
different package, so Conan builds it again and does not reuse the old one.

This setting covers two kinds of consumer: one that links coro as a shared library
(`coro`'s default), and a static library that requires a static coro, which is every
library on a Pico. An executable or shared library that links coro statically gets the
same treatment from Conan's own default for embedded dependencies, which is also
`full_mode`. See
[`package_id_embed_mode` / `package_id_non_embed_mode`](https://docs.conan.io/2/reference/conanfile/attributes.html#package-id-embed-non-embed-python-unknown-mode-build-mode).

!!! warning "WARNING: the mode must compare the whole version"
    Conan's default for this case, `minor_mode`, and the stricter `patch_mode` both
    compare only the numeric part of the version. Every commit between two releases has
    the same numeric part: the commits differ only in the `-dev.N+gSHA` suffix (see
    below). Under either mode a consumer built before such a commit is reused against
    the coro built after it. If that commit changed inline code in a header, the link
    fails with undefined references to coro symbols, or succeeds and misbehaves.

## How the version is derived on non-tagged commits

Only exact-tagged commits get a clean version (`X.Y.Z`, or `X.Y.Z-rc.N` — see
[Cutting a release](#cutting-a-release)). Everything else — every ordinary commit on
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
  changed headers would defeat `package_id_non_embed_mode = "full_mode"` above.
- If a branch name can be resolved (see
  [Metadata: branch name and dirty state](#metadata-branch-name-and-dirty-state) below),
  it's appended to the build-metadata component (the part after `+`) of either format
  above — `X.Y.(Z+1)-dev.N+gSHA.branchname` or `X.Y.Z-rc.N.dev.M+gSHA.branchname` — so a
  version string alone hints at where a build came from, without needing to look up
  `gSHA` in git first.

Because `git describe` walks commit ancestry rather than global tag chronology, backport
branches need no special-casing: branching from `v0.1.5`, committing a fix, and tagging
`v0.1.6` there computes correctly even if a newer `v0.2.0` already exists elsewhere —
it's simply not an ancestor of that branch.

This does mean `set_version()` requires, in a git checkout: (a) at least one reachable
tag to exist at all (a one-time bootstrap requirement — the very first tag has to be
created manually), and (b) enough git history to actually be present — a shallow clone
(the default in many CI checkout actions, and possible locally too) can leave no tag
reachable at all, in which case `set_version()` fails loudly rather than guessing a
fallback version. A [source archive](#source-archives) needs neither: the result of
`git describe` is already written into it.

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

## Signaling a minor/major dev-build bump

Bumping the patch component is only a *safe default*, not a claim about compatibility —
it guarantees correct ordering (a dev build always sorts after the last release and
before whatever comes next, whichever component that next release ends up bumping) but
says nothing about what the pending change actually is. A dev build that already
contains a breaking API change still reports itself as `X.Y.(Z+1)-dev.N+gSHA`, which
looks minor/patch-compatible with the last release. This is consistent with SemVer
itself — a prerelease's core version number was never meant to be a compatibility
promise about its own content ([spec item 9](https://semver.org/#spec-item-9)) — but it's
a real trap for anyone who's opted a version range into `resolve_prereleases=True`
(see [Developing against an unreleased coro](#developing-against-an-unreleased-coro)
below): a range like `coro/[~1.2]` would happily match a `1.2.4-dev.N` build that's
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

## Metadata: branch name and dirty state

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

## Source archives

A source archive is what `git archive` produces, and what GitHub generates for every tag
and release ("Source code (tar.gz)"). It has no `.git`, so nothing in it can ask git for
the version. git records the version while it makes the archive:

- `.git_archival.txt` holds two placeholders. `.gitattributes` marks the file
  `export-subst`, which makes git replace them, in an archive only, with the
  `git describe` result and the full commit hash.
- `conan_version.py` and `cmake/CoroVersion.cmake` read the `describe` line and derive the
  version from it by the rules above.

In an archive the file reads, for example:

```text
describe: v0.1.1-29-ge6f8db2
commit: e6f8db21f6801e4f275f003fe7120722a4485e4a
```

In a git checkout it still holds the unexpanded placeholders, and is ignored.

An archive differs from a checkout of the same commit in three ways:

- **No branch name and never `.dirty`.** An archive is of a commit, so the version above
  is `0.1.2-dev.29+ge6f8db2`, with nothing after the hash.
- **Repository-only files are left out.** `.gitattributes` marks `.github`, `.gitignore`
  and itself `export-ignore`.
- **A commit with two tags reports the final one.** Promoting a candidate puts `vX.Y.Z`
  on the commit that already has `vX.Y.Z-rc.N`, and git names the newer annotated tag. A
  candidate's archive downloaded after the promotion therefore carries the final
  version. It is the same source.

To make one locally:

```bash
mkdir /tmp/coro_src
git archive HEAD | tar -x -C /tmp/coro_src
```

`git archive` packs a commit, not the working tree: uncommitted changes are not in it.

## Where the version comes from

The Conan recipes (`conanfile.py` and `test/conanfile.py`, through `conan_version.py`)
take the version from the first of these that applies:

| Order | Source | Applies to |
|---|---|---|
| 1 | `--version=` on the Conan command line | That command |
| 2 | `.git_archival.txt` | A source archive |
| 3 | `CORO_VERSION_OVERRIDE` in the environment | Every checkout the shell touches |
| 4 | `.coro_version_override` in the repository root | That checkout |
| 5 | `scm_version` in the exported `conandata.yml` | The recipe's copy in the Conan cache |
| 6 | `git describe` | A git checkout |

An archive's stamp comes before the two overrides because an archive knows its own
version: an override left set for some other checkout must not replace it. The overrides
are for editable mode; see
[Developing against an unreleased coro](#developing-against-an-unreleased-coro).

A build that Conan is not driving takes it from `-DCORO_VERSION=<version>`, then
`.git_archival.txt`, then `git describe`, and reports `unknown` when none is available.
See [Non-Conan builds](#non-conan-builds).

!!! warning "WARNING: two implementations of one rule"
    `cmake/CoroVersion.cmake` repeats the derivation in `conan_version.py`, so that both
    kinds of build print the same string for the same source. A change to one must be
    made in the other.

## Cutting a release

A release is cut by dispatching the release workflow by hand. The workflow releases
**the commit it is dispatched on**: it builds and tests that commit, and only if that
passes does it create the tag there and publish the GitHub release. If it fails, nothing
is tagged.

The workflow is `.github/workflows/release.yml`.

It takes two inputs, the kind of release (`rc` or `final`) and the target version
(`X.Y.Z`), and GitHub lets a workflow be dispatched from a branch or from a tag. That
gives three ways to use it:

| To cut | Dispatch from | Kind | Result on success |
|---|---|---|---|
| A release candidate | A branch | `rc` | Tags `vX.Y.Z-rc.N`, publishes a GitHub pre-release |
| A final release, promoting a candidate | The tag `vX.Y.Z-rc.N` | `final` | Tags `vX.Y.Z` at the same commit, publishes the release |
| A final release, with no candidate | A branch | `final` | Tags `vX.Y.Z`, publishes the release |

In every case:

- The tests run with the version the tag will give, so what is tested reports the same
  version as what is released.
- The tag is annotated.
- The GitHub release carries the source archive of the tagged commit (see
  [Source archives](#source-archives)). No binaries are published.

Pick the target `X.Y.Z` by what changed since the last release; see
[Pre-1.0](#pre-10-minorpatch-discipline-is-relaxed-for-api-additions) and
[No ABI stability](#no-abi-stability-across-releases-including-patches).

Before it runs any test, the workflow checks the request and stops if:

- the version is not of the form `X.Y.Z`;
- `vX.Y.Z` already exists;
- the version is not the next patch, minor or major version after the latest release
  reachable from the commit (after `v0.1.1`, only `0.1.2`, `0.2.0` and `1.0.0`);
- for a final release, the version has candidates and the commit is not the latest one
  (see [Final releases](#final-releases)).

It does not check the branch.

To try the workflow without releasing anything, set its `dry_run` input: the checks and
the tests run, and no tag or release is created.

### Release candidates

A candidate is a checkpoint worth testing more widely than CI does: by hand, on real
hardware, or by a consumer trying `coro/1.2.3-rc.1` ahead of the release.

- The workflow picks `N`: one more than the highest existing `vX.Y.Z-rc.N`, or 1.
- The branch need not be `main`. A candidate can be cut before its branch merges.
- If testing finds a problem, fix it on the same branch and cut another candidate. There
  is no limit on how many a release goes through.

Commits after a candidate get `X.Y.Z-rc.N.dev.M+gSHA`; see
[How the version is derived on non-tagged commits](#how-the-version-is-derived-on-non-tagged-commits).

### Final releases

Promoting a candidate tags `vX.Y.Z` on the candidate's own commit. Nothing new is
compiled between "tested as a candidate" and "released": they are the same source.

A candidate is optional. For a change that needs no wider testing, dispatch a `final`
release straight from the branch.

One rule ties the two together: if any `vX.Y.Z-rc.N` tag exists, a final `vX.Y.Z` must
be cut on the commit that the latest of them names, and the workflow refuses any other.
So a branch that has moved past its latest candidate cannot be released as it stands:
cut another candidate, or dispatch from the candidate's tag. An older, superseded
candidate cannot be promoted either.

!!! note "NOTE: the dispatched commit must contain the workflow"
    GitHub runs a dispatched workflow as it exists on the ref it is dispatched from. A
    commit older than the release workflow cannot be released this way.

!!! warning "WARNING: nothing restricts a final release to `main`"
    Backport branches need to release, and a candidate may be cut before its branch
    merges, so the workflow does not check the branch. Releasing from an unmerged feature
    branch is not prevented.

## Developing against an unreleased coro

For day-to-day development on a consumer alongside coro itself, use
[editable mode](https://docs.conan.io/2/tutorial/developing_packages/editable_packages.html)
with a fixed version:

```bash
cd coro
echo "0.1.3+dev" > .coro_version_override   # if 0.1.2 is the latest real release
conan editable add .
```

`.coro_version_override` is gitignored, holds one line, and applies to this checkout
only. Every recipe in the repository reads it (`conanfile.py` and `test/conanfile.py`
both call `derive_coro_version()`), so the library, its tests and a consumer all agree on
the version with no `--version=` on any command. Conan prints a warning naming the file
whenever it is in effect.

The fixed version is needed for two reasons:

- **Conan identifies an editable package by its exact version.** The derived version
  changes on every commit and on every uncommitted edit, and each change orphans the
  registration until the old version is removed and the new one added.
- **The derived version is a prerelease.** A consumer that requires coro by a version
  range (e.g. `coro/[>=1.0.0]`) skips it, because Conan leaves prereleases out of range
  resolution by default. It resolves a cached or remote release instead, and the local
  edits are never built, with no error.

`0.1.3+dev` avoids both. It is one patch ahead of the latest release, so it wins range
resolution over that release. The `+dev` part is build metadata: Conan ignores it when
comparing versions, so the version is as eligible for a range as a plain `0.1.3`. The
text after `+` is arbitrary. It is there so that the version cannot be mistaken for the
real `0.1.3` if the build ends up somewhere it should not.

!!! warning "WARNING: keep the number ahead of whatever actually ships"
    If `0.1.3` is later released while you are still developing, move the override to
    `0.1.4+dev`. Otherwise it stops winning range resolution against the new release.
    Nothing reports this: the next resolve silently takes the published version.

!!! note "NOTE: the override hides which commit a build came from"
    With an override in effect the version no longer names a commit or shows `.dirty`.
    That is the price of a version that stays put. Remove the file to get the derived
    version back.

### Other ways to set the version

- **`CORO_VERSION_OVERRIDE` in the environment** does the same as the file, for every
  checkout the shell touches. It suits CI. In a shell profile or `.envrc` it also applies
  to checkouts it was not meant for, which is why the file is preferred for development.
  It takes precedence over the file.
- **`--version=` on a Conan command** overrides everything, for that command. Use it for
  a one-off snapshot in the cache, for example to hand someone a pinnable reference:

    ```bash
    conan create . --version=0.1.3+dev
    ```

    Unlike an editable package, this copies the source as it is now. Later edits are not
    reflected until it is run again.

A source archive ignores both overrides and honours only `--version=`; see
[Where the version comes from](#where-the-version-comes-from).

### Consuming a derived prerelease version

A consumer without a local checkout, or CI, can use a derived version directly. Pinned
exactly (`coro/0.1.3-dev.5+g1a2b3c4`), it needs nothing more. To let a version range
match prereleases, set the `core.version_ranges:resolve_prereleases` conf, either on the
command line:

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

This conf is global: it affects every version range in the graph, not only coro's. It
changes nothing for a dependency that publishes no prerelease versions.

## Embedding the version in the built binary

The version is available to C++ code, for example for a `--version` output:

```cpp
#include <coro/version.h>
std::puts(CORO_VERSION);  // "0.1.2-dev.3+gabc123.mybranch"
```

`CMakeLists.txt` generates `coro/version.h` from `cmake/coro_version.h.in` and installs
it with the other headers. The value is the CMake variable `CORO_VERSION`:

- A Conan build passes it in. The recipe's `generate()` sets it to the package version.
- Any other build derives it in `cmake/CoroVersion.cmake`, from the source archive's
  stamp or from git, and prints it at configure time. See
  [Where the version comes from](#where-the-version-comes-from).

The numeric part (`X.Y.Z`) also becomes the CMake project version.

## Non-Conan builds

Building coro with plain CMake is partly in place and not yet a supported workflow. A
release's source archive is the starting point: it needs neither git nor Conan to know
its version.

What works today:

- **The version.** A plain `cmake` configure of an archive or a checkout reports the
  same version a Conan build would.
- **The options.** They are ordinary CMake options: `WITH_GPERFTOOLS`,
  `CORO_USE_LOCAL_RUN_QUEUE`, `WITH_SANITIZE`, `CORO_PLATFORM`. The recipe's `shared` and
  `fPIC` options correspond to CMake's own `BUILD_SHARED_LIBS` and
  `CMAKE_POSITION_INDEPENDENT_CODE`.
- **Where each dependency comes from.** Each has a script in `cmake/deps/` and a
  `CORO_<DEPENDENCY>_PROVIDER` option, set one dependency at a time. The only provider
  so far is `package`: the script calls `find_package()`, and the environment must
  already make the dependency findable. A Conan build uses this provider.

| Dependency | Option | Needed | Version |
|---|---|---|---|
| libwebsockets | `CORO_LIBWEBSOCKETS_PROVIDER` | Always | 4.3.5 or later, below 5, built without libuv |
| gperftools | `CORO_GPERFTOOLS_PROVIDER` | With `WITH_GPERFTOOLS` | 2.17.2 |

What is missing:

!!! tip "TODO: a provider that fetches and builds a dependency"
    A second provider would download and build a pinned version with CMake's
    `FetchContent`, for a build with no prepared environment. It goes in the same
    `cmake/deps/` script as a second branch.

!!! tip "TODO: the C++ standard"
    The desktop target does not state the standard it needs; a Conan profile supplies
    it. A plain CMake build gets the compiler's default.

!!! tip "TODO: a CMake package for consumers"
    There is no `install(EXPORT)` and no package config file, so another CMake project
    cannot find an installed coro with `find_package(coro)`. Conan generates that file
    for its own consumers. The tests and examples use `find_package(coro)`, so they
    build only under Conan.

!!! tip "TODO: Pico"
    A Pico consumer gets its targets through build modules that Conan loads. Nothing
    replaces them in a plain CMake build yet.

None of this has been exercised: no CI job builds coro without Conan.
