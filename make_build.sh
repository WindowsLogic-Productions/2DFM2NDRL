#!/bin/bash
set -e
set -x

# Check for required packages
if ! command -v i686-w64-mingw32-gcc >/dev/null 2>&1; then
    echo "MinGW-w64 cross compiler not found. Installing required packages..."
    sudo apt update
    sudo apt install -y gcc-mingw-w64-i686 g++-mingw-w64-i686 binutils-mingw-w64-i686 ninja-build
fi

# Self-heal the submodule deps so a fresh clone (even one that forgot
# --recursive) builds. GekkoNet and the kgt-parity header are flattened
# in-tree, so they need no init here. SDL_image's nested image-codec
# submodules are initialized SELECTIVELY -- only the formats the build uses
# (PNG = libpng + zlib, JPEG, TIFF) -- so we don't drag in the huge,
# force-disabled AVIF/JXL/WEBP codec repos (aom, dav1d, libavif, libjxl,
# libwebp), which would add hundreds of MB to every clone for nothing.
if git rev-parse --git-dir >/dev/null 2>&1; then
    git submodule sync --quiet
    git submodule update --init \
        vendored/SDL vendored/SDL_image vendored/imgui vendored/minhook vendored/miniupnp
    git -C vendored/SDL_image submodule update --init \
        external/jpeg external/libpng external/libtiff external/zlib
fi

# Create build directory if it doesn't exist
if [ ! -d "build" ]; then
    rm -rf build # remove build directory if it exists
    mkdir build # create new build directory
fi

cd build # change to build directory

# Load build secrets (FM2K_LOG_UPLOAD_SECRET etc.) into the environment so
# CMake bakes them at configure time. Without this the crash/desync auto-upload
# secret bakes empty and EVERY upload is 4xx-rejected (telemetry blackout).
# `set +x` around the source so the secret is never echoed to the build log.
if [ -f "$HOME/.config/fm2k-release.env" ]; then
    set +x
    set -a
    . "$HOME/.config/fm2k-release.env"
    set +a
    set -x
fi

# Force CMake to run
cmake .. -G "Ninja" -D CMAKE_BUILD_TYPE=RelWithDebInfo \
    -D CMAKE_C_COMPILER=i686-w64-mingw32-gcc \
    -D CMAKE_CXX_COMPILER=i686-w64-mingw32-g++ \
    -D CMAKE_ASM_COMPILER=i686-w64-mingw32-gcc \
    -D CMAKE_RC_COMPILER=i686-w64-mingw32-windres \
    -D CMAKE_SYSTEM_NAME=Windows \
    -D CMAKE_SYSTEM_PROCESSOR=i686 \
    -D SDL_DISABLE_WINDOWS_VERSION_RESOURCE=ON \
    -D CMAKE_C_FLAGS="-w -static-libgcc" \
    -D CMAKE_CXX_FLAGS="-w -static-libgcc -static-libstdc++" \
    -D CMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER \
    -D CMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY \
    -D CMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY

echo "CMake configuration completed. Run 'bash go.sh' to build." 