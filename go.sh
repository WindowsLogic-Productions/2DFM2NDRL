#!/bin/bash
set -e
set -x
clear
cd build
ninja || {
    echo "Build failed!"
    exit 1
}

# Strip debug info from the deployed copies — DWARF debug is ~25 MB per
# binary at default (Debug) build settings. Stripping cuts each to ~5-8 MB
# without touching the unstripped originals in build/, so we can still
# pull symbols (addr2line / objdump) when diagnosing logs from users.
STAGED_DIR=$(mktemp -d)
cp FM2K_RollbackLauncher.exe FM2KHook.dll FM95Hook.dll FM2KUpdater.exe "$STAGED_DIR/"
i686-w64-mingw32-strip --strip-debug "$STAGED_DIR/FM2K_RollbackLauncher.exe"
i686-w64-mingw32-strip --strip-debug "$STAGED_DIR/FM2KHook.dll"
i686-w64-mingw32-strip --strip-debug "$STAGED_DIR/FM95Hook.dll"
i686-w64-mingw32-strip --strip-debug "$STAGED_DIR/FM2KUpdater.exe"

# Kill any running launcher / game / updater before overwrite. WSL drvfs
# doesn't propagate Windows file locks, so cp would silently succeed
# against a running .exe — disk gets new bytes but the in-memory
# process is still old, and tests run against the old code. /T cascades
# to spawned game children. `|| true` keeps a clean machine from failing.
taskkill.exe /F /T /IM FM2K_RollbackLauncher.exe 2>/dev/null || true
taskkill.exe /F /T /IM FM2KUpdater.exe           2>/dev/null || true
taskkill.exe /F /T /IM pkmncc.exe                2>/dev/null || true
taskkill.exe /F /T /IM CPW.exe                   2>/dev/null || true
sleep 0.3   # let Windows release file handles before cp

# Launcher distribution dir — both DLLs sit alongside the launcher exe so
# FM2KGameInstance::GetDLLPath(engine) picks the right one per game.engine.
cp "$STAGED_DIR/FM2K_RollbackLauncher.exe" \
   "$STAGED_DIR/FM2KHook.dll" \
   "$STAGED_DIR/FM95Hook.dll" \
   "$STAGED_DIR/FM2KUpdater.exe" \
   /mnt/c/games/

# Per-game copies (Windows resolves the DLL from the game's own dir first
# when LoadLibrary is called via the remote thread). Drop both DLLs to
# /mnt/c/games/2dfm/wanwan so we can iterate quickly on either engine.
cp "$STAGED_DIR/FM2KHook.dll" /mnt/c/games/2dfm/wanwan/
cp "$STAGED_DIR/FM95Hook.dll" /mnt/c/games/2dfm/wanwan/
[ -d /mnt/c/games/2dfm/pkmncc ] && cp "$STAGED_DIR/FM2KHook.dll" /mnt/c/games/2dfm/pkmncc/
# FM95 reference dir (clean CPW build) — ships FM95Hook.dll for end-to-end test.
[ -d /mnt/c/games/2dfm/fm95/CPW ] && cp "$STAGED_DIR/FM95Hook.dll" /mnt/c/games/2dfm/fm95/CPW/

# Sync the locale .ini files alongside the launcher. T() reads them from
# <exedir>\locales at runtime; stale deployed copies cause new translation
# keys to render as the literal key name in the UI.
mkdir -p /mnt/c/games/locales
cp /mnt/c/dev/wanwan/locales/*.ini /mnt/c/games/locales/

rm -rf "$STAGED_DIR"

echo "Build completed successfully!"
echo "Deployed (stripped) sizes:"
ls -la /mnt/c/games/FM2K_RollbackLauncher.exe /mnt/c/games/FM2KHook.dll | awk '{printf "  %-40s %s\n", $NF, $5}'
