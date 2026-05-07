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
    INPUT_REQUEST = 4,  // Spectator → host: "send me frames from start_frame
                        // onward." Payload empty; the request is encoded in
                        // SpecDataHeader.start_frame. Host responds with an
                        // INPUT_BATCH starting at that frame (or empty if
                        // not yet recorded). Gives bulletproof delivery —
                        // any persistent gap gets re-requested until filled.
    EVENT_BATCH   = 5,  // Payload = packed SessionEvent[] stream (variable-
                        // length, each event is 1-byte type + variant payload).
                        // Replaces INPUT_BATCH for the C2+ wire format —
                        // carries inputs interleaved with PIN_RNG /
                        // RESET_INPUT_STATE / SOUND_INIT / MATCH_START /
                        // MATCH_END / FINGERPRINT ops in a single ordered
                        // stream. SpecDataHeader.start_frame is the
                        // session-relative index of the first INPUT event
                        // in this batch; frame_count is the number of INPUT
                        // events (non-INPUT events are not counted). flags
                        // bit 0 set = the batch begins with deferred ops
                        // (ops with no following INPUT in this batch).
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
// SESSION EVENT STREAM (C1+: typed event log)
// =============================================================================
//
// One ordered event stream replaces the old "INPUT_BATCH only" wire format.
// Every state mutation the host applies — RNG pin, input-state reset, sound
// dedup re-init, match boundaries, per-frame inputs — is appended as a typed
// event in the same vector. The spectator drains the stream in order: non-
// INPUT events at the head are applied immediately; INPUT events are popped
// one per sim tick.
//
// "Frame number" is implicit: it's the index of the next INPUT event in the
// stream. Non-INPUT events execute *before* the input that follows them.
// This collapses ordering — there's no separate "ops timeline" to schedule
// against the input timeline, which is exactly the race that caused the
// game_mode-driven CheckGameModeTransition spectator branch to desync.
//
// Wire encoding per event (1-byte type tag + variant payload):
//
//   Type | Tag | Wire payload          | Bytes (incl. tag)
//   ---------------------------------------------------------
//   INPUT             | 1 | u16 p1, u16 p2     | 5
//   PIN_RNG           | 2 | u32 seed           | 5
//   RESET_INPUT_STATE | 3 | (none)             | 1
//   SOUND_INIT        | 4 | (none)             | 1
//   MATCH_START       | 5 | u8[96] ReplayHeader| 97
//   MATCH_END         | 6 | (none)             | 1
//   FINGERPRINT       | 7 | u32 fletcher32     | 5
//
// Endianness: little-endian throughout (matches FM2K's native layout).
enum class SessionEventType : uint8_t {
    INPUT             = 1,
    PIN_RNG           = 2,
    RESET_INPUT_STATE = 3,
    SOUND_INIT        = 4,
    MATCH_START       = 5,
    MATCH_END         = 6,
    FINGERPRINT       = 7,
};

constexpr size_t SESSION_EVENT_MATCH_HDR_SIZE = 96;
constexpr size_t SESSION_EVENT_MAX_WIRE_SIZE  = 1 + SESSION_EVENT_MATCH_HDR_SIZE;  // 97 (MATCH_START)

// Compact in-memory event. MATCH_START's 96-byte header is stored out-of-
// band in a parallel vector (see g_state.match_headers in spectator_node.cpp)
// keyed by match_start_idx. Keeps SessionEvent fixed at 5 bytes so a
// vector<SessionEvent> for a 1-hour 100 Hz set stays at ~1.7 MB instead of
// ~35 MB if MATCH_START's 96-byte payload were inline.
#pragma pack(push, 1)
struct SessionEvent {
    SessionEventType type;          // 1 byte
    union {                         // 4 bytes — same size across all variants
        struct { uint16_t p1; uint16_t p2; } input;
        uint32_t                             pin_rng_seed;
        uint32_t                             fingerprint_hash;
        uint16_t                             match_start_idx;  // index into match_headers side table
        uint8_t                              raw[4];
    } u;
};
#pragma pack(pop)
static_assert(sizeof(SessionEvent) == 5, "SessionEvent must be 5 bytes packed");

// ---- Wire-format encoders ---------------------------------------------------
// Each Encode* writes one event into `out`. Returns bytes written, or 0 if
// `cap` is insufficient. No partial writes — the buffer is left untouched on
// overflow.

size_t SessionEvent_EncodeInput            (uint8_t* out, size_t cap, uint16_t p1, uint16_t p2);
size_t SessionEvent_EncodePinRng           (uint8_t* out, size_t cap, uint32_t seed);
size_t SessionEvent_EncodeResetInputState  (uint8_t* out, size_t cap);
size_t SessionEvent_EncodeSoundInit        (uint8_t* out, size_t cap);
size_t SessionEvent_EncodeMatchStart       (uint8_t* out, size_t cap,
                                            const uint8_t header[SESSION_EVENT_MATCH_HDR_SIZE]);
size_t SessionEvent_EncodeMatchEnd         (uint8_t* out, size_t cap);
size_t SessionEvent_EncodeFingerprint      (uint8_t* out, size_t cap, uint32_t hash);

// ---- Wire-format decoder ----------------------------------------------------
// Decode the next event from a byte buffer. Returns bytes consumed (1, 5, or
// 97), or 0 if the buffer is truncated or the type byte isn't a valid
// SessionEventType.
//
// `out_event->u.match_start_idx` is left at 0 on MATCH_START — caller
// (HandleSpecData) populates it after appending the parsed header to the
// match_headers side table. The 96-byte header is copied into
// `out_match_header` if non-null; pass nullptr to skip the copy.
size_t SessionEvent_Decode(const uint8_t* in, size_t in_len,
                           SessionEvent* out_event,
                           uint8_t out_match_header[SESSION_EVENT_MATCH_HDR_SIZE]);

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

// Append a state-init op to the session event log + flush immediately so
// any currently-subscribed spectator receives it before the INPUT events
// that follow. The spectator drains non-INPUT events at the head of its
// playback queue and dispatches on type, applying the same memory writes
// the host just did. Late joiners see ops as part of their session backfill.
//
// Call sites (host only — no-op on spectator nodes):
//   PIN_RNG           — every site that writes 0x12345678 to ADDR_RANDOM_SEED
//                       (handshake, CSS rendezvous, battle start)
//   RESET_INPUT_STATE — paired with battle-start SaveState_Save's first-call
//                       buf_idx + edge-state + history-rings reset
//   SOUND_INIT        — every SoundRollback::Init() call site
//   FINGERPRINT       — diagnostic only; gated on FM2K_SPEC_FINGERPRINT
void SpectatorNode_AppendPinRng(uint32_t seed);
void SpectatorNode_AppendResetInputState();
void SpectatorNode_AppendSoundInit();
void SpectatorNode_AppendFingerprint(uint32_t hash);

// Append a MATCH_START op carrying the 96-byte ReplayHeader-compatible
// payload (magic + version + game_hash + initial_rng_seed +
// initial_state_hash + p1/p2 char/color/name + stage_id). The receiver
// caches the header in its pb_match_headers side table and flips
// playing_back=true at apply time. C6 supersedes the legacy
// INITIAL_MATCH packet path; the wire packet type stays in the enum
// for compatibility but is no longer emitted by C6+ hosts.
void SpectatorNode_AppendMatchStart(const uint8_t header[96]);

// Append a MATCH_END op. Apply-time effect: playing_back=false (queue
// keeps draining naturally so the final frames render).
void SpectatorNode_AppendMatchEnd();

// =============================================================================
// SESSION REPLAY FILE WRITERS (C7)
// =============================================================================
//
// Both write the same on-disk format — packed SessionEvent bytes (1-byte
// tag + variant payload, MATCH_START's 96-byte header inline) preceded by
// a 32-byte file header. Distinguished by the `is_battle_slice` flag in
// the header. Loaders (Replay_LoadSessionFile, C8) feed events into the
// playback driver pb_queue same as live wire ingest.
//
// Path format:
//   replays/<timestamp>.fm2krep   — per-battle slice
//   sessions/<timestamp>.fm2kset  — full session
// (Relative to the game's working directory; matches existing `replays/`
//  layout used by Replay_BeginRecording.)
//
// Returns true on successful write. False if there's no usable data
// (empty session, MATCH_START not seen yet, etc.) or the file open failed.

// Write everything in session_events to a .fm2kset.
bool SpectatorNode_WriteSessionFile(const char* path);

// Write the slice [last_match_start_idx ... session_events.size()) — i.e.
// the per-battle segment closing on the most-recent MATCH_END. Call
// AFTER OnMatchEnd has appended its MATCH_END so the slice includes it.
bool SpectatorNode_WriteCurrentBattleFile(const char* path);

// =============================================================================
// SESSION FILE LOADER (C8)
// =============================================================================
//
// Read a .fm2kset / .fm2krep file written by SpectatorNode_WriteSessionFile
// or WriteCurrentBattleFile and push every event into pb_queue. Same
// playback driver as the live wire path (HandleSpecData::EVENT_BATCH) —
// the trampoline's RunSpectatorTick drains them identically.
//
// Resets receiver-side state on entry: pb_queue cleared, pb_match_headers
// cleared, dedup baseline cleared, playing_back flipped on. Caller is
// responsible for ensuring g_spectator_mode is true before calling — the
// trampoline only routes through RunSpectatorTick when it is.
//
// Returns true on successful parse + queue. False on file open failure,
// magic/version mismatch, or truncated body.
bool SpectatorNode_LoadSessionFile(const char* path);

// =============================================================================
// FINGERPRINT DIAGNOSTIC (C9)
// =============================================================================

// True if FM2K_SPEC_FINGERPRINT=1 was set at process start. Gates host
// emit of FINGERPRINT ops + spectator-side mismatch logging.
bool SpectatorFingerprint_Enabled();

// Compute Fletcher-32 over the current FM2K state sample (RNG, buf_idx,
// HPs, timer, positions, scripts). Same shape on host and spectator;
// drain-at-head ordering on the spectator means both sides hash at the
// same logical frame. Mismatch indicates a desync.
uint32_t SpectatorFingerprint_Compute();

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

// Snapshot of subscriber addresses, used by Netplay_Start{CSS,Battle}Session
// to call gekko_add_actor(GekkoSpectator, &addr) for each. Returned by value
// (copy) to avoid exposing the internal Subscriber struct.
#include <vector>
std::vector<sockaddr_in> SpectatorNode_GetSubscriberAddrs();

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

// Handle inbound SPEC_JOIN_ACK — upstream accepted us. host_session_kind
// from the ACK payload (1=CSS, 2=BATTLE, 0=unknown/between-matches) is
// used to create a matching GekkoSpectateSession.
void SpectatorNode_HandleJoinAck(const sockaddr_in& from, uint8_t host_session_kind,
                                 uint16_t host_tcp_port);

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

// Host-side maintenance only — bind newly-accepted TCP clients to subscriber
// slots, ship deferred INITIAL_MATCH/backfill on first bind, and expire silent
// subscribers. Driven from ControlChannel_Poll so it runs on the host path
// (which doesn't go through SpectatorNode_TickHealth) every poll iteration.
// Idempotent and short-circuiting when there are no subscribers.
void SpectatorNode_TickHostMaintenance();

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
