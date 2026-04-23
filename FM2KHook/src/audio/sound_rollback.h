#pragma once
#include <cstdint>

// ============================================================================
// Rollback-safe sound layer (Mike Z "desired vs actual" design).
//
// FM2K's sound API is single-chokepoint — every SFX play goes through
// DispatchScriptSoundCommand @ 0x403430 → PlaySoundFromBufferArray @ 0x415DF0.
// The per-channel round-robin cursor at offset +12 of each SoundBufferArray
// is sim-mutable, which is why naive rollback replay clips sounds (replay
// re-advances the cursor and force-stops still-playing buffers).
//
// This layer interposes between the sim and DSound: during battle, the hook
// only updates `desired[channel]`. Once per displayed frame (after the
// advance batch completes, before render) we reconcile desired ↔ actual and
// trigger real DSound stops/plays, gated by Mike Z's rollback-window rules:
//
//   - If desired was written OUTSIDE the rollback window, rollback didn't
//     change it.  But if the currently-playing actual was set INSIDE the
//     window, rollback erased that play — stop it.
//   - If desired was written INSIDE the window, play it for real now.
//
// Channel identity = slot index in g_sound_channel_table @ 0x430640..0x433240
// (2816 DWORD-sized pointer slots). Populated once at battle start by scanning
// for non-null entries; each SoundBufferArray pointer maps to a stable slot id.
// ============================================================================

namespace SoundRollback {

// Per-channel desired state. Saved in the rollback ring.
struct DesiredState {
    uint32_t wave_ptr;       // identity: SoundBufferArray.wave_data pointer
    uint32_t play_frame;     // frame this was triggered on
    uint16_t seq_in_frame;   // tiebreaker when the same chan plays twice in one frame
    uint8_t  stopped;        // 1 = "silence was played on play_frame" (post-rollback-erase)
    uint8_t  _pad;
};

constexpr int MAX_CHANNELS = 2816;  // = (0x433240 - 0x430640) / 4

// Lifecycle — called from Netplay_StartBattle / Netplay_EndBattle.
void Init();
void OnBattleEnd();

// Called from Hook_DispatchScriptSoundCommand on the SFX branch.
// `arr` is the SoundBufferArray pointer (script_item + 36).
void RecordDesired(void* arr, uint32_t current_frame);

// Called once per displayed frame from Netplay_ProcessBattleInputPhase,
// AFTER the advance batch completes and BEFORE render. Walks desired[] vs
// actual[], applies Mike Z's window check, stops/starts DSound buffers via
// the original PlaySoundFromBufferArray / StopAllSoundsInBufferArray.
void SyncAfterAdvance(uint32_t earliest_frame, uint32_t current_frame);

// Savestate integration — called from SaveState_Save / SaveState_Load.
void CaptureDesired(DesiredState* out);
void RestoreDesired(const DesiredState* in);

} // namespace SoundRollback
