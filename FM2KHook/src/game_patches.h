#ifndef GAME_PATCHES_H
#define GAME_PATCHES_H

#include <cstdint>

void BypassMultiInstanceCheck();
void ApplyBootToCharacterSelectPatches();
void ApplyCharacterSelectModePatches();
uint32_t __cdecl Hook_GameRand();
void DisableInputRepeatDelays();
void DisableCursorHiding();

#endif // GAME_PATCHES_H
