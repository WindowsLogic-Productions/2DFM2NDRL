#ifndef HOOKS_H
#define HOOKS_H

#include "globals.h"

// Hook setup/teardown
bool InitializeHooks();
void ShutdownHooks();

// Hook implementations (called by MinHook)
int __cdecl Hook_GetPlayerInput(int player_id, int input_type);
// FM95 has SEPARATE per-player input fns — single arg `(player_idx)`. We
// hook both so the binder + facing-flip layer applies on FM95 too. The
// player_idx the host calls with is 1 (P1) and 2 (P2), used as multiplier
// into g_p_facing_snap[25*idx].
int __cdecl Hook_GetPlayerInput_FM95_P1(int player_idx);
int __cdecl Hook_GetPlayerInput_FM95_P2(int player_idx);
int __cdecl Hook_UpdateGameState();
void __cdecl Hook_RenderGame();
BOOL __cdecl Hook_RunGameLoop();
uint32_t __cdecl Hook_GameRand();
int __cdecl Hook_ProcessGameInputs();

// Engine-aware phase detection — FM2K reads g_game_mode magic numbers
// (2000=CSS, 3000+=Battle); FM95 walks the object pool for type==19/16
// with sub_state range. Defined in hooks.cpp; consumed by main_loop_
// trampoline.cpp's ClassifyPhase + the CSS/battle transition checks.
bool IsCSSMode(uint32_t mode);
bool IsBattleMode(uint32_t mode);

// SOCD (Simultaneous Opposite Cardinal Direction) cleaner — strips
// nonsense input combinations like L+R or U+D per the currently-active
// SOCD mode (env FM2K_SOCD_MODE, runtime override via Hook_SetSOCDMode).
// Exposed so netplay.cpp can pre-apply on the host side BEFORE storing
// into the spectator stream — eliminates SOCD-mode-mismatch between
// host and a spectator/replay using a different mode as a source of
// sim divergence. The spec re-applies on top (idempotent for the
// resolved input).
uint16_t Hook_ApplySOCD_Public(uint16_t input);

#endif // HOOKS_H
