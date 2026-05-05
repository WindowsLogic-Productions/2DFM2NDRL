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
cp FM2K_RollbackLauncher.exe FM2KHook.dll FM2KUpdater.exe "$STAGED_DIR/"
i686-w64-mingw32-strip --strip-debug "$STAGED_DIR/FM2K_RollbackLauncher.exe"
i686-w64-mingw32-strip --strip-debug "$STAGED_DIR/FM2KHook.dll"
i686-w64-mingw32-strip --strip-debug "$STAGED_DIR/FM2KUpdater.exe"

cp "$STAGED_DIR/FM2K_RollbackLauncher.exe" "$STAGED_DIR/FM2KHook.dll" "$STAGED_DIR/FM2KUpdater.exe" /mnt/c/games/
cp "$STAGED_DIR/FM2KHook.dll" /mnt/c/games/2dfm/wanwan/   # Windows resolves DLL from CPW.exe's dir first
[ -d /mnt/c/games/2dfm/pkmncc ] && cp "$STAGED_DIR/FM2KHook.dll" /mnt/c/games/2dfm/pkmncc/

rm -rf "$STAGED_DIR"

echo "Build completed successfully!"
echo "Deployed (stripped) sizes:"
ls -la /mnt/c/games/FM2K_RollbackLauncher.exe /mnt/c/games/FM2KHook.dll | awk '{printf "  %-40s %s\n", $NF, $5}'
