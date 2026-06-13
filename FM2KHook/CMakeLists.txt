cmake_minimum_required(VERSION 3.15)

# FM2K Hook DLL - must be 32-bit
project(FM2KHook)

# Force 32-bit build
set(CMAKE_GENERATOR_PLATFORM Win32)

# Enable debug symbols
set(CMAKE_BUILD_TYPE Debug)
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -g")

# Shared source list — both FM2KHook and FM95Hook are built from the same
# sources; only the engine ifdef in src/core/globals.h changes the address
# tables. See CLAUDE.md and FM95_Integration.h for the engine model.
set(FM2KHOOK_SOURCES
    # Core
    src/core/dllmain.cpp
    src/core/globals.cpp
    src/core/main_loop_trampoline.cpp
    # Hooks
    src/hooks/hooks.cpp
    src/hooks/input.cpp
    src/hooks/game_patches.cpp
    src/hooks/round_events.cpp
    src/hooks/css_autoconfirm.cpp
    src/hooks/per_game_patches.cpp
    src/hooks/csm_diag.cpp
    src/hooks/combat_state.cpp
    src/hooks/wndproc_subclass.cpp
    src/hooks/render_simd.cpp
    # Netplay
    src/netplay/netplay.cpp
    src/netplay/control_channel.cpp
    src/netplay/savestate.cpp
    src/netplay/spectator_node.cpp
    src/netplay/spectator_tcp.cpp
    src/netplay/nat_traversal.cpp
    src/netplay/game_hash.cpp
    src/netplay/upload_queue.cpp
    src/netplay/spec_relay_queue.cpp
    # Audio — Mike Z rollback-safe sound layer
    src/audio/sound_rollback.cpp
    # UI
    src/ui/imgui_overlay.cpp
    src/ui/fc_hud.cpp
    src/ui/shared_mem.cpp
    src/ui/input_binder.cpp
    src/ui/screenshot.cpp
    # Parity — kgtengine cross-engine ground-truth recorder
    src/parity/parity_recorder.cpp
    # Locale spoofing for Japanese-only games (CPW etc.)
    src/locale/locale_spoof.cpp
    # Shared PII scrubber for log lines (hook + launcher both use it).
    src/util/pii_scrub.cpp
    # STORED-method ZIP writer for desync/crash log bundling (ported
    # from /mnt/c/dev/bbbr/revolve_input_sdl3). No deps.
    src/util/zip_writer.cpp
    # SafetyHook + Zydis (amalgamated vendored singles). Used by the KOF
    # HP-init MidHook in per_game_patches.cpp. Skips the FetchContent /
    # subdirectory dance — we just compile the single-file amalgamations
    # directly into FM2KHook + FM95Hook.
    ${CMAKE_SOURCE_DIR}/vendored/safetyhook/amalgamated-dist/safetyhook.cpp
    ${CMAKE_SOURCE_DIR}/vendored/zydis-amalgamated/amalgamated-dist/Zydis.c
)

set(FM2KHOOK_INCLUDES
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core
    ${CMAKE_CURRENT_SOURCE_DIR}/src/hooks
    ${CMAKE_CURRENT_SOURCE_DIR}/src/netplay
    ${CMAKE_CURRENT_SOURCE_DIR}/src/audio
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui
    ${CMAKE_CURRENT_SOURCE_DIR}/src/locale
    ${CMAKE_SOURCE_DIR}/vendored/minhook/include
    ${CMAKE_SOURCE_DIR}/vendored/SDL/include
    ${CMAKE_SOURCE_DIR}/vendored/SDL_net/include
    ${CMAKE_SOURCE_DIR}/vendored/GekkoNet/include
    ${CMAKE_SOURCE_DIR}/vendored/GekkoNet/GekkoLib/include
    ${CMAKE_SOURCE_DIR}/vendored/imgui
    ${CMAKE_SOURCE_DIR}/vendored/quill/include
    # SafetyHook amalgamated (Zydis + SafetyHook bundled as single-file
    # sources). The amalgamated SafetyHook header pulls in Zydis via
    # __has_include("Zydis.h"), so adding the Zydis amalgamation dir to
    # the include path is sufficient — no FetchContent / subdirectory
    # dance + no cross-toolchain pain.
    ${CMAKE_SOURCE_DIR}/vendored/safetyhook/amalgamated-dist
    ${CMAKE_SOURCE_DIR}/vendored/zydis-amalgamated/amalgamated-dist
    # kgtengine parity-snapshot header (sibling repo at /mnt/c/dev/kgtengine).
    ${CMAKE_CURRENT_SOURCE_DIR}/../../kgtengine/include
)

set(FM2KHOOK_LIBS
    MinHook
    GekkoNet
    SDL3::SDL3-static
    SDL3_net::SDL3_net-static
    imgui_dx
    ws2_32
    winmm
    d3d9
    xinput9_1_0
    psapi
    advapi32  # CryptAcquireContext / CryptCreateHash for upload_queue SHA1
)

set(FM2KHOOK_DEFS
    FM2KHOOK_EXPORTS
    _CRT_SECURE_NO_WARNINGS
    WIN32_LEAN_AND_MEAN
    GEKKONET_STATIC
    _WIN32_WINNT=0x0601
)

# Helper macro — both targets are configured identically except for the
# ENGINE_FM95 flag and the output name.
function(fm2khook_configure_target target_name)
    target_compile_definitions(${target_name} PRIVATE ${FM2KHOOK_DEFS} ${ARGN})
    target_link_libraries(${target_name} PRIVATE ${FM2KHOOK_LIBS})
    target_include_directories(${target_name} PRIVATE ${FM2KHOOK_INCLUDES})
    set_target_properties(${target_name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    )
    install(TARGETS ${target_name}
        RUNTIME DESTINATION bin
        LIBRARY DESTINATION lib
        ARCHIVE DESTINATION lib
    )
endfunction()

# FM2K target — default engine (Fighter Maker 2nd).
add_library(FM2KHook SHARED ${FM2KHOOK_SOURCES})
fm2khook_configure_target(FM2KHook)

# FM95 target — Fighter Maker 95 prototype (CPW.exe and friends). Same source,
# different address tables via ENGINE_FM95 ifdef in src/core/globals.h. Always
# installs the locale spoof unconditionally (FM95-era games are Japanese-only).
add_library(FM95Hook SHARED ${FM2KHOOK_SOURCES})
fm2khook_configure_target(FM95Hook ENGINE_FM95=1)
set_target_properties(FM95Hook PROPERTIES OUTPUT_NAME "FM95Hook")

# render_simd.cpp vectorizes the FM2K software blit/blur with SSE2 intrinsics
# (<emmintrin.h>). The rest of the DLL is built unoptimized (-O0) for
# debuggability, but this is the ONE hot path that must beat the game's
# precompiled-optimized blit -- at -O0 the intrinsics spill to stack on every op
# and the blend helpers don't inline, making it slower than the original. So
# force -O3 + -msse2 for just this translation unit (32-bit MinGW defaults to
# x87/no __SSE2__). Integer SSE2 is bit-exact at any -O (no float/-ffast-math),
# re-verified via FM2K_BLIT_SIMD=simd,verify.
set_source_files_properties(src/hooks/render_simd.cpp PROPERTIES
    COMPILE_OPTIONS "-O3;-msse2")
