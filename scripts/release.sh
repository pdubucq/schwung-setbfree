#!/usr/bin/env bash
# Publish a GitHub release with the module tarball attached.
#
# Usage:  ./scripts/release.sh 0.1.1
#         ./scripts/release.sh 0.1.1 --draft          # review on GitHub first
#         ./scripts/release.sh 0.1.1 --skip-tests     # skip the native test run
#         ./scripts/release.sh 0.1.1 --skip-build     # upload the existing dist/ tarball (implies --skip-tests)
#         ./scripts/release.sh 0.1.1 --notes "text"   # default: generated from commits
#
# Needs the GitHub CLI (`gh`, logged in via `gh auth login`) and Docker (or an
# aarch64 toolchain) for the build.
#
# The version must already be in src/schwung_bfree/module.json — that file is
# what ends up on the device, so a release whose tag disagrees with its manifest
# would install as the wrong version. Bump it, commit, push, then run this.
#
# Asset: setbfree-organ-module.tar.gz. The name carries no version on purpose, so
# .../releases/latest/download/setbfree-organ-module.tar.gz always resolves.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
cd "$REPO_ROOT"

fail() { echo "FAIL: $*" >&2; exit 1; }

VERSION=""
DRAFT=0
SKIP_TESTS=0
SKIP_BUILD=0
NOTES=""

while [ $# -gt 0 ]; do
    case "$1" in
        --draft)       DRAFT=1 ;;
        --skip-tests)  SKIP_TESTS=1 ;;
        --notes)       shift; [ $# -gt 0 ] || fail "--notes needs a value"; NOTES="$1" ;;
        --skip-build)  SKIP_BUILD=1 ;;
        -h|--help)     sed -n '2,18p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        -*)            fail "unknown option: $1" ;;
        *)             [ -z "$VERSION" ] || fail "version given twice"; VERSION="${1#v}" ;;
    esac
    shift
done

[ -n "$VERSION" ] || fail "usage: ./scripts/release.sh <version>   e.g. 0.1.1"
echo "$VERSION" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.]+)?$' \
    || fail "'$VERSION' is not a semantic version (expected X.Y.Z)"

TAG="v$VERSION"
MODULE_JSON="src/schwung_bfree/module.json"
ASSET="dist/setbfree-organ-module.tar.gz"

# ---------------------------------------------------------------------
# Preconditions. All of these are cheap and all of them are cheaper to find out
# before a 5-minute Docker build than after.
# ---------------------------------------------------------------------
command -v gh >/dev/null 2>&1 || fail "GitHub CLI not found. Install it from https://cli.github.com/"
gh auth status >/dev/null 2>&1 || fail "gh is not logged in. Run: gh auth login"

git diff --quiet && git diff --cached --quiet \
    || fail "uncommitted changes to tracked files. Commit or stash them — a release must be reproducible from a commit."

BRANCH="$(git rev-parse --abbrev-ref HEAD)"
[ "$BRANCH" = "main" ] || fail "on branch '$BRANCH'. Release from main."

git fetch --quiet origin main --tags
[ "$(git rev-parse HEAD)" = "$(git rev-parse origin/main)" ] \
    || fail "HEAD differs from origin/main. Push (or pull) first, so the tag lands on a commit that exists on GitHub."

if git rev-parse -q --verify "refs/tags/$TAG" >/dev/null || \
   git ls-remote --exit-code --tags origin "refs/tags/$TAG" >/dev/null 2>&1; then
    fail "tag $TAG already exists."
fi

MANIFEST_VERSION="$(sed -n 's/.*"version"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$MODULE_JSON" | head -1)"
[ "$MANIFEST_VERSION" = "$VERSION" ] \
    || fail "$MODULE_JSON says version '$MANIFEST_VERSION', not '$VERSION'. Update it, commit and push first."

echo "=== releasing $TAG from $(git rev-parse --short HEAD) ==="

# ---------------------------------------------------------------------
# Test, then build. The tarball is rebuilt from scratch — never upload whatever
# happens to be in dist/ from an earlier run.
# ---------------------------------------------------------------------
if [ "$SKIP_BUILD" -eq 1 ]; then
    echo "--- skipping tests and build (--skip-build): using existing $ASSET ---"
    [ -f "$ASSET" ] || fail "$ASSET does not exist. Build first (./scripts/build.sh) or drop --skip-build."
else
    if [ "$SKIP_TESTS" -eq 0 ]; then
        ./scripts/test.sh
    else
        echo "--- skipping tests (--skip-tests) ---"
    fi

    ./scripts/build.sh

    [ -f "$ASSET" ] || fail "$ASSET was not produced by the build."
fi

# The thing on GitHub must be the thing the manifest describes.
tar -xzOf "$ASSET" setbfree-organ/module.json | grep -q "\"version\"[[:space:]]*:[[:space:]]*\"$VERSION\"" \
    || fail "$ASSET contains a module.json that is not version $VERSION."

echo ""
echo "=== uploading $ASSET ($(du -h "$ASSET" | cut -f1)) ==="

ARGS=(release create "$TAG" "$ASSET"
      --target "$(git rev-parse HEAD)"
      --title "setBfree Organ $VERSION")

if [ -n "$NOTES" ]; then
    ARGS+=(--notes "$NOTES")
else
    ARGS+=(--generate-notes)
fi
[ "$DRAFT" -eq 1 ] && ARGS+=(--draft)
case "$VERSION" in *-*) ARGS+=(--prerelease) ;; esac

gh "${ARGS[@]}"

echo ""
echo "Released $TAG"
[ "$DRAFT" -eq 1 ] && echo "(draft — publish it from the GitHub releases page)"
exit 0
