// Control Channel - Multiplexed UDP Socket for Netplay
// Shares single socket between control messages and GekkoNet
#pragma once

#include "netplay_state.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <cstdint>

// Forward declaration for GekkoNet
struct GekkoNetAdapter;

// =============================================================================
// SOCKET MANAGEMENT (shared between control channel and GekkoNet)
// =============================================================================

// Initialize the shared UDP socket
// Called at DLL load, stays open for entire session
bool NetSocket_Init(uint16_t local_port, const char* remote_addr);

// Shutdown the shared socket
// Called at DLL unload
void NetSocket_Shutdown();

// Check if socket is initialized
bool NetSocket_IsInitialized();

// Get the raw socket handle (for debugging)
SOCKET NetSocket_GetHandle();

// Get remote address info (for GekkoNet adapter)
const sockaddr_in* NetSocket_GetRemoteAddr();

// =============================================================================
// CONTROL CHANNEL (0xCC-prefixed packets)
// =============================================================================

// Poll for incoming control packets and process them
// Should be called every frame when connected
// Updates CSS state, handles connection keepalive
void ControlChannel_Poll();

// Send a control packet to remote peer
void ControlChannel_Send(const CtrlPacket& packet);

// Check if control channel is connected (received HELLO_ACK)
bool ControlChannel_IsConnected();

// Get time since last received packet (for timeout detection)
uint32_t ControlChannel_GetLastRecvMs();

// Get round-trip time estimate (ping)
uint32_t ControlChannel_GetRttMs();

// =============================================================================
// CONTROL CHANNEL - CONVENIENCE FUNCTIONS
// =============================================================================

// Send HELLO to initiate connection
void ControlChannel_SendHello(uint8_t player_id, uint32_t game_hash);

// Send HELLO_ACK to accept connection
void ControlChannel_SendHelloAck(uint8_t player_id);

// Send PING for keepalive
void ControlChannel_SendPing();

// Send CSS raw input with frame number for lockstep
void ControlChannel_SendCSSInput(uint16_t input, uint32_t frame);

// Send CSS cursor position update [deprecated - use CSS input instead]
void ControlChannel_SendCursor(uint8_t x, uint8_t y);

// Send CSS character selection (highlighted, not locked)
void ControlChannel_SendCharSelect(uint8_t slot, uint8_t color);

// Send CSS character lock (confirmed selection)
void ControlChannel_SendCharLock(uint8_t slot, uint8_t color);

// Send CSS character unlock (cancelled selection)
void ControlChannel_SendCharUnlock();

// Send CSS start signal (both players synced, start counting frames NOW)
void ControlChannel_SendCSSStart();

// Send battle ready signal (for CSS sync). Carries the sender's proposed
// local input delay so both peers can agree on max() before starting the
// GekkoNet session — otherwise mismatched RTT samples produce asymmetric
// per-peer local delay and one side feels more input lag than the other.
void ControlChannel_SendBattleReady(uint8_t proposed_local_delay);

// Send battle acknowledgment
void ControlChannel_SendBattleAck();

// Send battle entering signal (game_mode changed to battle)
void ControlChannel_SendBattleEntering();

// Send battle start (both sides ready)
void ControlChannel_SendBattleStart(uint32_t start_frame);

// Send battle end
void ControlChannel_SendBattleEnd();

// Send clean disconnect
void ControlChannel_SendDisconnect();

// =============================================================================
// GEKKONET CUSTOM ADAPTER
// Creates an adapter that shares our socket and filters control packets
// =============================================================================

// Create custom GekkoNet adapter that uses our shared socket
// Control packets (0xCC prefix) are filtered out and queued for ControlChannel_Poll
// GekkoNet packets are passed through normally
GekkoNetAdapter* CreateMultiplexAdapter();

// Destroy the custom adapter (just nulls the static, doesn't free socket)
void DestroyMultiplexAdapter();

// =============================================================================
// CALLBACKS - Set by netplay.cpp to handle received control messages
// =============================================================================

// Callback function type for control messages
typedef void (*ControlMsgCallback)(const CtrlPacket* packet);

// Set callback for when control messages are received
void ControlChannel_SetCallback(ControlMsgCallback callback);

// =============================================================================
// INTERNAL HELPERS (called by netplay.cpp message handler)
// =============================================================================

// Mark connection as established (called when HELLO_ACK received)
void ControlChannel_SetConnected(bool connected);

// Handle incoming PONG to calculate RTT
void ControlChannel_HandlePong(uint32_t sent_time);
