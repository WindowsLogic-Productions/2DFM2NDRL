# DISCLAIMER

YOU CANNOT COPYRIGHT AI GENERATED CODE AS IT USES SOURCES WHICH ARE COPYRIGHTED AND IS UNETHICAL, WINDOWSLOGIC PRODUCTIONS OWNS NOTHING HERE AND IS PRESERVING IT FOR PROSPERITY REASONS. THIS FORKED REPOSITORY WAS ORIGINALLY UNDER THE MIT LICENSE WHICH ALLOWS FORKING AND MODIFICATION. GEKKONET IS OPEN-SOURCE AND ALL GEKKONET CODE IN THIS FORKED REPO IS STOLEN, SAME WITH THE SOUND ENGINE, NETWORKING AND ANY 2D FIGHTER MAKER 2ND. ENGINE CODE. THIS USED CODE IS WRITTEN BY AN AI CALLED CLAUDE CODE MADE BY THE COMPANY ANTHROPIC AND MAKES NO MENTION OR REFERENCE TO THE USED CODE OR IF ANY PERMISSION HAS BEEN GIVEN.

- GekkoNet is under the BSD 2-Clause license and is Copyright (c) 2024-2026, Jamie Meyer
- 2D Fighter Maker 2nd. is Copyright (c) 2001 Kadokawa (Formally Enterbrain).

# wanwan

rollback launcher for fighter maker games (fm2k/fm95). fm95 support not finished
yet. spawns the game and injects a hook dll that does the save-state rollback,
online play, spectating and replays. windows 32-bit only, cross-compiled from
wsl/linux with mingw-w64 (i686) -- injection + the 32-bit game abi are why.
native linux isn't wired up.

## build

need wsl2 or linux with the i686 mingw toolchain:

    sudo apt install -y gcc-mingw-w64-i686 g++-mingw-w64-i686 binutils-mingw-w64-i686 cmake ninja-build git

then:

    git clone --recursive https://github.com/Armonte/wanwan.git
    cd wanwan
    ./make_build.sh    # inits submodules + runs cmake
    ./build.sh         # compiles, stripped binaries land in dist/

forgot --recursive? make_build.sh self-heals it. gekkonet and the one kgt parity
header are vendored in-tree as plain files so they need no submodule step.
SDL, SDL_image, imgui, minhook and miniupnp are submodules make_build.sh pulls
for you (and only the jpeg/png/tiff image codecs, not the big unused
avif/jxl/webp ones).

outputs land in dist/: FM2K_RollbackLauncher.exe, FM2KHook.dll, FM95Hook.dll,
FM2KUpdater.exe. unstripped copies with full debug info stay in build/ for
symbolication. build/ is incremental, don't nuke it between builds.

## run

drop the launcher + both hook dlls in a folder, point it at your fm2k games, go.
both dlls ship together so it picks the right one per engine (fm2k vs fm95/cpw).
deeper notes in docs/.

## deploy (maintainer only)

go.sh builds then copies into my local windows games tree (/mnt/c/games). that
half skips itself on any machine without that tree, so go.sh just builds for
everyone else. the normal flow is make_build.sh && build.sh.

## license

source-available, NOT open source. the launcher + hooks (my own code) are under
the PolyForm Noncommercial License 1.0.0 with a revocation addendum -- see
[LICENSE](LICENSE). short version: personal/noncommercial use, modification and
noncommercial redistribution are fine; commercial use needs a separate license;
the grant is revocable. bundled third-party components keep their own (permissive)
licenses -- see [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md). the native
frontend uses Slint under its royalty-free license.
