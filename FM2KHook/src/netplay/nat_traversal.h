#pragma once

// FM2K NAT traversal — STUN probe + UDP hole-punch driver.
//
// Wire protocol (matches docs/FM2K_Matchmaking_Design.md §15.4):
//
//   0xCD 0x01 [24-byte user_id] [padded NULs]
//        STUN probe — client -> hub. Hub replies and stores reflexive
//        (ip, port) on the user. Used to learn our public mapping.
//
//   0xCD 0x02 [4-byte ip_be] [2-byte port_be]
//        STUN ack — hub -> client. Confirms reflexive address.
//
//   0xCD 0x10 [16-byte match_token]
//        CTRL_PUNCH — peer-to-peer punch packet. Burst-sent on
//        match_start; first authentic inbound from peer latches the
//        connectivity (existing peer-learning slot in
//        control_channel.cpp picks up from there).
//
// All three share the same UDP socket as the existing 0xCC control
// channel and 0xCE spectator path. The first-byte demux is in
// control_channel.cpp::RawReceive. NatTraversal_HandleDatagram is
// the entry point for any 0xCD packet.

#include <cstdint>
#include <cstddef>

#include <winsock2.h>

namespace fm2k::nat {

// Send a single STUN probe to the configured hub UDP address. Carries
// our user_id (from FM2K_HUB_USER_ID env). Returns false if no hub addr
// configured (we're not on a hub session). Safe to call repeatedly;
// the hub binds idempotently.
bool SendStunProbe();

// Burst-punch toward (peer_ip, peer_port). Sends ~30 packets over
// ~300 ms with priority boost. Idempotent — if Punch_Tick has
// already latched the peer for this match_token, returns immediately.
//
// lan_ip_be/lan_port (optional, 0 = none): the peer's PRIVATE same-LAN
// candidate from the hub's local_ip exchange. When non-zero the burst also
// punches there so same-router pairs connect directly over the LAN instead of
// hairpinning their public IP. Safe off-LAN (never authenticates if wrong).
void StartPunch(uint32_t peer_ip_be, uint16_t peer_port,
                const uint8_t match_token[16],
                uint32_t lan_ip_be = 0, uint16_t lan_port = 0);

// Called once per call to control_channel.cpp::RawReceive when the
// first byte of an inbound packet is 0xCD. `data`/`len` is the full
// datagram. `from` is the source sockaddr — used as the candidate
// peer address when the packet authenticates via match_token.
void HandleDatagram(const uint8_t* data, size_t len, const sockaddr_in& from);

// Hex-encode the 16-byte match_token (32 chars) into `out` if it has
// been latched (StartPunch was called). Returns true on success, false
// if the token isn't set yet — in which case `out` is left as an empty
// string. `out_size` must be >= 33 (32 hex chars + NUL).
//
// Same token on both peers post-NAT-punch; usable as a cross-peer
// match identifier for tying together both sides' desync uploads.
bool GetMatchTokenHex(char* out, size_t out_size);

// =============================================================================
// Relay fallback
// =============================================================================
// Read FM2K_HUB_RELAY_ADDR + FM2K_HUB_RELAY_SESSION env vars; configures
// the relay envelope used when direct punch fails. Idempotent — safe to
// call from Netplay_Init regardless of whether relay info is present.
// Returns true if both env vars are set and parsed cleanly.
bool ConfigureRelay();

// True once we've decided to fall back to relaying — set after
// StartPunch's burst completes without a peer-latch, OR explicitly
// via ForceRelayMode() (test hook). Once true, RawSend wraps every
// gameplay packet in a 0xCF envelope and sends to the relay; RawReceive
// unwraps inbound 0xCF packets and re-dispatches the inner payload.
bool IsRelayMode();
void ForceRelayMode();   // test/diagnostic only

// Borrowed pointers — caller must not free or modify. Valid for the
// lifetime of the process once ConfigureRelay() returned true.
const sockaddr_in* GetRelayAddr();
const uint8_t*     GetRelaySessionId();   // 16 bytes

// Wrap raw outbound payload bytes in the 0xCF envelope. `out` must
// have room for `len + 18` bytes. Returns the total wrapped length.
size_t WrapForRelay(const uint8_t* in, size_t len, uint8_t* out, size_t out_cap);

// Strip the 0xCF envelope from an inbound packet. Returns true if the
// packet was a valid relay envelope for our session and writes the
// inner payload pointer/length via `out_inner`/`out_inner_len`. The
// returned pointer aliases into `data`, no copy.
bool UnwrapFromRelay(const uint8_t* data, size_t len,
                     const uint8_t** out_inner, size_t* out_inner_len);

}  // namespace fm2k::nat
