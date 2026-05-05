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
    NATIVE,             // Menu / intro / results — full original flow reproduced
    CSS,                // Character select — control-channel-lockstep, no GekkoNet
    TRAMPOLINE_BATTLE,  // GekkoNet drives sim + save + load + advance
    SPECTATOR_PLAYBACK  // Inputs sourced from spectator stream queue, no GekkoNet
};

// Entry point replacing main_game_loop wholesale. Returns the same BOOL the
// original did: non-zero on normal exit via WM_QUIT, zero on abnormal.
BOOL TrampolineMainLoop();

// [EB]-OPCODE DIAGNOSTIC: log shake-effect timer + camera position at the
// given render-boundary phase (PRE-SAVE / PRE-RENDER / POST-RENDER /
// POST-RESTORE). Gated on FM2K_EB_DIAG=1 env var; output goes to a per-PID
// `FM2K_eb_diag_pid<PID>.log` next to the game EXE (NOT the main launcher
// log). Used by both the trampoline render path AND Hook_RenderGame so
// FM2K_BYPASS_TRAMPOLINE=1 still produces diag output for A/B testing
// against the trampoline path. Cheap when env not set (single static-bool
// branch). Frame counter advances on each PRE-SAVE call.
void EbDiag_Dump(const char* tag);
