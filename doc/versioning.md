# Versioning and Releases

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

## Developing against an unreleased coro

For day-to-day development on a consumer alongside coro itself, the simplest and
recommended approach is [editable mode](https://docs.conan.io/2/tutorial/developing_packages/editable_packages.html):

```bash
cd coro && conan editable add .
```

By default, this registers the editable package under whatever version `set_version()`
currently computes for your working tree — the same prerelease derivation described below,
`.dirty` included, since it still runs `git describe` against the real checkout. That's a
problem: if the consumer requires coro via a version range (e.g. `coro/[>=1.0.0]`), Conan
silently skips the editable package for that requirement — prereleases are excluded from
range resolution by default, and the auto-derived dev version is always a prerelease.
Without further configuration, Conan resolves the range against a cached or remote release
instead, and your local edits are never built at all, with no error to flag that this
happened. A consumer that pins coro to an exact version has the same problem unless that
exact string happens to match the editable package's current derived version, which changes
on every commit and any uncommitted edit — not a realistic thing to keep pinned to.

**Recommended: register the package under an explicit version instead of the auto-derived
one**, one patch ahead of whatever's actually published, with some build-metadata suffix
appended to mark it as a dev build:

```bash
conan editable add . --version=0.1.3+dev   # if 0.1.2 is the latest real release
```

`+dev` here is just an example — the metadata string itself is arbitrary and doesn't matter
to Conan at all (`+local`, `+abc123`, `+yourname` all work identically); the only thing that
matters is that *some* metadata suffix is present, so the version doesn't read as a genuine
release string (see the `+dev` discussion further below for why that distinction is worth
keeping).

This works because Conan pre-populates `self.version` with an explicit CLI-supplied version
before calling `set_version()` — the recipe still has to defer to it explicitly, though:
`derive_coro_version()` checks `conanfile.version is not None` first and returns it as-is
before ever reaching the `git describe` logic. Build
metadata (the part after `+`) also never affects SemVer precedence or Conan's version
comparison, so `0.1.3+dev` is exactly as eligible for range resolution as a plain `0.1.3`
would be — nothing about it looks like a prerelease. Conan picks the highest version
satisfying a range among everything visible (editable + cache + remotes), so being one
patch ahead guarantees this package wins over the real `0.1.2` for any `coro/[>=...]`-style
requirement, with zero configuration needed on the consumer side — no `resolve_prereleases`,
no lockfile changes, nothing. Removing the override (`conan editable remove .`, or simply
not re-running `export`/`create` again) leaves only the real `0.1.2` to satisfy the range,
and resolution falls back to it automatically.

The same explicit-version override works identically outside editable mode, for
`conan export` and `conan create`:

```bash
conan create . --version=0.1.3+dev
```

The difference is what it's pointing at: `conan editable add` registers a live pointer to
your working tree, so every subsequent build picks up whatever you currently have on disk;
`conan export`/`conan create` copies the source into the cache at that moment, a snapshot —
new local edits aren't reflected until you re-run one of them, presumably bumping the
version again (`0.1.4+dev`) each time. Use editable mode for the usual day-to-day
inner loop; reach for `export`/`create` when you specifically want a real, buildable cache
entry under a synthetic dev version — e.g. to hand a colleague a pinnable reference, or to
test against what a consumer's build will actually resolve without a local coro checkout.

Passing `--version=` on every command gets old fast once you're also building the test suite
or examples against the same override — each of those is a separate Conan invocation with its
own command line to remember. Set `CORO_VERSION_OVERRIDE` in the environment instead and every
recipe in this repo (`conanfile.py` and `test/conanfile.py` both call `derive_coro_version()`)
picks it up automatically, with no `--version=` needed anywhere:

```bash
export CORO_VERSION_OVERRIDE=0.1.3+dev
conan editable add .
cd test && conan install . --build=missing && conan build .
```

It takes effect at exactly the same point the CLI override would — just below it in
precedence, so an explicit `--version=` on a given command still wins if both are set.

The `+dev` suffix isn't load-bearing for any of that — it's purely a readability safeguard.
Without it, a build registered as plain `coro/0.1.3` is indistinguishable from an eventual
real `0.1.3` release if it ever ends up installed somewhere it shouldn't (a machine you
forgot had editable mode on, a cache entry someone copied around) — nothing about the
version string itself would tell you it wasn't the genuine release. `+dev` costs nothing
and closes that gap.

!!! warning "WARNING: keep the number ahead of whatever actually ships"
    If `0.1.3` is later tagged for real while you're still mid-development, your override
    needs to move to `0.1.4+dev` (or later) or it stops winning range resolution against
    the new real release. This is manual upkeep, not something enforced — there's no error
    if you forget, just a silent fall-back to the newly-published version the next time you
    resolve.

### Making every auto-derived build resolvable, without a manual override

The override above requires remembering to bump it by hand. `CORO_VERSION_DEV_AS_METADATA`
gets the same range-resolvability out of the ordinary, no-override, `git describe`-derived
dev version, automatically, for every commit:

```bash
export CORO_VERSION_DEV_AS_METADATA=1
```

Normally, an auto-derived dev version puts the commit count in the *prerelease* part and the
commit sha in *build metadata* — `0.1.3-dev.5+g1a2b3c4`, say. The `-dev.5` there is what makes
it a prerelease, and thus excluded from range resolution by default (see below). With this
env var set, `derive_coro_version()` moves that same information entirely into build metadata
instead: `0.1.3+dev.5.g1a2b3c4`. Same coordinates — you can still tell at a glance which commit
and how many commits past the last tag a given build came from — but now there's no prerelease
part at all, so the version resolves against ranges exactly like a real `0.1.3` would, with no
override, no `resolve_prereleases`, nothing to remember to bump.

The tradeoff is exactly the one this section has been managing throughout: every commit's dev
build now silently satisfies a range as if it *were* the pending `0.1.3` release, whether or
not it actually behaves like one. That's fine for your own local inner loop where you know
what you're building against, which is why this is opt-in rather than the default — flip it
on in your own shell/`.envrc`, not in a shared CI profile where a consumer might resolve a
half-finished commit's build without realizing it isn't the tagged release.

This only helps consumers using a version range, though — a consumer pinning an *exact*
coro version still needs that exact string to match, same as the auto-derived case above.
For that case, or for CI, or for any consumer without a local coro checkout to point at,
fall back to enabling prereleases directly instead:

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
one. (A [lockfile](https://docs.conan.io/2/tutorial/versioning/lockfiles.html)-based
partial override can scope this to coro specifically if that global effect is ever a
problem, but the synthetic-version approach above already covers the common case well
enough that this is rarely worth reaching for.)

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

Consumers who link coro as a shared library (`coro`'s default) never get a stale cached
binary across a patch bump silently: the `coro` recipe sets
`package_id_non_embed_mode = "patch_mode"`, so any version change — patch included —
changes the `package_id` of anything depending on it and forces a rebuild. (This is
distinct from Conan's global default of `minor_mode` for this scenario, which would
otherwise treat patch bumps as binary-compatible and skip the rebuild — see
[`package_id_embed_mode` / `package_id_non_embed_mode`](https://docs.conan.io/2/reference/conanfile/attributes.html#package-id-embed-non-embed-python-unknown-mode-build-mode).
Consumers that embed coro statically or header-only already get this via Conan's
`full_mode` default for embedded dependencies.)

## How the version is derived on non-tagged commits

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

## Release candidates

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

## Embedding the version in the built binary

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

## Non-Conan builds

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
