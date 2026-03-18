// Netplay - State Machine + GekkoNet Integration
// Two-layer networking:
//   1. Control Channel (always active when connected) - CSS sync, transitions
//   2. GekkoNet Session (battle only) - full rollback netcode
#pragma once

#include "netplay_state.h"
#include <cstdint>

// =============================================================================
// LIFECYCLE
// =============================================================================

// Initialize the netplay system (control channel + state machine)
// player_index: 0 = P1 (host), 1 = P2 (client)
// local_port: UDP port to listen on
// remote_addr: "ip:port" of remote player
// Returns true on success
bool Netplay_Init(int player_index, uint16_t local_port, const char* remote_addr);

// Shutdown everything (control channel + GekkoNet if active)
void Netplay_Shutdown();

// =============================================================================
// STATE MACHINE
// =============================================================================

// Get current netplay state
NetplayState Netplay_GetState();

// Set netplay state (for internal use, triggers logging)
void Netplay_SetState(NetplayState state);

// Get CSS state (cursor positions, character locks, etc.)
const CSSState& Netplay_GetCSSState();

// Reset CSS state (call when returning from battle for rematch)
void Netplay_ResetCSSState();

// CSS sync functions - for coordinating CSS entry
bool Netplay_IsRemoteCSSReady();
void Netplay_SetLocalCSSReady(bool ready);
bool Netplay_IsCSSFullySynced();

// =============================================================================
// PER-FRAME PROCESSING (called from hooks)
// =============================================================================

// Process CSS mode frame (lockstep sync)
// Call when game_mode == 2000
// Returns true if game should advance, false if waiting for sync
bool Netplay_ProcessCSS();

// Check if CSS can advance (have remote input for current frame)
// Called by BOTH ProcessGameInputs and UpdateGameState hooks to freeze game during stalls
bool Netplay_CanAdvanceCSS();

// Poll CSS network (call during stalls to receive pending data)
void Netplay_PollCSS();

// Process battle input phase - called from Hook_ProcessGameInputs
// Polls GekkoNet, handles Save/Load events, sets synced inputs
// For rollback with multiple AdvanceEvents, runs complete frames for all but the last
// Returns true if ready to advance (AdvanceEvent received), false if waiting
bool Netplay_ProcessBattleInputPhase();

// Process menu/other mode frame (just keepalive)
// Call for other game modes
void Netplay_ProcessMenu();

// =============================================================================
// BATTLE MODE SYNC BARRIER
// =============================================================================

// Signal that we're entering battle mode (game_mode changed to 3000+)
// Sends BATTLE_ENTERING to remote, returns immediately
void Netplay_SignalBattleEntry();

// Check if both clients have entered battle mode
// Returns true when both have signaled battle entry
bool Netplay_IsBattleSynced();

// Poll for battle sync messages (call while waiting for sync)
void Netplay_PollBattleSync();

// =============================================================================
// GEKKONET SESSION LIFECYCLE
// =============================================================================

// Start GekkoNet session when entering battle mode
// Call when game_mode transitions from CSS (2000) to Battle (3000+)
// Returns true if session started successfully
bool Netplay_StartBattle();

// End GekkoNet session when leaving battle mode
// Keeps control channel connected for rematch
void Netplay_EndBattle();

// Legacy API (delegates to StartBattle/EndBattle)
bool Netplay_StartGekkoSession();
void Netplay_StopGekkoSession();

// Check if GekkoNet session is ready (synced with remote)
bool Netplay_IsSessionReady();

// Poll GekkoNet for events without advancing game
void Netplay_PollGekkoNet();

// =============================================================================
// INPUT
// =============================================================================

// Get synchronized input for a player (0 = P1, 1 = P2)
// Only valid during BATTLE_RUNNING state
uint16_t Netplay_GetInput(int player_id);

// Get CSS input for a player (0 = P1, 1 = P2)
// Returns remote player's input via control channel, 0 for local
uint16_t Netplay_GetCSSInput(int player_id);

// =============================================================================
// STATUS QUERIES
// =============================================================================

// Check if GekkoNet session is active
bool Netplay_IsActive();

// Check if control channel is connected
bool Netplay_IsConnected();

// Get current frame counter
uint32_t Netplay_GetFrame();

// Get round-trip time in milliseconds
uint32_t Netplay_GetPingMs();
