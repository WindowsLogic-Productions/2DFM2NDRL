#!/bin/bash
set -e
set -x
clear
cd build
ninja || {
    echo "Build failed!"
    exit 1
}

cp FM2K_RollbackLauncher.exe FM2KHook.dll /mnt/c/games/
cp FM2KHook.dll /mnt/c/games/2dfm/wanwan/   # Windows resolves DLL from CPW.exe's dir first

echo "Build completed successfully!"
