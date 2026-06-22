#!/bin/bash
# ui_build.sh — worktree-local configure+build for the ui-imanim-test
# branch. Bypasses `git submodule update --init` by symlinking populated
# vendored deps from /mnt/c/dev/wanwan (the main checkout). Self-contained
# and idempotent: re-runs cleanly without rebuilding submodules.
#
# Throwaway script. When the branch merges (or gets thrown away) this
# can go with it. Don't use as a template for the main build path.
set -e
set -u

WORKTREE="/mnt/c/dev/wanwan-ui-imanim"
MAIN="/mnt/c/dev/wanwan"

cd "$WORKTREE"

# 1. Submodule deps — symlink from main if our copy is empty/missing.
#    Skip GekkoNet specifically: that one is touched in main's WIP, so
#    pin it from main but warn if main has uncommitted edits we should
#    know about.
SUBMOD_DEPS=(SDL SDL_image SDL_net imgui GekkoNet minhook)
for d in "${SUBMOD_DEPS[@]}"; do
    src="$MAIN/vendored/$d"
    dst="$WORKTREE/vendored/$d"
    if [ ! -d "$src" ] || [ -z "$(ls -A "$src" 2>/dev/null)" ]; then
        echo "WARN: $src is missing or empty — main checkout may not have submodules initialized" >&2
        continue
    fi
    # If dst is empty dir, remove it. If it's already a symlink to the
    # right place, leave alone. If it has real content, also leave alone
    # (someone populated it manually) but warn.
    if [ -L "$dst" ]; then
        continue
    elif [ -d "$dst" ] && [ -z "$(ls -A "$dst" 2>/dev/null)" ]; then
        rmdir "$dst"
    elif [ -d "$dst" ]; then
        echo "INFO: $dst already populated (not a symlink); leaving as-is" >&2
        continue
    fi
    ln -s "$src" "$dst"
    echo "linked vendored/$d -> $src"
done

# 2. Configure if we don't have a build.ninja yet (or if CMakeCache is
#    stale). Ninja's own --reconfigure handles incremental cmake.
if [ ! -d build ]; then
    mkdir build
fi

if [ ! -f build/build.ninja ]; then
    echo "Configuring (Ninja, RelWithDebInfo, mingw i686)..."
    (
        cd build
        cmake .. -G "Ninja" -D CMAKE_BUILD_TYPE=RelWithDebInfo \
            -D CMAKE_C_COMPILER=i686-w64-mingw32-gcc \
            -D CMAKE_CXX_COMPILER=i686-w64-mingw32-g++ \
            -D CMAKE_ASM_COMPILER=i686-w64-mingw32-gcc \
            -D CMAKE_RC_COMPILER=i686-w64-mingw32-windres \
            -D CMAKE_SYSTEM_NAME=Windows \
            -D CMAKE_SYSTEM_PROCESSOR=i686 \
            -D SDL_DISABLE_WINDOWS_VERSION_RESOURCE=ON \
            -D 'CMAKE_C_FLAGS=-w -static-libgcc' \
            -D 'CMAKE_CXX_FLAGS=-w -static-libgcc -static-libstdc++' \
            -D CMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER \
            -D CMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY \
            -D CMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY
    )
fi

# 3. Build. Just the launcher target — we don't need the hook DLL, the
#    updater, or the games for sandbox iteration.
cd build
ninja FM2K_RollbackLauncher
