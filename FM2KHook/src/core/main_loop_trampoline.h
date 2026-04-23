// Main-loop trampoline — replaces the game's native main_game_loop @0x405AD0.
//
// During battle under GekkoNet the trampoline IS the game loop. During CSS
// and menus it emulates the native loop's structure so gameplay code that
// relies on main_game_loop's prologue writes keeps working. This eliminates
// the cadence mismatch between forward-sim and rollback-replay that was
// causing object_pool divergence — main_game_loop's per-iteration state
// writes never happen on forward any more either, so there's nothing to
// mismatch against.
//
// Hook chain: Hook_RunGameLoop @ FM2K::ADDR_RUN_GAME_LOOP (0x405AD0) detours
// to TrampolineMainLoop(). No calls to original_run_game_loop — we own the
// outer loop.
#pragma once
#include <windows.h>
#include <cstdint>

enum class LoopPhase {
    NATIVE,           // Menu / intro / results — full original flow reproduced
    CSS,              // Character select — control-channel-lockstep, no GekkoNet
    TRAMPOLINE_BATTLE // GekkoNet drives sim + save + load + advance
};

// Entry point replacing main_game_loop wholesale. Returns the same BOOL the
// original did: non-zero on normal exit via WM_QUIT, zero on abnormal.
BOOL TrampolineMainLoop();
