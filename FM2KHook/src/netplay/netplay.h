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

// =============================================================================
// PER-FRAME PROCESSING (called from hooks)
// =============================================================================

// Process CSS mode frame (lockstep sync)
// Call when game_mode == 2000
// Returns true if game should advance, false if waiting for sync
bool Netplay_ProcessCSS();

// Process battle mode frame (GekkoNet rollback)
// Call when game_mode >= 3000 && < 4000
// Returns true if game should advance, false if waiting for sync
bool Netplay_ProcessBattle();

// Process menu/other mode frame (just keepalive)
// Call for other game modes
void Netplay_ProcessMenu();

// =============================================================================
// GEKKONET SESSION LIFECYCLE
// =============================================================================

// Start GekkoNet session (call when CSS_BOTH_READY)
// Returns true if session started successfully
bool Netplay_StartGekkoSession();

// Stop GekkoNet session (call when leaving battle)
// Keeps control channel connected for rematch
void Netplay_StopGekkoSession();

// Check if GekkoNet session is ready (synced with remote)
bool Netplay_IsSessionReady();

// Poll GekkoNet for events without advancing game (for BATTLE_INIT phase)
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
