#!/usr/bin/env bash
# End-to-end release cutter. Runs the full publish flow in one shot:
#
#   1. ./go.sh              build + deploy locally
#   2. package_release.sh   zip the artifacts into dist/fm2k_v<ver>.zip
#   3. gh release create    upload the zip + notes to GitHub
#                           (with --prerelease on the dev channel)
#   4. (stable only) update LatestVersion in fm2ktest checkout, push
#
# Required setup (one-time):
#   - gh auth login
#   - clone fm2ktest somewhere:    git clone https://github.com/Armonte/fm2ktest ~/projects/fm2ktest
#   - export FM2KTEST_REPO_DIR=~/projects/fm2ktest
#
# Usage:
#   ./scripts/cut_release.sh                            # STABLE release
#   ./scripts/cut_release.sh "release notes"            # STABLE w/ notes
#   ./scripts/cut_release.sh --dev                      # DEV pre-release
#   ./scripts/cut_release.sh --dev "release notes"      # DEV w/ notes
#   ./scripts/cut_release.sh --bleeding                 # BLEEDING pre-release
#   ./scripts/cut_release.sh --bleeding "release notes" # BLEEDING w/ notes
#
# Bump the version IN scripts/make_version.sh BEFORE running this.
#
# Channel mechanics:
#   - Stable cuts a non-prerelease GH release AND bumps LatestVersion
#     (= what /releases/latest returns to the launcher's auto-updater).
#   - Dev cuts a PRE-release GH tag. LatestVersion is NOT touched, so
#     stable-channel clients ignore it. Dev-channel clients ask for
#     /releases?per_page=20 and pick the first plain (non-bleeding)
#     prerelease entry — that's this build.
#   - Bleeding cuts a PRE-release GH tag SUFFIXED "-bleeding"
#     (v0.2.58-bleeding) so the launcher routes it to the bleeding
#     channel only -- dev/stable clients never see it. It also SKIPS the
#     Discord webhook: bleeding is the silent fast lane, no @notify spam.
#     Bump make_version.sh AHEAD of the current stable/dev before cutting
#     (the version compare ignores the "-bleeding" suffix, so a bleeding
#     build at the same number as stable won't be offered as an update).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

CHANNEL="stable"
if [ "${1:-}" = "--dev" ]; then
    CHANNEL="dev"
    shift
elif [ "${1:-}" = "--bleeding" ]; then
    CHANNEL="bleeding"
    shift
fi

VERSION="$(grep -E '^FM2K_VERSION=' "$SCRIPT_DIR/make_version.sh" | head -1 | cut -d'"' -f2)"
if [ -z "$VERSION" ]; then
    echo "cut_release: couldn't read version from scripts/make_version.sh" >&2
    exit 1
fi

# The version that lands in the GH tag, zip name, and download URL. For
# bleeding it carries the tier-marker suffix so the launcher's parser
# routes it to the bleeding channel; the binary's own kAppVersion stays
# the plain number (make_version.sh isn't told about the suffix).
TAG_VERSION="$VERSION"
if [ "$CHANNEL" = "bleeding" ]; then
    TAG_VERSION="${VERSION}-bleeding"
fi

NOTES="${1:-Released v${TAG_VERSION}}"

if ! command -v gh >/dev/null 2>&1; then
    echo "cut_release: gh CLI not installed (https://cli.github.com)" >&2
    exit 1
fi

# Refuse to release from a dirty working tree. We learned the hard
# way (v0.2.49 -> v0.2.57): cutting from `git status`-dirty state
# means the released zip and the git history disagree, and you can't
# bisect or diff what shipped vs what's local. Force commit-first.
#
# Override with FM2K_RELEASE_ALLOW_DIRTY=1 for emergencies where the
# release truly must go out before a commit can be made -- but the
# message above ("we learned the hard way") is the default for a
# reason. Don't use the override casually.
if [ -z "${FM2K_RELEASE_ALLOW_DIRTY:-}" ]; then
    DIRTY="$(cd "$REPO_ROOT" && git status --porcelain | grep -vE '^.. vendored/SDL_image$' || true)"
    if [ -n "$DIRTY" ]; then
        echo "cut_release: working tree is dirty -- refuse to release a phantom build." >&2
        echo "" >&2
        echo "  modified / untracked files (top 20):" >&2
        echo "$DIRTY" | head -20 | sed 's/^/    /' >&2
        echo "" >&2
        echo "  commit (or stash, or gitignore) these first, then re-run." >&2
        echo "  override (NOT recommended) with FM2K_RELEASE_ALLOW_DIRTY=1" >&2
        exit 1
    fi
fi
# Stable channel needs the fm2ktest checkout for the LatestVersion bump.
# Dev channel skips the bump — only the GH pre-release tag matters; the
# launcher's dev-channel updater finds it via the /releases API.
if [ "$CHANNEL" = "stable" ]; then
    if [ -z "${FM2KTEST_REPO_DIR:-}" ]; then
        echo "cut_release: set FM2KTEST_REPO_DIR=/path/to/fm2ktest checkout" >&2
        exit 1
    fi
    if [ ! -d "$FM2KTEST_REPO_DIR/.git" ]; then
        echo "cut_release: $FM2KTEST_REPO_DIR isn't a git repo" >&2
        exit 1
    fi
fi

echo "==> building (channel=${CHANNEL})"
( cd "$REPO_ROOT" && ./go.sh )

echo "==> packaging"
"$SCRIPT_DIR/package_release.sh" "$TAG_VERSION"
ZIP_PATH="$REPO_ROOT/dist/fm2k_v${TAG_VERSION}.zip"

echo "==> creating gh release v${TAG_VERSION} (channel=${CHANNEL})"
GH_FLAGS=()
# Both dev and bleeding ship as GH pre-releases; only the tag suffix
# (handled via TAG_VERSION) tells the launcher which of the two it is.
if [ "$CHANNEL" = "dev" ] || [ "$CHANNEL" = "bleeding" ]; then
    GH_FLAGS+=(--prerelease)
fi
gh release create "v${TAG_VERSION}" "$ZIP_PATH" \
    --repo "Armonte/fm2ktest" \
    --title "v${TAG_VERSION}" \
    --notes "$NOTES" \
    "${GH_FLAGS[@]}"

if [ "$CHANNEL" = "stable" ]; then
    echo "==> bumping LatestVersion in $FM2KTEST_REPO_DIR"
    ( cd "$FM2KTEST_REPO_DIR" \
        && git pull --quiet \
        && echo "$VERSION" > LatestVersion \
        && git add LatestVersion \
        && git commit -m "release v${VERSION}" \
        && git push )
fi

# Discord webhook announcement — extracted to its own script so it can
# also be run standalone (./scripts/post_release_webhook.sh stable
# 0.2.54) if a webhook send is missed. Env vars come from
# ~/.config/fm2k-release.env which post_release_webhook.sh sources
# itself — no per-shell rc setup needed. Optional: a release-step
# failure here is non-fatal (release itself already succeeded), so we
# set OPTIONAL=1 to keep the channel-flag schema permissive.
#
# Bleeding is the silent fast lane: it skips the webhook entirely so it
# never pings the release channel. That's the whole reason the tier
# exists -- iterate without spamming @notify.
if [ "$CHANNEL" = "bleeding" ]; then
    echo "==> skipping Discord webhook (bleeding is silent)"
else
    echo "==> posting Discord webhook"
    FM2K_RELEASE_WEBHOOK_OPTIONAL=1 \
        "$SCRIPT_DIR/post_release_webhook.sh" "$CHANNEL" "$VERSION" "$NOTES" || \
        echo "    (webhook step failed but release itself succeeded — fix env then run:"
    echo "    ./scripts/post_release_webhook.sh $CHANNEL $VERSION 'notes...')"
fi

echo
if [ "$CHANNEL" = "stable" ]; then
    echo "released v${VERSION} (stable). all clients pick it up on next launcher start."
elif [ "$CHANNEL" = "bleeding" ]; then
    echo "released v${TAG_VERSION} (bleeding pre-release, no webhook)."
    echo "Only bleeding-channel clients see it. Stable/dev clients ignore it."
    echo "When it soaks well, cut it forward as a dev build: bump make_version.sh, then"
    echo "  ./scripts/cut_release.sh --dev 'notes'"
else
    echo "released v${VERSION} (dev pre-release)."
    echo "Stable-channel clients ignore this. Dev-channel clients pick it up on next check."
    echo "To promote to stable later:  ./scripts/promote_release.sh ${VERSION}"
fi
