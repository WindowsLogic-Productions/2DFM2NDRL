#ifndef HOOKS_H
#define HOOKS_H

#include "globals.h"

// Hook setup/teardown
bool InitializeHooks();
void ShutdownHooks();

// Hook implementations (called by MinHook)
int __cdecl Hook_GetPlayerInput(int player_id, int input_type);
int __cdecl Hook_UpdateGameState();
void __cdecl Hook_RenderGame();
BOOL __cdecl Hook_RunGameLoop();
uint32_t __cdecl Hook_GameRand();
int __cdecl Hook_ProcessGameInputs();

#endif // HOOKS_H
