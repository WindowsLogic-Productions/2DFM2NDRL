#!/usr/bin/env bash
# Build + run the FM2K unit tests on native Linux gcc. Fast iteration loop
# for the wire format / queue logic — does NOT exercise the actual hook DLL
# (that's the mingw-cross + Wine path used by go.sh).
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p build
cd build
cmake .. -G Ninja -DCMAKE_BUILD_TYPE=Debug "$@"
ninja
./fm2k_tests --reporters=console
