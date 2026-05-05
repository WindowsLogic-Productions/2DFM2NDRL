#!/usr/bin/env bash
# End-to-end release cutter. Runs the full publish flow in one shot:
#
#   1. ./go.sh              build + deploy locally
#   2. package_release.sh   zip the artifacts into dist/fm2k_v<ver>.zip
#   3. gh release create    upload the zip + notes to GitHub
#   4. update LatestVersion in the public fm2ktest checkout, push
#
# Required setup (one-time):
#   - gh auth login
#   - clone fm2ktest somewhere:    git clone https://github.com/Armonte/fm2ktest ~/projects/fm2ktest
#   - export FM2KTEST_REPO_DIR=~/projects/fm2ktest
#
# Usage:    ./scripts/cut_release.sh                  # version comes from make_version.sh
#           ./scripts/cut_release.sh "release notes"  # title is "v<ver>"
#
# Bump the version IN scripts/make_version.sh BEFORE running this.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

VERSION="$(grep -E '^FM2K_VERSION=' "$SCRIPT_DIR/make_version.sh" | head -1 | cut -d'"' -f2)"
if [ -z "$VERSION" ]; then
    echo "cut_release: couldn't read version from scripts/make_version.sh" >&2
    exit 1
fi

NOTES="${1:-Released v${VERSION}}"

if ! command -v gh >/dev/null 2>&1; then
    echo "cut_release: gh CLI not installed (https://cli.github.com)" >&2
    exit 1
fi
if [ -z "${FM2KTEST_REPO_DIR:-}" ]; then
    echo "cut_release: set FM2KTEST_REPO_DIR=/path/to/fm2ktest checkout" >&2
    exit 1
fi
if [ ! -d "$FM2KTEST_REPO_DIR/.git" ]; then
    echo "cut_release: $FM2KTEST_REPO_DIR isn't a git repo" >&2
    exit 1
fi

echo "==> building"
( cd "$REPO_ROOT" && ./go.sh )

echo "==> packaging"
"$SCRIPT_DIR/package_release.sh" "$VERSION"
ZIP_PATH="$REPO_ROOT/dist/fm2k_v${VERSION}.zip"

echo "==> creating gh release v${VERSION}"
gh release create "v${VERSION}" "$ZIP_PATH" \
    --repo "Armonte/fm2ktest" \
    --title "v${VERSION}" \
    --notes "$NOTES"

echo "==> bumping LatestVersion in $FM2KTEST_REPO_DIR"
( cd "$FM2KTEST_REPO_DIR" \
    && git pull --quiet \
    && echo "$VERSION" > LatestVersion \
    && git add LatestVersion \
    && git commit -m "release v${VERSION}" \
    && git push )

echo
echo "released v${VERSION}. clients should pick it up on next launcher start."
