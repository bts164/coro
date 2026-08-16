# Shared derivation of coro's git-tag-based version string -- used by both
# conanfile.py (to set its own package version) and test/conanfile.py (to
# pin its `coro/<version>` requirement to exactly the version this same
# checkout would produce), so the two recipes can never silently drift out
# of sync the way a hand-maintained/hardcoded pin or a `coro/[*]` range
# requirement can -- see README.md's "Versioning and Releases".
#
# Deliberately a plain importable module, not a Conan `python_requires`
# package: python_requires exists for sharing recipe logic *across*
# repositories/remotes, with its own versioned package and its own
# resolution step. Both recipes here always live in, and are built from,
# the same git checkout, so a plain sibling-file import gets the same
# de-duplication without that machinery. Revisit this choice if this logic
# is ever needed by a recipe outside this repo -- python_requires would be
# the right tool at that point.

import re, os
import yaml
from conan.errors import ConanException
from conan.tools.scm import Git


def load_conandata(folder):
    """Reads conandata.yml from `folder` (e.g. next_bump — see
    conandata.yml's own comment), returning {} if the file doesn't exist.
    self.conan_data only auto-populates from the conandata.yml next to the
    recipe currently being evaluated, so test/conanfile.py (which needs
    *coro's* conandata.yml, one directory up from its own recipe) has to
    load it explicitly rather than relying on that.
    """
    path = os.path.join(folder, "conandata.yml")
    if not os.path.isfile(path):
        return {}
    with open(path) as f:
        return yaml.safe_load(f) or {}


def derive_coro_version(conanfile, folder, conandata=None):
    """Returns coro's version string for the git checkout at `folder`.

    Mirrors what conanfile.py's set_version() has always done: read
    conandata['scm_version'] if present (the cache-exported-copy fallback —
    see conanfile.py's comment on set_version() for why that's needed),
    otherwise run `git describe` against `folder` and derive an
    exact/dev/rc version from the nearest reachable vX.Y.Z tag. Raises
    ConanException if no such tag is reachable.
    """
    cached = (conandata or {}).get("scm_version")
    if cached:
        return cached

    git = Git(conanfile, folder=folder)
    try:
        describe = git.run(
            "describe --tags --long --dirty --match v[0-9]*.[0-9]*.[0-9]*"
        ).strip()
    except Exception as e:
        raise ConanException(
            "coro: `git describe` could not find a reachable vX.Y.Z tag "
            f"({e}). Either no such tag exists yet in this history (create "
            "one, e.g. `git tag v0.1.0`), or this is a shallow clone that "
            "doesn't include it — fetch full history/tags (e.g. "
            "`git fetch --unshallow --tags`, or `fetch-depth: 0` / "
            "`GIT_DEPTH: 0` in CI)."
        )

    dirty = describe.endswith("-dirty")
    if dirty:
        describe = describe[: -len("-dirty")]

    # The --match glob above also matches "vX.Y.Z-rc.N" tags (the trailing
    # "*" absorbs the "-rc.N" suffix), so a release-candidate tag can be
    # the "nearest reachable tag" here just like a plain release tag.
    tag, count, _sha_with_g = describe.rsplit("-", 2)
    sha = _sha_with_g[1:]  # strip git describe's "g" prefix on the short hash

    match = re.fullmatch(r"v(\d+)\.(\d+)\.(\d+)(?:-rc\.(\d+))?", tag)
    if not match:
        raise ConanException(
            f"coro: tag '{tag}' found by `git describe` doesn't match the "
            "expected vX.Y.Z or vX.Y.Z-rc.N format"
        )
    major, minor, patch, rc = match.groups()
    major, minor, patch = int(major), int(minor), int(patch)
    exact = count == "0" and not dirty

    if rc is not None:
        # rc tags already name the pending release, so the patch is not
        # bumped again — see README.md's "Release candidates".
        version = f"{major}.{minor}.{patch}-rc.{rc}"
        if not exact:
            version += f".dev.{count}+g{sha}"
    else:
        if exact:
            version = f"{major}.{minor}.{patch}"
        else:
            # See README.md's "Signaling a minor/major dev-build bump" —
            # patch is only a safe-for-ordering default, not a claim that
            # the pending change is actually patch-compatible.
            next_bump = (conandata or {}).get("next_bump", "patch")
            if next_bump == "major":
                major, minor, patch = major + 1, 0, 0
            elif next_bump == "minor":
                minor, patch = minor + 1, 0
            elif next_bump == "patch":
                patch += 1
            else:
                raise ConanException(
                    f"coro: conandata.yml's next_bump is '{next_bump}', "
                    "expected 'patch', 'minor', or 'major'"
                )
            version = f"{major}.{minor}.{patch}-dev.{count}+g{sha}"

    if not exact:
        # Build metadata only — never affects SemVer precedence or Conan
        # resolution. See README.md's "Metadata: branch name and dirty
        # state". Exact tags (release or rc) carry none of this.
        branch = _branch_metadata(git)
        if branch:
            version += f".{branch}"
        if dirty:
            version += ".dirty"

    return version


# Best-effort branch name for build metadata, resolved in the order
# documented in README.md: the CI-provided ref first (coro is hosted on
# GitHub, so only GitHub Actions' env vars need handling), then a local
# git query. Returns None (metadata omitted, not guessed) if neither
# resolves, e.g. a detached-HEAD checkout of a bare commit with no CI
# context.
def _branch_metadata(git):
    branch = os.environ.get("GITHUB_HEAD_REF") or os.environ.get("GITHUB_REF_NAME")
    if not branch:
        try:
            branch = git.run("branch --show-current").strip()
        except Exception:
            branch = None
    if not branch:
        return None
    sanitized = re.sub(r"[^0-9A-Za-z-]", "-", branch)[:12].strip("-")
    return sanitized or None
