// per_game_patches_fm95.cpp -- FM95 no-op stubs for the PerGamePatches_* API.
// Split (engine axis) from per_game_patches.cpp; the FM95 patch home as FM95 matures.

#if defined(ENGINE_FM95)

#include "per_game_patches.h"
#include <cstdint>

       // engine, different addresses, different round-state plumbing).

#include "per_game_patches.h"

bool PerGamePatches_InstallDamageMultiplierHook()              { return true; }
bool PerGamePatches_InstallKofHpInitPatch()                    { return true; }
void PerGamePatches_ApplyRuntime()                             {}
int  PerGamePatches_TryOverrideInput(int, uint32_t)            { return -1; }
uint16_t PerGamePatches_GatedP2CssInput(uint16_t)              { return 0; }
int      PerGamePatches_BattleInputOverride(int, uint16_t)     { return -1; }
void PerGamePatches_CycleTrainingP2Behavior()                  {}
int  PerGamePatches_GetTrainingP2Behavior()                    { return 0; }
const char* PerGamePatches_TrainingP2BehaviorLabel(int)        { return "?"; }
void PerGamePatches_OnOptionPressed()                          {}
int  PerGamePatches_GetVsSubmode()                             { return 0; }
int  PerGamePatches_GetVsMenuContext()                         { return 0; }
const char* PerGamePatches_VsSubmodeLabel(int, int)            { return "?"; }
bool PerGamePatches_IsOptionModeSelectorActive()               { return false; }
bool PerGamePatches_IsTrainingModeActive()                     { return false; }
bool PerGamePatches_IsVsCpuModeActive()                        { return false; }
bool PerGamePatches_IsCpuVsCpuModeActive()                     { return false; }
int  PerGamePatches_GetTeamSizeOverride()                      { return 0; }
void PerGamePatches_OnFrameTick()                              {}
void PerGamePatches_OnTitleInputTick(uint16_t, uint32_t)       {}
void PerGamePatches_OnGameStateManagerEntry()                  {}
bool PerGamePatches_InstallBootToBattleHook()                  { return true; }
bool PerGamePatches_InstallStoryInitHijack()                   { return true; }
void PerGamePatches_OnBattleInitComplete()                     {}
void PerGamePatches_SetRuntimeBtbOverrides(uint8_t, uint8_t, uint8_t, uint8_t, uint8_t) {}
void PerGamePatches_AbortBtbNaturalBoot()                      {}


#endif  // ENGINE_FM95

