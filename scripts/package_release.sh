#!/usr/bin/env bash
# Bundles the latest build into fm2k_v<version>.zip for upload to a
# GitHub release. Invoked manually after `./go.sh` succeeds. Looks up
# the version code from scripts/make_version.sh so the zip name and
# the launcher's reported version always stay in lockstep.
#
# Usage:   ./scripts/package_release.sh
#          ./scripts/package_release.sh 0.1.1   # override version
#
# Output is dropped in dist/ next to the repo root.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$REPO_ROOT/build"
OUT_DIR="$REPO_ROOT/dist"

if [ "$#" -ge 1 ]; then
    VERSION="$1"
else
    VERSION="$(grep -E '^FM2K_VERSION=' "$SCRIPT_DIR/make_version.sh" | head -1 | cut -d'"' -f2)"
fi
if [ -z "$VERSION" ]; then
    echo "package_release: couldn't derive version" >&2
    exit 1
fi

LAUNCHER="$BUILD_DIR/FM2K_RollbackLauncher.exe"
# Ninja drops the hook DLL alongside the launcher (single-config build).
# Paths are relative to the project root because we cd'd here above.
HOOK="$BUILD_DIR/FM2KHook.dll"
UPDATER="$BUILD_DIR/FM2KUpdater.exe"

for f in "$LAUNCHER" "$HOOK" "$UPDATER"; do
    if [ ! -f "$f" ]; then
        echo "package_release: missing $f — run ./go.sh first" >&2
        exit 1
    fi
done

mkdir -p "$OUT_DIR"
STAGED="$(mktemp -d)"
cp "$LAUNCHER" "$HOOK" "$UPDATER" "$STAGED/"
i686-w64-mingw32-strip --strip-debug "$STAGED/FM2K_RollbackLauncher.exe"
i686-w64-mingw32-strip --strip-debug "$STAGED/FM2KHook.dll"
i686-w64-mingw32-strip --strip-debug "$STAGED/FM2KUpdater.exe"

ZIP_NAME="fm2k_v${VERSION}.zip"
ZIP_PATH="$OUT_DIR/$ZIP_NAME"
rm -f "$ZIP_PATH"
( cd "$STAGED" && zip -9 -j "$ZIP_PATH" \
    FM2K_RollbackLauncher.exe FM2KHook.dll FM2KUpdater.exe )

rm -rf "$STAGED"

echo "wrote $ZIP_PATH"
echo
echo "next steps:"
echo "  1. cut release:  gh release create v${VERSION} ${ZIP_PATH} --title v${VERSION} --notes 'release notes'"
echo "  2. update LatestVersion in armonte/fm2ktest:"
echo "       (in fm2ktest checkout) echo '${VERSION}' > LatestVersion && git commit -am 'release v${VERSION}' && git push"
