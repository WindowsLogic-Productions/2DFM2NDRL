#pragma once

// FM2K NAT traversal — STUN probe + UDP hole-punch driver.
//
// Wire protocol (matches docs/FM2K_Matchmaking_Design.md §15.4):
//
//   0xCD 0x01 [12-byte user_id] [padded NULs]
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
// Phase-1 stub: this currently only logs intent. The actual burst
// driver (port from bbbr_holepunch.cpp) lands in a follow-up.
void StartPunch(uint32_t peer_ip_be, uint16_t peer_port,
                const uint8_t match_token[16]);

// Called once per call to control_channel.cpp::RawReceive when the
// first byte of an inbound packet is 0xCD. `data`/`len` is the full
// datagram. `from` is the source sockaddr — used as the candidate
// peer address when the packet authenticates via match_token.
void HandleDatagram(const uint8_t* data, size_t len, const sockaddr_in& from);

}  // namespace fm2k::nat
