#pragma once

// C3.5 — vs_round_function detour for ROUND_START / ROUND_END emit.
//
// FM2K-only emit; on FM95 builds these are no-ops (FM95 has different
// round-state plumbing — separate hand-off).
//
// Lifecycle:
//   - RoundEvents_Install: called from Hooks::Hook_Initialize after the
//     other MinHook detours are wired up. Adds the trampoline for
//     vs_round_function @ 0x004086A0 (FM2K) so each call edge-detects the
//     substate field and emits the appropriate session event.
//   - RoundEvents_OnMatchStart: called from Netplay_StartBattle so the
//     1-based round-counter resets at every match boundary.

bool RoundEvents_Install();
void RoundEvents_OnMatchStart();
