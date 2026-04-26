// SpectatorNode — daisy-chain replay streaming (see docs/FM2K_Spectator_Design.md).
//
// Every node (host, gameplay peer, accepted spectator) can run a SpectatorNode
// that accepts downstream subscribers, holds the input-history window needed to
// serve them, and forwards INITIAL_MATCH + INPUT_BATCH + MATCH_END datagrams.
// Over its direct-subscriber capacity it redirects new joiners to an existing
// subscriber, à la CCCaster's `getRandomRedirectAddress`.
//
// This is a SEPARATE transport from GekkoNet. It does not participate in
// rollback or input prediction. Viewers reconstruct the match by driving a
// local replay engine from the streamed inputs, not by running a GekkoNet
// SpectateSession.
#pragma once

#include <cstdint>
#include <cstddef>
#include <winsock2.h>

// Spectator-stream datagram magic — distinct from 0xCC control and GekkoNet
// traffic on the multiplexed UDP socket.
constexpr uint8_t SPEC_DATA_MAGIC = 0xCE;

// Variable-length spectator datagram types (live on the 0xCE path).
//
// Design note: inputs are inputs. We don't have a separate CSS-state-mirror
// packet. The host streams confirmed (p1, p2) input pairs from the moment
// CSS starts (lockstep-confirmed) through battle (GekkoNet-confirmed) and
// back to CSS for the next match. The spectator's local FM2K, also booted
// to CSS, consumes the same inputs in lockstep, walks the same CSS path,
// locks the same chars, transitions to battle when game_mode flips, etc.
// INITIAL_MATCH carries metadata + a seed sanity-rewrite at each new match;
// MATCH_END is informational only.
enum class SpecDataType : uint8_t {
    INITIAL_MATCH = 1,  // Payload = 96-byte ReplayHeader. Per-match metadata
                        // (RNG seed, char selects, state-hash). Seed write is
                        // idempotent — subscription already wrote the same
                        // fixed seed via JOIN_ACK.
    INPUT_BATCH   = 2,  // Payload = ReplayFrame[frame_count], 4 B each.
                        // Used for ALL phases (CSS + battle).
    MATCH_END     = 3,  // No payload — informational, queue keeps draining.
};

#pragma pack(push, 1)
struct SpecDataHeader {
    uint8_t      magic;         // SPEC_DATA_MAGIC
    SpecDataType type;
    uint32_t     start_frame;   // First frame in payload (for INPUT_BATCH)
    uint16_t     frame_count;   // Number of ReplayFrames in payload (INPUT_BATCH only)
    uint16_t     flags;         // Reserved
};
static_assert(sizeof(SpecDataHeader) == 10, "SpecDataHeader must be 10 bytes");
#pragma pack(pop)

// =============================================================================
// LIFECYCLE
// =============================================================================

// Start accepting spectator subscribers on this node. Called once at hook init.
// Pulls the shared UDP socket from control_channel (no separate socket).
void SpectatorNode_Init();

// Shutdown — disconnects all subscribers, clears state.
void SpectatorNode_Shutdown();

// =============================================================================
// HOST-SIDE (match is running on this node)
// =============================================================================

// Called at battle start (alongside Replay_BeginRecording). Captures the
// initial-match metadata that gets handed to every new subscriber.
void SpectatorNode_OnMatchStart(
    uint32_t game_hash,
    uint32_t initial_rng_seed,
    uint32_t initial_state_hash,
    uint8_t p1_char, uint8_t p1_color,
    uint8_t p2_char, uint8_t p2_color);

// Called once per confirmed (non-rollback) frame. The node buffers the input
// pair and, every broadcast_interval frames, batches and forwards to all
// subscribers.
void SpectatorNode_OnFrameConfirmed(uint16_t p1_input, uint16_t p2_input);

// Called at battle end — broadcasts MATCH_END to subscribers.
void SpectatorNode_OnMatchEnd();

// =============================================================================
// JOIN / REDIRECT (runs on any accepting node)
// =============================================================================

// Capacity: the default direct-subscriber cap. Star topology — host serves
// everyone directly up to this many. Beyond capacity, new joiners get
// redirected to an existing subscriber (1-hop relay), à la CCCaster's
// random-redirect. We rarely expect to hit this in practice: at ~500 B/s
// per spectator, even 100 direct subscribers is only 50 KB/s upload.
// Daisy-chain is the failure case, not the design.
constexpr size_t SPECTATOR_DEFAULT_CAPACITY = 32;

// Failover timing.
constexpr uint64_t SPECTATOR_HEARTBEAT_INTERVAL_MS = 1000;   // 1 s outbound
constexpr uint64_t SPECTATOR_SILENCE_FAILOVER_MS   = 5000;   // 5 s with no
                                                             // inbound from
                                                             // current
                                                             // upstream → fall
                                                             // back to root
constexpr uint64_t SPECTATOR_RECONNECT_BACKOFF_MS  = 2000;   // throttle JOIN_REQ
constexpr uint64_t SPECTATOR_SUBSCRIBER_EXPIRY_MS  = 30000;  // upstream-side
                                                             // sweep: drop
                                                             // subscribers
                                                             // silent >30 s

// Set the direct-subscriber cap. 0 disables accepting (node still relays if
// it already has subscribers).
void SpectatorNode_SetCapacity(size_t max_direct);

// Handle an inbound SPEC_JOIN_REQ from a peer. Either:
//   - accept (below capacity) → enqueue INITIAL_MATCH + SPEC_JOIN_ACK
//   - redirect (at capacity, have subscribers) → send SPEC_JOIN_REDIRECT
//   - reject (at capacity, no subscribers) → send SPEC_JOIN_REDIRECT w/ null
// Called from the control-channel message handler.
void SpectatorNode_HandleJoinReq(const sockaddr_in& from);

// Handle SPEC_LEAVE — remove subscriber from list.
void SpectatorNode_HandleLeave(const sockaddr_in& from);

// Handle SPEC_HEARTBEAT — refresh last-seen timestamp for this subscriber.
void SpectatorNode_HandleHeartbeat(const sockaddr_in& from);

// =============================================================================
// STATUS / INTROSPECTION
// =============================================================================

// How many direct subscribers this node is serving.
size_t SpectatorNode_GetSubscriberCount();

// Is this node currently pushing a live match to subscribers?
bool SpectatorNode_IsBroadcasting();

// =============================================================================
// VIEWER-SIDE (this node is a spectator subscribing upstream)
// =============================================================================

// Send a SPEC_JOIN_REQ upstream. Called when the user clicks "Spectate" on
// a match in the lobby (or enters a direct IP). upstream is the host/relay's
// address; socket is the multiplexed UDP socket we already have.
bool SpectatorNode_RequestJoin(const sockaddr_in& upstream);

// Set the always-on failback root address. TickHealth will reconnect to
// root if our current upstream goes silent. Called once at spectator init
// from Netplay_InitAsSpectator.
void SpectatorNode_SetRootAddr(const sockaddr_in& root);

// Handle inbound SPEC_JOIN_ACK — upstream accepted us; further 0xCE
// datagrams on the same socket will populate our replay queue.
void SpectatorNode_HandleJoinAck(const sockaddr_in& from);

// Handle inbound SPEC_JOIN_REDIRECT — retry against redirect target.
void SpectatorNode_HandleJoinRedirect(const sockaddr_in& from,
                                      uint32_t redirect_ip,
                                      uint16_t redirect_port);

// Handle an inbound 0xCE datagram. Parses the SpecDataHeader and routes into
// the replay-playback input queue (Replay_LoadFromBuffer for INITIAL_MATCH;
// append for INPUT_BATCH; match-end for MATCH_END). Called by the UDP poll
// path in control_channel.cpp when it sees SPEC_DATA_MAGIC.
void SpectatorNode_HandleSpecData(const uint8_t* buf, size_t len,
                                  const sockaddr_in& from);

// Are we currently subscribed upstream (receiving a live match stream)?
bool SpectatorNode_IsSubscribedUpstream();

// Periodic health tick. Called from the trampoline once per iteration
// (cheap — internally rate-limited). Drives:
//   * heartbeat send to upstream every HEARTBEAT_INTERVAL_MS
//   * silence detection → failover to root after SILENCE_FAILOVER_MS
//   * upstream-side subscriber sweep (expire silent subscribers)
void SpectatorNode_TickHealth();

// Clean disconnect from upstream.
void SpectatorNode_LeaveUpstream();

// =============================================================================
// VIEWER-SIDE PLAYBACK DRIVER (consumed by main_loop_trampoline)
// =============================================================================
//
// When the spectator is subscribed and the host has sent INITIAL_MATCH,
// `IsPlayingBack()` flips true and inputs flow:
//   trampoline RunSpectatorTick() →
//     pulls (p1, p2) via PopFrameInputs() →
//     stores into shared globals →
//     drives original_process_game_inputs / original_update_game →
//     RenderFrameWithSnapshot
// MATCH_END flips IsPlayingBack() back to false; queue drains naturally.

// Are we actively playing back a host's match (inside SPECTATOR_PLAYBACK phase)?
// True from INITIAL_MATCH receipt until MATCH_END.
bool SpectatorNode_IsPlayingBack();

// Pull the next confirmed frame's (p1, p2) inputs from the local queue.
// Returns false if the queue is empty (we're paused, waiting for the next
// upstream INPUT_BATCH); the trampoline holds the current rendered frame
// rather than advancing the sim. Returns true and fills *p1_input/*p2_input
// when a frame is available.
bool SpectatorNode_PopFrameInputs(uint16_t* p1_input, uint16_t* p2_input);

// Cached input pair from the most recent successful PopFrameInputs. The hook
// at Hook_GetPlayerInput uses these to satisfy multiple GetPlayerInput calls
// per sim tick from a single queue pop. Mirrors the netplay-active path
// where Netplay_GetInput returns a stable per-frame value.
uint16_t SpectatorNode_GetCurrentP1Input();
uint16_t SpectatorNode_GetCurrentP2Input();

// Force the cached inputs to zero. Used by RunSpectatorTick during
// post-match idle (queue empty, playing_back=false) so the local sim
// auto-advances through results / intermission with neutral inputs
// rather than re-using last frame's streamed input.
void SpectatorNode_ResetCurrentInputs();

// Number of buffered (p1, p2) frames waiting to be consumed. Used by the
// trampoline to decide if we're paused (0) or have headroom.
size_t SpectatorNode_PendingFrameCount();
