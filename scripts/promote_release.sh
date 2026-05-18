#!/usr/bin/env bash
# Promote a dev pre-release to stable. Flips the GitHub release's
# prerelease flag and bumps LatestVersion so the stable-channel
# auto-updater picks it up.
#
# Usage:    ./scripts/promote_release.sh 0.2.53
#
# Requires the dev release tag to already exist (./scripts/cut_release.sh
# --dev was run previously for this version). Idempotent — running it
# twice just no-ops the second time.

set -euo pipefail

if [ $# -ne 1 ]; then
    echo "usage: $0 <version>     # e.g. $0 0.2.53" >&2
    exit 1
fi
VERSION="$1"

if ! command -v gh >/dev/null 2>&1; then
    echo "promote_release: gh CLI not installed" >&2
    exit 1
fi
if [ -z "${FM2KTEST_REPO_DIR:-}" ]; then
    echo "promote_release: set FM2KTEST_REPO_DIR=/path/to/fm2ktest checkout" >&2
    exit 1
fi
if [ ! -d "$FM2KTEST_REPO_DIR/.git" ]; then
    echo "promote_release: $FM2KTEST_REPO_DIR isn't a git repo" >&2
    exit 1
fi

echo "==> flipping v${VERSION} to non-prerelease (= stable)"
gh release edit "v${VERSION}" \
    --repo "Armonte/fm2ktest" \
    --prerelease=false \
    --latest

echo "==> bumping LatestVersion in $FM2KTEST_REPO_DIR"
( cd "$FM2KTEST_REPO_DIR" \
    && git pull --quiet \
    && echo "$VERSION" > LatestVersion \
    && git add LatestVersion \
    && git commit -m "promote v${VERSION} to stable" \
    && git push )

echo "==> posting Discord webhook (promotion announcement)"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
FM2K_RELEASE_WEBHOOK_OPTIONAL=1 \
    "$SCRIPT_DIR/post_release_webhook.sh" promote "$VERSION" \
    "v${VERSION} has been promoted to the **stable** channel." || \
    echo "    (webhook step failed but promotion itself succeeded — fix env then run:"
echo "    ./scripts/post_release_webhook.sh stable $VERSION 'notes...')"

echo
echo "v${VERSION} promoted to stable. stable-channel clients pick it up on next check."
