#include "spectator_node.h"
#include "spectator_tcp.h"        // TCP transport for INPUT_BATCH stream
#include "control_channel.h"
#include "netplay.h"
#include "replay.h"
#include "savestate.h"            // SaveState_Save / Peek for snapshot capture
#include "netplay_state.h"
#include "../audio/sound_rollback.h"  // Op apply: SOUND_INIT
#include "../hooks/css_autoconfirm.h" // Replay-mode CSS lock-and-confirm
#include "../hooks/per_game_patches.h" // PerGamePatches_SetRuntimeBtbOverrides
#include "../ui/shared_mem.h"         // C10: SharedMem_PublishMatchSession / RoundResult
#include "gekkonet.h"

#include <SDL3/SDL_log.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <array>
#include <set>
#include <string>
#include <vector>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <ctime>

// =============================================================================
// MODULE STATE
// =============================================================================

namespace {

// Broadcast cadence: emit a batch every N newly-confirmed frames.
constexpr size_t BROADCAST_BATCH_FRAMES = 8;

// Redundancy window: each batch carries the last N confirmed frames (not
// just the BROADCAST_BATCH_FRAMES new ones). This means each frame appears
// in floor(REDUNDANCY_WINDOW / BROADCAST_BATCH_FRAMES) consecutive batches,
// so a single dropped UDP packet doesn't lose any inputs — the next batch
// re-delivers them. With WINDOW=32 and BATCH=8, each frame ships in 4
// batches: tolerates up to 3 consecutive packet losses before any data is
// genuinely lost.
//
// Wire cost: ~128 B / batch payload (32 frames × 4 B) + 10 B header + UDP
// overhead ≈ 180 B / packet. At 12.5 batches/sec that's ~2.3 KB/s per
// subscriber — still nothing.
constexpr size_t REDUNDANCY_WINDOW = 32;

// Subscriber entry from the host's perspective.
struct Subscriber {
    sockaddr_in  addr;
    uint64_t     last_seen_ms;
    uint32_t     ack_frame;    // Last frame we know this subscriber has. TODO: fill on SPEC_ACK.
    bool         tcp_bound;    // True once SpectatorTCP::RegisterAcceptedClient(addr)
                               // has paired this sub with an accepted TCP socket.
                               // Until then, INITIAL_MATCH + backfill are deferred —
                               // any send before binding silently drops on the floor.
    SpecJoinMode join_mode;    // Backfill preference declared in SPEC_JOIN_REQ.
                               // Phase 1: stored only; phase 3 branches on it
                               // (CURRENT_MATCH ships snapshot+tail instead of
                               // SendSessionBackfillTo from frame 0).
    // spec_user_id -- hub's identifier for this spectator. Populated in
    // relay mode (Phase 2c) by parsing spec_incoming's spec_user_id
    // field via shared-mem. In TCP mode this stays empty; addressing
    // uses `addr` instead. Phase 2b just declares the field so the
    // Subscriber shape is forward-compatible.
    std::string  spec_user_id;
};

// Cached initial-match metadata so new joiners get a consistent handoff.
struct InitialMatch {
    bool     valid;
    uint8_t  header_bytes[96];  // Serialized ReplayHeader
};

// Side table of MATCH_START headers (96 bytes each) referenced by index from
// SessionEvent::u.match_start_idx. Out-of-band so SessionEvent stays at 5 B
// in memory regardless of match boundary frequency.
using MatchHeader = std::array<uint8_t, SESSION_EVENT_MATCH_HDR_SIZE>;

struct State {
    bool                      broadcasting      = false;
    size_t                    capacity          = SPECTATOR_DEFAULT_CAPACITY;
    std::vector<Subscriber>   subscribers;
    InitialMatch              initial_match     = {};
    // spec_transport_relay -- Phase 2b. True when FM2K_SPEC_TRANSPORT=relay
    // was set at SpectatorNode_Init. In relay mode the TCP listener +
    // PerformTcpStun are skipped entirely; spec data plane will flow
    // through the hub once Phase 2c wires the launcher's binary-frame
    // queue. False = legacy P2P TCP path (every shipped client to date).
    bool                      spec_transport_relay = false;

    // ─── HOST SIDE: session event log ──────────────────────────────────
    // Every confirmed event the host produces (INPUT pairs in C2; PIN_RNG
    // / RESET_INPUT_STATE / SOUND_INIT / MATCH_START / MATCH_END /
    // FINGERPRINT in C3+) is appended monotonically here. Late joiners get
    // the whole vector replayed via SendSessionBackfillTo. Memory: 5 B/event
    // → ~1.7 MB for an hour of 100 Hz INPUT events; non-INPUT events are
    // sparse (a few per match).
    std::vector<SessionEvent> session_events;
    std::vector<MatchHeader>  match_headers;     // side table for MATCH_START
    uint32_t                  total_input_count   = 0;  // INPUT events in session_events
    uint32_t                  session_start_frame = 0;  // INPUT-frame index of session_events[0]
                                                         // (always 0 unless we ever drop history)

    // Pending broadcast cursor: events past this index are unflushed.
    // FlushBatch encodes [last_flushed_event_idx .. session_events.size()),
    // emits as one EVENT_BATCH datagram, advances the cursor.
    size_t                    last_flushed_event_idx = 0;
    uint32_t                  flushed_input_count    = 0;  // INPUT events flushed so far

    // Index of the most recent MATCH_START event in session_events. Used by
    // SpectatorNode_WriteCurrentBattleFile to slice the per-battle segment.
    // -1 sentinel = no MATCH_START emitted in this session yet.
    int64_t                   last_match_start_idx   = -1;

    // Index of the FIRST state-init event in the contiguous init block
    // immediately preceding the most recent MATCH_START. Set in
    // SpectatorNode_AppendMatchStart by backward-scanning from
    // last_match_start_idx through PIN_RNG / RESET_INPUT_STATE /
    // SOUND_INIT / SESSION_ID events until a non-init event is hit.
    // WriteCurrentBattleFile slices from THIS index instead of
    // last_match_start_idx so the .fm2krep file is fully self-replayable
    // (RNG seed re-pin, input ring reset, sound layer init all included).
    // Defaults to last_match_start_idx if no init block precedes it.
    int64_t                   last_pre_match_init_idx = -1;

    // Snapshot cache (task #18 phase 2). Refreshed by StashSnapshot at
    // every Netplay_StartBattle. When a spectator joins with mode=
    // CURRENT_MATCH, the host's TickHostMaintenance bind path will (in
    // phase 3) send this blob via SNAPSHOT_BEGIN/CHUNK/END instead of
    // calling SendSessionBackfillTo from frame 0. Spectator's
    // SaveState_Load on receipt skips every prior match.
    //
    // Empty / invalid before the first match starts. New session_events
    // accumulating during pre-battle CSS don't disturb the previous
    // snapshot — it just goes stale until the next StashSnapshot
    // overwrites it.
    struct SnapshotCache {
        std::vector<uint8_t> blob;
        uint32_t             input_frame        = 0;
        uint32_t             match_index        = 0;
        uint32_t             checksum           = 0;
        // captured_game_mode: g_game_mode at the moment we captured this
        // snapshot (2000 = CSS, 3000 = battle). Phase E uses this to gate
        // the spec-side apply on a matching mode so a CSS snapshot doesn't
        // get applied during the spec's battle phase (wrong state regions
        // would dominate). 0 means "legacy battle-only capture" — apply
        // when spec reaches game_mode >= 3000.
        uint32_t             captured_game_mode = 0;
        bool                 valid              = false;
    } current_snapshot;

    // ─── VIEWER SIDE: playback queue ───────────────────────────────────
    // Populated by HandleSpecData (decodes incoming EVENT_BATCH /
    // INPUT_BATCH); consumed by RunSpectatorTick. Mirrors the host's
    // session_events shape: typed events drained in order, non-INPUT
    // events at the head are applied (RNG pin etc.) before the next
    // INPUT pops. C2 keeps non-INPUT drain as a no-op gate; C3+ wires
    // the apply paths.
    std::vector<SessionEvent> pb_queue;
    std::vector<MatchHeader>  pb_match_headers;

    // Snapshot reassembly buffer (task #18 phase 4). Filled across
    // SNAPSHOT_BEGIN → SNAPSHOT_CHUNK*N → SNAPSHOT_END from the upstream's
    // CURRENT_MATCH bind path. Applied once via SaveState_LoadFromBytes
    // when END validates; cleared on apply or on protocol error.
    //
    // anchor_frame = SNAPSHOT_BEGIN's start_frame, replayed into
    // next_expected_frame at apply time so the EVENT_BATCH stream that
    // follows resumes exactly where the host stashed the snapshot.
    struct SnapshotInbox {
        std::vector<uint8_t> blob;
        SnapshotMetadata     meta            = {};
        uint32_t             anchor_frame    = 0;
        size_t               bytes_received  = 0;
        bool                 active          = false;
        // Set by SNAPSHOT_END once the blob has been validated (size +
        // fletcher32) but the SaveState_LoadFromBytes call is held back
        // because the local game hasn't booted past mode 0 yet.
        // SpectatorNode_ApplyPendingSnapshot polls each spec tick, sees
        // game_mode > 0, and runs the actual apply. Without this gate the
        // raw memcpy lands during the engine's pre-WinMain init and the
        // next render tick reads from corrupted state → crash. Observed
        // on Vanguard Princess (1.08 MB snapshots arrived 12ms after
        // JOIN_ACK, before mode flipped 0→1000).
        bool                 pending_apply   = false;
    } pb_snapshot_inbox;

    // Viewer-side state (this node subscribed upstream).
    bool                      subscribed_upstream = false;
    bool                      join_req_pending    = false;  // We sent SPEC_JOIN_REQ; expect ACK.
    sockaddr_in               upstream_addr       = {};
    // Sticky copy of the mode we declared on the FIRST RequestJoin from
    // Netplay_StartSpectator. The reconnect path (silence failover) calls
    // RequestJoin(root) without specifying a mode, which would otherwise
    // fall through to the SpectatorNode_RequestJoin default (FULL_SESSION)
    // and clobber the original CURRENT_MATCH preference — host then ships
    // no snapshot, spec sits with placeholder chars on a /F-booted battle.
    SpecJoinMode              last_requested_mode = SpecJoinMode::FULL_SESSION;
    // Failover support. root_addr is the originally-configured upstream
    // (the actual match host). If our current upstream goes silent we
    // fall back to root, which always-on by design. Liveness is tracked
    // via SpectatorTCP::LastUpstreamRecvMs() (TCP-side) — this struct
    // owns only handshake / heartbeat send-cadence state.
    sockaddr_in               root_addr           = {};
    uint64_t                  last_heartbeat_send_ms = 0;
    uint64_t                  last_reconnect_attempt_ms = 0;
    // De-dup gate. Backfill from a reconnected upstream replays history
    // from frame 0; frames at or below this counter were already consumed
    // locally and would re-render the past. Updated by PopFrameInputs.
    uint32_t                  highest_consumed_frame = 0;
    // Frame-counter offset so highest_consumed_frame is comparable to
    // the start_frame field on incoming EVENT_BATCH datagrams. Set on the
    // first batch received after JOIN_ACK; subsequent batches increment.
    bool                      have_frame_baseline = false;
    uint32_t                  next_expected_frame = 0;  // session-relative INPUT-frame index
    // Pull-based gap recovery: periodic + on-demand INPUT_REQUEST
    // (spectator → upstream) for the missing frame range. Closes any
    // gap that the push-based redundancy window can't recover from.
    uint64_t                  last_input_request_send_ms = 0;

    // Viewer-side playback driver state.
    // Populated by HandleSpecData (INITIAL_MATCH / EVENT_BATCH / MATCH_END);
    // consumed by the trampoline's RunSpectatorTick + Hook_GetPlayerInput.
    bool                      playing_back        = false;
    uint32_t                  pb_initial_seed     = 0;
    uint32_t                  pb_initial_state    = 0;  // fingerprint sanity
    uint8_t                   pb_p1_char          = 0;
    uint8_t                   pb_p1_color         = 0;
    uint8_t                   pb_p2_char          = 0;
    uint8_t                   pb_p2_color         = 0;
    uint8_t                   pb_stage_id         = 0;
    uint16_t                  pb_current_p1       = 0;
    uint16_t                  pb_current_p2       = 0;
    // PIN_RNG seed deferred from event drain to battle-entry. The host
    // emitted PIN_RNG at MATCH_START (battle entry). Applying it on the
    // spec side at the first event drain (= title screen) would let the
    // pre-battle sim mutate the seed further, breaking rng parity with
    // host's battle frame 0. Stash it here; SpectatorSimOneFrame writes
    // it to engine RNG when game_mode flips to 3000.
    uint32_t                  pending_pin_rng_seed  = 0;
    bool                      pending_pin_rng_valid = false;

    // C7 — host's session_id for this peer connection. Generated lazily
    // (first AppendSessionId call) and stays stable until the SpectatorNode
    // is shut down or the next AppendSessionId overwrites it. The wire
    // event sources g_state.session_events; the file writer sources this
    // field; spectator-side application copies the wire payload here so
    // a spectator's own .fm2kset/.fm2krep recordings carry the same id
    // as the upstream's.
    uint64_t                  session_id           = 0;
};

State g_state;

// Per-session tracking of which GekkoSpectator addrs we've already added.
// GekkoNet has no remove-actor API (gekkonet.h:185-203 -- only
// gekko_add_actor exists), so a naive add-on-rejoin pattern leaks one
// GekkoSpectator actor per spec retry. Over a 5s-retry storm, the host's
// per-tick spectator iteration cost grows linearly and crushes the frame
// budget down to single-digit FPS.
//
// Fix: gate gekko_add_actor on this set. Cleared once per session boundary
// by SpectatorNode_ClearGekkoSpectatorTracking() (called from netplay.cpp
// after each fresh gekko_create + gekko_start -- new session = no actors
// yet = empty set). Worst case per session: one zombie actor per
// ever-seen spec addr, instead of one per retry.
//
// Keyed by "ip:port" string (matches the addr_str gekko_add_actor sees).
// std::set instead of unordered_set so we don't have to hash, and the
// member count is tiny (single-digit per match in practice).
std::set<std::string> g_gekko_spectator_addrs;

// In-flight TCP punch sockets — see TickHostMaintenance comment for why
// we defer close. Each entry holds a SOCKET handle and the wall-clock
// deadline (ms) after which it's safe to closesocket(). The sweep at
// the bottom of TickHostMaintenance (which runs every frame) processes
// expired entries. Bounded by spectator-join rate, ~1/sec at most.
struct PendingPunchSock {
    SOCKET   handle;
    uint64_t close_after_ms;
};
std::vector<PendingPunchSock> g_pending_punch_sockets;

// Classic Fletcher-32 over a byte buffer. Used both by the C9 desync
// fingerprint and the task-#18 snapshot-blob checksum. Self-contained so
// the test-side mirror can verify by replicating the algorithm without
// pulling in any production headers.
uint32_t Fletcher32(const uint8_t* data, size_t len) {
    uint32_t sum1 = 0xFFFF, sum2 = 0xFFFF;
    size_t i = 0;
    while (i + 1 < len) {
        uint16_t w = (uint16_t)data[i] | ((uint16_t)data[i + 1] << 8);
        sum1 = (sum1 + w)    % 0xFFFFu;
        sum2 = (sum2 + sum1) % 0xFFFFu;
        i += 2;
    }
    if (i < len) {
        uint16_t w = data[i];
        sum1 = (sum1 + w)    % 0xFFFFu;
        sum2 = (sum2 + sum1) % 0xFFFFu;
    }
    return (sum2 << 16) | sum1;
}

// =============================================================================
// UDP HELPERS
// =============================================================================

bool AddrEqual(const sockaddr_in& a, const sockaddr_in& b) {
    return a.sin_family == b.sin_family
        && a.sin_port   == b.sin_port
        && a.sin_addr.s_addr == b.sin_addr.s_addr;
}

void FormatAddr(const sockaddr_in& a, char* out, size_t out_sz) {
    char ip[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &a.sin_addr, ip, sizeof(ip));
    std::snprintf(out, out_sz, "%s:%u", ip, ntohs(a.sin_port));
}

// Send a raw UDP buffer to a peer via the shared socket.
void SendRaw(const void* buf, size_t len, const sockaddr_in& to) {
    SOCKET sock = NetSocket_GetHandle();
    if (sock == INVALID_SOCKET) return;
    sendto(sock, reinterpret_cast<const char*>(buf), static_cast<int>(len), 0,
           reinterpret_cast<const sockaddr*>(&to), sizeof(to));
}

// =============================================================================
// BROADCAST
// =============================================================================

// Append one event's wire encoding into a byte vector. Used by both the
// live broadcast path (FlushBatch) and the backfill path
// (SendSessionBackfillTo) — keeps wire-encoding logic in one place.
void AppendEventToWire(std::vector<uint8_t>& out, const SessionEvent& ev,
                       const std::vector<MatchHeader>& headers) {
    uint8_t buf[SESSION_EVENT_MAX_WIRE_SIZE];
    size_t w = 0;
    switch (ev.type) {
        case SessionEventType::INPUT:
            w = SessionEvent_EncodeInput(buf, sizeof(buf),
                                         ev.u.input.p1, ev.u.input.p2);
            break;
        case SessionEventType::PIN_RNG:
            w = SessionEvent_EncodePinRng(buf, sizeof(buf), ev.u.pin_rng_seed);
            break;
        case SessionEventType::RESET_INPUT_STATE:
            w = SessionEvent_EncodeResetInputState(buf, sizeof(buf));
            break;
        case SessionEventType::SOUND_INIT:
            w = SessionEvent_EncodeSoundInit(buf, sizeof(buf));
            break;
        case SessionEventType::MATCH_START:
            if (ev.u.match_start_idx < headers.size()) {
                w = SessionEvent_EncodeMatchStart(buf, sizeof(buf),
                                                  headers[ev.u.match_start_idx].data());
            }
            break;
        case SessionEventType::MATCH_END:
            w = SessionEvent_EncodeMatchEnd(buf, sizeof(buf), ev.u.match_end);
            break;
        case SessionEventType::FINGERPRINT:
            w = SessionEvent_EncodeFingerprint(buf, sizeof(buf), ev.u.fingerprint_hash);
            break;
        case SessionEventType::ROUND_START:
            w = SessionEvent_EncodeRoundStart(buf, sizeof(buf), ev.u.round_start);
            break;
        case SessionEventType::ROUND_END:
            w = SessionEvent_EncodeRoundEnd(buf, sizeof(buf), ev.u.round_end);
            break;
        case SessionEventType::SESSION_ID:
            w = SessionEvent_EncodeSessionId(buf, sizeof(buf), ev.u.session_id);
            break;
    }
    if (w > 0) out.insert(out.end(), buf, buf + w);
}

// Count INPUT events in [first, last) of a SessionEvent slice. Used to
// populate SpecDataHeader.frame_count for log/diagnostic purposes — wire
// dedup uses start_frame.
uint32_t CountInputs(const std::vector<SessionEvent>& events,
                     size_t first, size_t last) {
    uint32_t n = 0;
    for (size_t i = first; i < last; i++) {
        if (events[i].type == SessionEventType::INPUT) ++n;
    }
    return n;
}

// Emit an EVENT_BATCH datagram covering all events appended since the last
// flush. TCP guarantees in-order, exactly-once delivery — no redundancy
// window. The header carries:
//   start_frame = session-relative INPUT-frame index of the first INPUT in
//                 this batch (= flushed_input_count at entry). Used by the
//                 receiver's next_expected_frame dedup gate.
//   frame_count = number of INPUT events in this batch (informational).
//   payload     = packed SessionEvent[] (1-byte type tag + variant payload
//                 per event; see SessionEvent_Encode* in this file).
void FlushBatch() {
    const size_t first = g_state.last_flushed_event_idx;
    const size_t last  = g_state.session_events.size();
    if (first == last) return;

    const uint32_t input_count = CountInputs(g_state.session_events, first, last);
    const uint32_t start_frame = g_state.flushed_input_count;

    // Advance flush watermark unconditionally so the cadence trigger
    // ("every BROADCAST_BATCH_FRAMES INPUT events") keeps walking even
    // when there are no subscribers to receive the batch.
    g_state.last_flushed_event_idx = last;
    g_state.flushed_input_count   += input_count;

    if (g_state.subscribers.empty()) return;

    std::vector<uint8_t> payload;
    payload.reserve((last - first) * SESSION_EVENT_MAX_WIRE_SIZE);
    for (size_t i = first; i < last; i++) {
        AppendEventToWire(payload, g_state.session_events[i], g_state.match_headers);
    }
    if (payload.empty()) return;

    std::vector<uint8_t> buf(sizeof(SpecDataHeader) + payload.size());
    SpecDataHeader hdr = {};
    hdr.magic       = SPEC_DATA_MAGIC;
    hdr.type        = SpecDataType::EVENT_BATCH;
    hdr.start_frame = start_frame;
    hdr.frame_count = static_cast<uint16_t>(std::min<uint32_t>(input_count, 0xFFFFu));
    // For EVENT_BATCH, flags carries the payload byte count so the TCP
    // framer can size the receive without re-decoding events. 16-bit cap
    // (65535) is well above any reasonable live FlushBatch — backfill
    // chunks are explicitly bounded at BACKFILL_CHUNK_BYTES=1024.
    hdr.flags       = static_cast<uint16_t>(std::min<size_t>(payload.size(), 0xFFFFu));
    std::memcpy(buf.data(), &hdr, sizeof(hdr));
    std::memcpy(buf.data() + sizeof(hdr), payload.data(), payload.size());

    SpectatorTCP::BroadcastToAll(buf.data(), buf.size());
}

// Legacy SendInitialMatchTo / INITIAL_MATCH packet path removed in C12.
// MATCH_START flows as a SessionEvent op interleaved with INPUTs in the
// EVENT_BATCH stream; late joiners get it via SendSessionBackfillTo.

// Send the full session event log to a specific subscriber, chunked at
// ~BACKFILL_CHUNK_BYTES per datagram. Used on JOIN_ACK so a late joiner
// can fast-forward from session start to live. Each chunk is its own
// EVENT_BATCH datagram. Hdr.start_frame = session-relative INPUT-frame
// index of the first INPUT in the chunk; receiver's dedup gate uses this
// to detect gaps / redundant retransmits across reconnects.
//
// Bytes-based chunking (vs the old fixed-frame chunking) handles variable-
// length events: a chunk with mostly INPUT events fits ~200 events; a
// chunk with a 96-byte MATCH_START fits fewer. Sized to stay under typical
// UDP MTU (1200 B safe) once we keep TCP — over TCP this only matters for
// receive-buffer pacing and avoids an oversized syscall stalling the
// session_events traversal.
// 8 KB chunks — a 1000-INPUT backfill (5 B/INPUT) + non-INPUT ops fits
// in a single chunk. The previous 1 KB cap chunked into ~3 chunks; in
// practice spectators only ever received the FIRST chunk, leaving a
// 500+ frame gap that they couldn't bridge before live EVENT_BATCH
// broadcasts started flowing (host's pkmncc match — see 07:54 logs).
// Root cause of the multi-chunk delivery loss is still unclear (TCP
// is reliable; both sides on localhost) but a single-chunk send
// sidesteps it. 8 KB stays well under the uint16 hdr.flags cap (65 KB).
constexpr size_t BACKFILL_CHUNK_BYTES = 8192;

// Walk session_events from a given index/INPUT-frame anchor and stream
// chunked EVENT_BATCH datagrams to a single subscriber. The legacy
// "from frame 0" backfill is just (first_event_idx=0,
// start_input_frame=session_start_frame); the snapshot-join path
// (task #18 phase 3) calls with the snapshot's anchor — events emitted
// reflect "everything from the snapshot's frame onward," not the
// preceding history we already shipped via SNAPSHOT_*.
void SendSessionEventsTo(const sockaddr_in& to,
                         size_t   first_event_idx,
                         uint32_t start_input_frame) {
    const size_t total_events = g_state.session_events.size();
    if (first_event_idx >= total_events) return;

    char addr_buf[48] = {};
    FormatAddr(to, addr_buf, sizeof(addr_buf));
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "SpectatorNode: streaming events [%zu..%zu) (%u INPUTs total in session) "
                "to %s, anchor INPUT-frame=%u",
                first_event_idx, total_events, g_state.total_input_count,
                addr_buf, start_input_frame);

    // Walk session_events, packing into chunks bounded by BACKFILL_CHUNK_BYTES.
    // For each chunk, hdr.start_frame is the INPUT-frame index of the first
    // INPUT *in the chunk*. If a chunk is non-INPUT-only (rare; trailing
    // ops at session tail), use the running input cursor.
    size_t   ev_idx          = first_event_idx;
    uint32_t cursor_inputs   = start_input_frame;

    while (ev_idx < total_events) {
        std::vector<uint8_t> payload;
        payload.reserve(BACKFILL_CHUNK_BYTES);

        const size_t chunk_first_idx   = ev_idx;
        uint32_t     chunk_first_input = cursor_inputs;
        bool         saw_input         = false;
        uint32_t     chunk_input_count = 0;

        while (ev_idx < total_events) {
            const SessionEvent& ev = g_state.session_events[ev_idx];
            // Pre-encode to know the size; if it'd overflow the chunk
            // budget, defer to the next chunk (unless the chunk is empty,
            // in which case ship even an oversize event alone).
            uint8_t one[SESSION_EVENT_MAX_WIRE_SIZE];
            size_t  one_w = 0;
            switch (ev.type) {
                case SessionEventType::INPUT:
                    one_w = SessionEvent_EncodeInput(one, sizeof(one),
                                                     ev.u.input.p1, ev.u.input.p2);
                    break;
                case SessionEventType::PIN_RNG:
                    one_w = SessionEvent_EncodePinRng(one, sizeof(one),
                                                      ev.u.pin_rng_seed);
                    break;
                case SessionEventType::RESET_INPUT_STATE:
                    one_w = SessionEvent_EncodeResetInputState(one, sizeof(one));
                    break;
                case SessionEventType::SOUND_INIT:
                    one_w = SessionEvent_EncodeSoundInit(one, sizeof(one));
                    break;
                case SessionEventType::MATCH_START:
                    if (ev.u.match_start_idx < g_state.match_headers.size()) {
                        one_w = SessionEvent_EncodeMatchStart(one, sizeof(one),
                            g_state.match_headers[ev.u.match_start_idx].data());
                    }
                    break;
                case SessionEventType::MATCH_END:
                    one_w = SessionEvent_EncodeMatchEnd(one, sizeof(one),
                                                        ev.u.match_end);
                    break;
                case SessionEventType::FINGERPRINT:
                    one_w = SessionEvent_EncodeFingerprint(one, sizeof(one),
                                                           ev.u.fingerprint_hash);
                    break;
                case SessionEventType::ROUND_START:
                    one_w = SessionEvent_EncodeRoundStart(one, sizeof(one),
                                                          ev.u.round_start);
                    break;
                case SessionEventType::ROUND_END:
                    one_w = SessionEvent_EncodeRoundEnd(one, sizeof(one),
                                                        ev.u.round_end);
                    break;
                case SessionEventType::SESSION_ID:
                    one_w = SessionEvent_EncodeSessionId(one, sizeof(one),
                                                         ev.u.session_id);
                    break;
            }
            if (one_w == 0) { ++ev_idx; continue; }   // unknown / unencodable

            if (!payload.empty() && payload.size() + one_w > BACKFILL_CHUNK_BYTES) {
                break;  // emit current chunk; this event goes in the next
            }
            payload.insert(payload.end(), one, one + one_w);

            if (ev.type == SessionEventType::INPUT) {
                if (!saw_input) {
                    saw_input         = true;
                    chunk_first_input = cursor_inputs;
                }
                ++chunk_input_count;
                ++cursor_inputs;
            }
            ++ev_idx;
        }

        if (payload.empty()) break;

        std::vector<uint8_t> buf(sizeof(SpecDataHeader) + payload.size());
        SpecDataHeader hdr = {};
        hdr.magic       = SPEC_DATA_MAGIC;
        hdr.type        = SpecDataType::EVENT_BATCH;
        hdr.start_frame = saw_input ? chunk_first_input
                                    : cursor_inputs;     // tail-only chunk
        hdr.frame_count = static_cast<uint16_t>(std::min<uint32_t>(chunk_input_count, 0xFFFFu));
        // EVENT_BATCH: flags carries payload byte count for the receiver's
        // TCP framer (see PayloadLenForType in spectator_tcp.cpp). The
        // BACKFILL_CHUNK_BYTES=1024 cap keeps this well under the 16-bit
        // limit. (Was: bit 0 flagged deferred-ops chunk — moot now since
        // the framer needs the byte count regardless.)
        hdr.flags       = static_cast<uint16_t>(std::min<size_t>(payload.size(), 0xFFFFu));
        std::memcpy(buf.data(), &hdr, sizeof(hdr));
        std::memcpy(buf.data() + sizeof(hdr), payload.data(), payload.size());

        SpectatorTCP::SendTo(to, buf.data(), buf.size());

        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "SpectatorNode:   chunk sent: start_frame=%u count=%u "
            "payload=%zu bytes (events [%zu..%zu))",
            (unsigned)hdr.start_frame, (unsigned)hdr.frame_count,
            payload.size(), chunk_first_idx, ev_idx);
    }
}

// Legacy entry point: ships ALL session_events from frame 0. Used by
// FULL_SESSION-mode subscribers and as the back-compat fallback when
// CURRENT_MATCH was requested but no snapshot has been captured yet
// (e.g. JOIN_REQ landed mid-CSS before any battle started).
void SendSessionBackfillTo(const sockaddr_in& to) {
    SendSessionEventsTo(to, 0, g_state.session_start_frame);
}

// Phase 3 entry point: ships only events at-or-after the given INPUT-frame
// anchor. Paired with SendSnapshotTo for CURRENT_MATCH-mode subscribers —
// the snapshot covers state up to its anchor frame, then this fills in
// the events from there to live edge.
//
// Linear scan over session_events to find the first event whose
// session-relative INPUT-frame index >= anchor_frame. Non-INPUT events
// (PIN_RNG / RESET / SOUND_INIT / MATCH_START / MATCH_END / FINGERPRINT)
// don't have an explicit frame number; they belong to "the INPUT that
// follows them" so we keep them grouped. We therefore find the FIRST
// INPUT >= anchor and back up to include any preceding non-INPUT ops in
// the same group.
void SendSessionBackfillFromFrame(const sockaddr_in& to,
                                  uint32_t anchor_input_frame) {
    const size_t total = g_state.session_events.size();
    if (total == 0) return;

    // Walk session_events tracking the running input-frame cursor; find
    // the first INPUT event whose cursor value == anchor_input_frame.
    uint32_t cursor = g_state.session_start_frame;
    size_t   first_idx = total;   // sentinel: nothing matched
    for (size_t i = 0; i < total; ++i) {
        const SessionEvent& ev = g_state.session_events[i];
        if (ev.type == SessionEventType::INPUT) {
            if (cursor >= anchor_input_frame) {
                first_idx = i;
                break;
            }
            ++cursor;
        }
    }

    if (first_idx >= total) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "SpectatorNode: no events at-or-after INPUT-frame=%u "
            "(total INPUTs=%u, snapshot is at the live edge)",
            anchor_input_frame, g_state.total_input_count);
        return;
    }

    // Back up over any non-INPUT ops immediately preceding first_idx
    // so they belong to the SAME chunk as their following INPUT (the
    // spectator's drain semantic applies them just before that INPUT
    // pops). Without this, ops would land in the previous chunk that
    // we're skipping → spectator misses PIN_RNG/etc that should fire
    // at the snapshot's anchor frame.
    while (first_idx > 0 &&
           g_state.session_events[first_idx - 1].type != SessionEventType::INPUT) {
        --first_idx;
    }

    SendSessionEventsTo(to, first_idx, anchor_input_frame);
}

// Phase 3: ship the cached SaveState blob to a single subscriber as a
// SNAPSHOT_BEGIN / SNAPSHOT_CHUNK*N / SNAPSHOT_END sequence. Spectator
// reassembles in HandleSpecData (phase 4) and calls SaveState_Load on
// END. After this, host must call SendSessionBackfillFromFrame with the
// snapshot's input_frame anchor so the spectator's pb_queue picks up at
// the same point the loaded state expects.
//
// Idempotent and side-effect-free on g_state. Caller is responsible for
// ordering this BEFORE BroadcastToAll re-engages for the new sub
// (TickHostMaintenance handles that via the backfill_done fence).
void SendSnapshotTo(const sockaddr_in& to) {
    const auto& cache = g_state.current_snapshot;
    if (!cache.valid || cache.blob.empty()) return;

    char addr_buf[48] = {};
    FormatAddr(to, addr_buf, sizeof(addr_buf));
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "SpectatorNode: shipping snapshot to %s "
                "(match=%u, %zu bytes, anchor INPUT-frame=%u)",
                addr_buf, cache.match_index, cache.blob.size(), cache.input_frame);

    // ---- SNAPSHOT_BEGIN ----------------------------------------------
    {
        SnapshotMetadata meta = {};
        meta.version            = SPECTATOR_SNAPSHOT_VERSION;
        meta.total_bytes        = (uint32_t)cache.blob.size();
        meta.match_index        = cache.match_index;
        // Phase E: tell the spec which mode this snapshot was captured
        // at. v0.2.41 spectators see this as `reserved1` and ignore it
        // (their apply gate is hard-coded to `game_mode >= 3000` which
        // matches battle-only captures). v0.2.42+ spectators check the
        // field and apply when their local engine reaches the matching
        // mode.
        meta.captured_game_mode = cache.captured_game_mode;

        std::vector<uint8_t> buf(sizeof(SpecDataHeader) + sizeof(meta));
        SpecDataHeader hdr = {};
        hdr.magic       = SPEC_DATA_MAGIC;
        hdr.type        = SpecDataType::SNAPSHOT_BEGIN;
        hdr.start_frame = cache.input_frame;       // anchor for spectator's next_expected_frame
        hdr.frame_count = 0;
        hdr.flags       = (uint16_t)sizeof(meta);  // 16 — payload byte count
        std::memcpy(buf.data(), &hdr, sizeof(hdr));
        std::memcpy(buf.data() + sizeof(hdr), &meta, sizeof(meta));
        SpectatorTCP::SendTo(to, buf.data(), buf.size());
    }

    // ---- SNAPSHOT_CHUNK xN -------------------------------------------
    size_t emitted = 0;
    while (emitted < cache.blob.size()) {
        const size_t remaining = cache.blob.size() - emitted;
        const size_t chunk_n   = std::min(remaining, SPECTATOR_SNAPSHOT_CHUNK_BYTES);

        std::vector<uint8_t> buf(sizeof(SpecDataHeader) + chunk_n);
        SpecDataHeader hdr = {};
        hdr.magic       = SPEC_DATA_MAGIC;
        hdr.type        = SpecDataType::SNAPSHOT_CHUNK;
        hdr.start_frame = (uint32_t)emitted;       // byte offset (running)
        hdr.frame_count = 0;
        hdr.flags       = (uint16_t)chunk_n;
        std::memcpy(buf.data(), &hdr, sizeof(hdr));
        std::memcpy(buf.data() + sizeof(hdr),
                    cache.blob.data() + emitted, chunk_n);
        SpectatorTCP::SendTo(to, buf.data(), buf.size());

        emitted += chunk_n;
    }

    // ---- SNAPSHOT_END -------------------------------------------------
    {
        std::vector<uint8_t> buf(sizeof(SpecDataHeader) + sizeof(uint32_t));
        SpecDataHeader hdr = {};
        hdr.magic       = SPEC_DATA_MAGIC;
        hdr.type        = SpecDataType::SNAPSHOT_END;
        hdr.start_frame = 0;
        hdr.frame_count = 0;
        hdr.flags       = (uint16_t)sizeof(uint32_t);  // 4
        std::memcpy(buf.data(), &hdr, sizeof(hdr));
        std::memcpy(buf.data() + sizeof(hdr), &cache.checksum, sizeof(uint32_t));
        SpectatorTCP::SendTo(to, buf.data(), buf.size());
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "SpectatorNode: snapshot shipped (%zu bytes in %zu chunks, fletcher32=0x%08X)",
                cache.blob.size(),
                (cache.blob.size() + SPECTATOR_SNAPSHOT_CHUNK_BYTES - 1) /
                    SPECTATOR_SNAPSHOT_CHUNK_BYTES,
                cache.checksum);
}

// Legacy SendMatchEndToAll / MATCH_END packet path removed in C12.
// MATCH_END flows as a SessionEvent op (see SpectatorNode_AppendMatchEnd).

// (SendInputRequest + RespondToInputRequest deleted: TCP guarantees in-order
// delivery exactly once, so the spectator-side gap-recovery handshake is
// dead code. The whole class of UDP-loss recovery — REDUNDANCY_WINDOW,
// INPUT_REQUEST_POLL_MS in TickHealth, the on-gap immediate request inside
// HandleSpecData — has been removed.)

} // namespace

// =============================================================================
// SESSION EVENT WIRE FORMAT (C1)
// =============================================================================
//
// Pure byte-level encoders/decoders. No socket / state side effects — these
// just mediate between SessionEvent values and packed wire bytes. Production
// integration (vector<SessionEvent> session_history, head-of-queue drain in
// RunSpectatorTick, etc.) is layered on top in C2+.

namespace {

// Wire payload size (excluding the 1-byte type tag) per SessionEventType.
// Used by both encoders (sanity) and the decoder (bounds check before memcpy).
size_t WirePayloadSize(SessionEventType t) {
    switch (t) {
        case SessionEventType::INPUT:             return 4;   // u16 + u16
        case SessionEventType::PIN_RNG:           return 4;   // u32
        case SessionEventType::RESET_INPUT_STATE: return 0;
        case SessionEventType::SOUND_INIT:        return 0;
        case SessionEventType::MATCH_START:       return SESSION_EVENT_MATCH_HDR_SIZE;
        case SessionEventType::MATCH_END:         return sizeof(MatchEndPayload);    // 7  (was 0 in v1)
        case SessionEventType::FINGERPRINT:       return 4;   // u32
        case SessionEventType::ROUND_START:       return sizeof(RoundStartPayload);  // 7
        case SessionEventType::ROUND_END:         return sizeof(RoundEndPayload);    // 9
        case SessionEventType::SESSION_ID:        return 8;   // u64
    }
    return SIZE_MAX;  // unknown tag — caller treats as malformed
}

bool IsValidEventTag(uint8_t tag) {
    return tag >= static_cast<uint8_t>(SessionEventType::INPUT)
        && tag <= static_cast<uint8_t>(SessionEventType::SESSION_ID);
}

} // namespace

size_t SessionEvent_EncodeInput(uint8_t* out, size_t cap, uint16_t p1, uint16_t p2) {
    constexpr size_t need = 1 + 4;
    if (cap < need) return 0;
    out[0] = static_cast<uint8_t>(SessionEventType::INPUT);
    std::memcpy(out + 1, &p1, 2);
    std::memcpy(out + 3, &p2, 2);
    return need;
}

size_t SessionEvent_EncodePinRng(uint8_t* out, size_t cap, uint32_t seed) {
    constexpr size_t need = 1 + 4;
    if (cap < need) return 0;
    out[0] = static_cast<uint8_t>(SessionEventType::PIN_RNG);
    std::memcpy(out + 1, &seed, 4);
    return need;
}

size_t SessionEvent_EncodeResetInputState(uint8_t* out, size_t cap) {
    if (cap < 1) return 0;
    out[0] = static_cast<uint8_t>(SessionEventType::RESET_INPUT_STATE);
    return 1;
}

size_t SessionEvent_EncodeSoundInit(uint8_t* out, size_t cap) {
    if (cap < 1) return 0;
    out[0] = static_cast<uint8_t>(SessionEventType::SOUND_INIT);
    return 1;
}

size_t SessionEvent_EncodeMatchStart(uint8_t* out, size_t cap,
                                     const uint8_t header[SESSION_EVENT_MATCH_HDR_SIZE]) {
    constexpr size_t need = 1 + SESSION_EVENT_MATCH_HDR_SIZE;
    if (cap < need) return 0;
    out[0] = static_cast<uint8_t>(SessionEventType::MATCH_START);
    std::memcpy(out + 1, header, SESSION_EVENT_MATCH_HDR_SIZE);
    return need;
}

size_t SessionEvent_EncodeMatchEnd(uint8_t* out, size_t cap, const MatchEndPayload& p) {
    constexpr size_t need = 1 + sizeof(MatchEndPayload);
    if (cap < need) return 0;
    out[0] = static_cast<uint8_t>(SessionEventType::MATCH_END);
    std::memcpy(out + 1, &p, sizeof(p));
    return need;
}

size_t SessionEvent_EncodeSessionId(uint8_t* out, size_t cap, uint64_t session_id) {
    constexpr size_t need = 1 + 8;
    if (cap < need) return 0;
    out[0] = static_cast<uint8_t>(SessionEventType::SESSION_ID);
    std::memcpy(out + 1, &session_id, 8);
    return need;
}

size_t SessionEvent_EncodeRoundStart(uint8_t* out, size_t cap, const RoundStartPayload& p) {
    constexpr size_t need = 1 + sizeof(RoundStartPayload);
    if (cap < need) return 0;
    out[0] = static_cast<uint8_t>(SessionEventType::ROUND_START);
    std::memcpy(out + 1, &p, sizeof(p));
    return need;
}

size_t SessionEvent_EncodeRoundEnd(uint8_t* out, size_t cap, const RoundEndPayload& p) {
    constexpr size_t need = 1 + sizeof(RoundEndPayload);
    if (cap < need) return 0;
    out[0] = static_cast<uint8_t>(SessionEventType::ROUND_END);
    std::memcpy(out + 1, &p, sizeof(p));
    return need;
}

size_t SessionEvent_EncodeFingerprint(uint8_t* out, size_t cap, uint32_t hash) {
    constexpr size_t need = 1 + 4;
    if (cap < need) return 0;
    out[0] = static_cast<uint8_t>(SessionEventType::FINGERPRINT);
    std::memcpy(out + 1, &hash, 4);
    return need;
}

size_t SessionEvent_Decode(const uint8_t* in, size_t in_len,
                           SessionEvent* out_event,
                           uint8_t out_match_header[SESSION_EVENT_MATCH_HDR_SIZE]) {
    if (in_len < 1) return 0;
    const uint8_t tag = in[0];
    if (!IsValidEventTag(tag)) return 0;

    const SessionEventType type = static_cast<SessionEventType>(tag);
    const size_t payload = WirePayloadSize(type);
    if (in_len < 1 + payload) return 0;

    if (out_event) {
        out_event->type = type;
        // Zero the union so unused-variant readers see deterministic 0s.
        std::memset(&out_event->u, 0, sizeof(out_event->u));

        switch (type) {
            case SessionEventType::INPUT:
                std::memcpy(&out_event->u.input.p1, in + 1, 2);
                std::memcpy(&out_event->u.input.p2, in + 3, 2);
                break;
            case SessionEventType::PIN_RNG:
                std::memcpy(&out_event->u.pin_rng_seed, in + 1, 4);
                break;
            case SessionEventType::FINGERPRINT:
                std::memcpy(&out_event->u.fingerprint_hash, in + 1, 4);
                break;
            case SessionEventType::MATCH_START:
                // u.match_start_idx left at 0 — caller assigns when appending
                // to its match_headers side table.
                break;
            case SessionEventType::ROUND_START:
                std::memcpy(&out_event->u.round_start, in + 1, sizeof(RoundStartPayload));
                break;
            case SessionEventType::ROUND_END:
                std::memcpy(&out_event->u.round_end, in + 1, sizeof(RoundEndPayload));
                break;
            case SessionEventType::MATCH_END:
                std::memcpy(&out_event->u.match_end, in + 1, sizeof(MatchEndPayload));
                break;
            case SessionEventType::SESSION_ID:
                std::memcpy(&out_event->u.session_id, in + 1, 8);
                break;
            case SessionEventType::RESET_INPUT_STATE:
            case SessionEventType::SOUND_INIT:
                break;
        }
    }
    if (type == SessionEventType::MATCH_START && out_match_header) {
        std::memcpy(out_match_header, in + 1, SESSION_EVENT_MATCH_HDR_SIZE);
    }

    return 1 + payload;
}

// =============================================================================
// PUBLIC API
// =============================================================================

void SpectatorNode_Init() {
    g_state = State{};
    g_state.capacity = SPECTATOR_DEFAULT_CAPACITY;

    // FM2K_SPEC_TRANSPORT (Phase 2b of v0.3 spec rebuild). "relay" =
    // route spec data through hub WS binary frames instead of P2P TCP.
    // When set, we skip the entire TCP-listener + TCP-STUN dance below
    // -- no listener to bind, no external port to discover, no NAT
    // punch dance to coordinate. Spec data plane comes online once the
    // launcher's shared-mem queue is wired (Phase 2c).
    //
    // Default "tcp" preserves legacy behavior for every client up
    // through v0.2.57. Read once here at Init -- env changes mid-run
    // wouldn't be safe (existing subscribers expect one mode).
    if (const char* transport = std::getenv("FM2K_SPEC_TRANSPORT");
        transport && std::strcmp(transport, "relay") == 0) {
        g_state.spec_transport_relay = true;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "SpectatorNode: FM2K_SPEC_TRANSPORT=relay -- skipping TCP "
            "listener + TCP-STUN (relay data plane not yet wired; spec "
            "subscriptions will negotiate but not stream until Phase 2c)");
        // Capacity still applies (hub fan-out is bounded too). Match
        // the TCP path's default so behavior is identical from the
        // host-control-flow perspective.
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "SpectatorNode: Init (capacity=%zu, batch=%zu frames, "
            "transport=relay)",
            g_state.capacity, BROADCAST_BATCH_FRAMES);
        return;
    }

    // Bring up the TCP listener for the host→spectator INPUT_BATCH stream.
    //
    // Bind strategy is tiered because Windows reserves chunks of the
    // dynamic port range for Hyper-V / WSL2 / docker / etc., and the
    // reserved chunks shift across reboots. Three attempts in order:
    //
    //   1. UDP_port + 100  (deterministic, gives predictable port for
    //      debugging when it succeeds)
    //   2. UDP_port + 1000 (different bucket; usually clear of the
    //      WSL-reserved range)
    //   3. port = 0        (OS picks any free port; always succeeds)
    //
    // JOIN_ACK includes our actual listener port via
    // SpectatorTCP::GetListenPort(), so spectators learn whichever port
    // we ended up on with no client-side coordination. The deterministic
    // attempts are first only to keep logs readable; the OS-picked
    // fallback is what guarantees we never come up listener-less.
    //
    // Real-world repro on pkmncc 2026-05-13: P1 udp=51376 → +100=51476
    // failed with WSAEACCES (Windows-reserved range), needed +1000 OR
    // port=0 to bind successfully.
    //
    // Idempotent — re-Init won't double-bind.
    const uint16_t udp_port = NetSocket_GetLocalPort();
    const uint16_t candidate_ports[] = {
        (uint16_t)(udp_port + 100),
        (uint16_t)(udp_port + 1000),
        0,  // OS picks
    };
    bool spec_listener_bound = false;
    for (uint16_t cand : candidate_ports) {
        if (SpectatorTCP::StartListener(cand)) {
            spec_listener_bound = true;
            if (cand == 0) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: TCP listener bound via OS-picked port "
                    "(actual port %u — preferred offsets all hit "
                    "WSAEACCES, usually Windows reserved-port range)",
                    (unsigned)SpectatorTCP::GetListenPort());
            }
            break;
        }
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "SpectatorNode: TCP listener bind on port %u failed — "
            "trying next candidate", (unsigned)cand);
    }
    if (!spec_listener_bound) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "SpectatorNode: Init — TCP listener failed to bind on ANY "
            "candidate port (udp=%u, +100, +1000, OS-picked all failed); "
            "spectator subscriptions will fail",
            (unsigned)udp_port);
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "SpectatorNode: Init (capacity=%zu, batch=%zu frames, "
                "tcp_port=%u)",
                g_state.capacity, BROADCAST_BATCH_FRAMES,
                (unsigned)SpectatorTCP::GetListenPort());

    // TCP-STUN — discover external TCP addr by source-binding an outbound
    // connect to (hub:tcp_stun) from our listener port. Only meaningful
    // for spectators (where the cross-NAT punch path needs the *external*
    // tcp_port for the host-side punch to fire at the right port). Hosts
    // run this too for symmetry — costs ~50ms once and pre-populates
    // the field if the host ever later acts as a spectator (daisy chain).
    // Failure here is logged but non-fatal; punching falls back to local
    // listener port which works on port-preserving NATs.
    SpectatorTCP::PerformTcpStun();
}

void SpectatorNode_Shutdown() {
    // C7: write full session log on shutdown if there's anything to flush.
    // Skipped on the spectator side where session_events is the relay log
    // (correct to write — the relay's local view IS the canonical session
    // for any sub-spectators, even if we'd be one of two writers when
    // host + spectator both run on this machine; per-process file paths
    // already include timestamp + pid disambiguation).
    if (!g_state.session_events.empty()) {
        char ts[64] = {};
        std::time_t now = std::time(nullptr);
        std::tm tm_buf{};
        localtime_s(&tm_buf, &now);
        std::strftime(ts, sizeof(ts), "sessions/%Y-%m-%d_%H%M%S.fm2kset", &tm_buf);
        CreateDirectoryA("sessions", nullptr);
        SpectatorNode_WriteSessionFile(ts);
    }

    // Best-effort: notify all subscribers before tearing down so they
    // can fail over to root immediately instead of waiting out their
    // silence timer.
    for (const auto& sub : g_state.subscribers) {
        CtrlPacket leave = {};
        leave.header.type = CtrlMsg::SPEC_LEAVE;
        ControlChannel_SendTo(leave, sub.addr);
    }
    // And tell our upstream we're going away (frees their subscriber slot).
    if (g_state.subscribed_upstream) {
        CtrlPacket leave = {};
        leave.header.type = CtrlMsg::SPEC_LEAVE;
        ControlChannel_SendTo(leave, g_state.upstream_addr);
    }

    g_state.subscribers.clear();
    g_state.session_events.clear();
    g_state.session_events.shrink_to_fit();
    g_state.match_headers.clear();
    g_state.match_headers.shrink_to_fit();
    g_state.last_flushed_event_idx = 0;
    g_state.flushed_input_count    = 0;
    g_state.total_input_count      = 0;
    g_state.broadcasting = false;
    g_state.subscribed_upstream = false;
    g_state.playing_back = false;
    SpectatorTCP::Shutdown();
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "SpectatorNode: Shutdown");
}

// Clear the GekkoSpectator addr-tracking set. Called from netplay.cpp
// after each fresh gekko_create + gekko_start so the next session
// starts with no "already added" entries. Without this, post-session
// spec rejoins would be skipped because their addr is "remembered"
// from the previous (now-destroyed) session.
void SpectatorNode_ClearGekkoSpectatorTracking() {
    if (!g_gekko_spectator_addrs.empty()) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "SpectatorNode: cleared %zu GekkoSpectator addr tracking entries (session boundary)",
            g_gekko_spectator_addrs.size());
        g_gekko_spectator_addrs.clear();
    }
}

void SpectatorNode_SetCapacity(size_t max_direct) {
    g_state.capacity = max_direct;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "SpectatorNode: Capacity set to %zu", max_direct);
}

// -----------------------------------------------------------------------------
// HOST-SIDE
// -----------------------------------------------------------------------------

void SpectatorNode_OnMatchStart(
    uint32_t game_hash,
    uint32_t initial_rng_seed,
    uint32_t initial_state_hash,
    uint8_t p1_char, uint8_t p1_color,
    uint8_t p2_char, uint8_t p2_color,
    uint8_t stage_id)
{
    g_state.broadcasting = true;
    // Flush any unbatched CSS events before the match-start MATCH_START
    // event hits the wire — keeps the per-INPUT-frame numbering monotonic
    // across the CSS→battle seam. Without this, trailing CSS frames sit
    // unbatched in session_events past last_flushed_event_idx; their
    // session-relative INPUT-frame indices are below the next live battle
    // batch's start_frame, but the spectator's next_expected_frame would
    // already be at the higher index → indefinite gap.
    FlushBatch();

    // Stash the initial-match metadata as a 96-byte payload that's the
    // canonical MATCH_START event body (layout pinned by Replay::ReplayHeader
    // in replay.h — kept stable so the wire schema doesn't churn).
    uint8_t* h = g_state.initial_match.header_bytes;
    std::memset(h, 0, 96);
    uint32_t magic   = 0x52504D46;  // Replay::REPLAY_MAGIC
    uint16_t version = 1;
    std::memcpy(h + 0,  &magic,              4);
    std::memcpy(h + 4,  &version,            2);
    std::memcpy(h + 16, &game_hash,          4);  // game_hash (after timestamp)
    std::memcpy(h + 20, &initial_rng_seed,   4);
    std::memcpy(h + 24, &initial_state_hash, 4);
    h[28] = p1_char;
    h[29] = p1_color;
    h[30] = p2_char;
    h[31] = p2_color;
    // p1_name / p2_name at h+32 / h+56 left zeroed; filled once UI plumbs them.
    h[80] = stage_id;
    // frame_count at h+92 stays 0 — subscribers get INPUT_BATCH frames live.
    g_state.initial_match.valid = true;

    // C6: append MATCH_START as a SessionEvent op so the metadata flows in
    // the same ordered stream as INPUTs. Spectator's drain applies the op
    // at exactly the logical frame the host set the match up. Late joiners
    // get this op as part of SendSessionBackfillTo. The legacy INITIAL_MATCH
    // packet path (still sent below) is kept for back-compat; once all
    // peers run C6+ builds we can retire it.
    SpectatorNode_AppendMatchStart(g_state.initial_match.header_bytes);

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "SpectatorNode: Match start broadcast (seed=0x%08X, subs=%zu)",
                initial_rng_seed, g_state.subscribers.size());
}

void SpectatorNode_OnFrameConfirmed(uint16_t p1_input, uint16_t p2_input) {
    // Append an INPUT event to the session log so a late joiner can backfill
    // every confirmed frame from session start, including CSS frames that
    // happened before any spectator subscribed. 5 B/event in memory.
    SessionEvent ev{};
    ev.type = SessionEventType::INPUT;
    ev.u.input.p1 = p1_input;
    ev.u.input.p2 = p2_input;
    g_state.session_events.push_back(ev);
    ++g_state.total_input_count;

    // Live broadcast batching window — only fan out to existing subscribers.
    // Cadence trigger: every BROADCAST_BATCH_FRAMES new INPUT events.
    const uint32_t pending_inputs =
        g_state.total_input_count - g_state.flushed_input_count;
    if (pending_inputs >= BROADCAST_BATCH_FRAMES) {
        FlushBatch();
    }
}

void SpectatorNode_OnMatchEnd(const MatchEndPayload& p) {
    if (!g_state.broadcasting) return;
    // Flush whatever's left in the pending event window so viewers see the
    // final frames before MATCH_END.
    FlushBatch();
    // MATCH_END flows in-band as a SessionEvent op; the apply-at-head drain
    // on the receiver flips playing_back=false at the same logical frame
    // the host appended. (Legacy MATCH_END packet was retired in C12.)
    SpectatorNode_AppendMatchEnd(p);
    g_state.broadcasting = false;
    g_state.initial_match.valid = false;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "SpectatorNode: Match end broadcast (winner=%u rounds=%u-%u frames=%u)",
        p.winner_idx, p.rounds_won_p1, p.rounds_won_p2, p.frames_total);
}

// -----------------------------------------------------------------------------
// HOST-SIDE OP APPENDERS (C3)
// -----------------------------------------------------------------------------
//
// Append-and-flush helpers. Called by host pin sites in netplay.cpp /
// savestate.cpp immediately after the local memory write. The append+flush
// pair guarantees the op reaches subscribed spectators before the next
// INPUT event in the stream — drain-at-head semantics on the receiver
// then apply the op exactly when the spectator's local sim is about to
// consume the same logical frame the host did.

namespace {

void AppendOpAndFlush(const SessionEvent& ev) {
    g_state.session_events.push_back(ev);
    // Flush eagerly when subscribers exist (host with live spectators OR
    // relay node with sub-spectators). When the subscriber list is empty,
    // there's nothing to send; late joiners get the full backlog via
    // SendSessionBackfillTo. Note: we don't gate on `broadcasting` —
    // that flag is host-side match state and doesn't apply to the relay
    // path where a spectator's HandleSpecData re-Appends incoming ops
    // to its own session_events for sub-spectator forwarding.
    if (!g_state.subscribers.empty()) {
        FlushBatch();
    }
}

} // namespace

void SpectatorNode_AppendPinRng(uint32_t seed) {
    SessionEvent ev{};
    ev.type            = SessionEventType::PIN_RNG;
    ev.u.pin_rng_seed  = seed;
    AppendOpAndFlush(ev);
}

void SpectatorNode_AppendResetInputState() {
    SessionEvent ev{};
    ev.type = SessionEventType::RESET_INPUT_STATE;
    AppendOpAndFlush(ev);
}

void SpectatorNode_AppendSoundInit() {
    SessionEvent ev{};
    ev.type = SessionEventType::SOUND_INIT;
    AppendOpAndFlush(ev);
}

void SpectatorNode_AppendFingerprint(uint32_t hash) {
    SessionEvent ev{};
    ev.type                  = SessionEventType::FINGERPRINT;
    ev.u.fingerprint_hash    = hash;
    AppendOpAndFlush(ev);
}

// C3.5 — round events. Snapshot input-frame at ROUND_START so AppendRoundEnd
// can compute frames_elapsed without the hook needing access to the private
// total_input_count counter.
static uint32_t s_round_start_input_frame = 0;

// Most-recent rounds_won values seen at AppendRoundEnd time. Cached
// because Netplay_EndBattle's read of FM2K::ADDR_P1/P2_ROUNDS_WON fires
// AFTER vs_round_function's match-over branch creates the type=10
// match-end object, whose update sometimes resets the live counters
// before the read. ROUND_END's read is reliably accurate (verified
// empirically), so AppendMatchEnd overrides the (potentially stale)
// values Netplay_EndBattle passed in with these.
static uint8_t s_last_seen_rounds_won_p1 = 0;
static uint8_t s_last_seen_rounds_won_p2 = 0;

// C10 — 1-based per-session match counter. Bumped at every
// AppendMatchStart. Reset to 0 in SpectatorNode_AppendSessionId so a
// new session restarts numbering at 1 for its first match.
static uint8_t s_match_index_in_session = 0;

void SpectatorNode_AppendRoundStart(uint8_t  round_idx,
                                    uint16_t p1_hp_max,
                                    uint16_t p2_hp_max,
                                    uint16_t timer_seconds) {
    s_round_start_input_frame = g_state.total_input_count;
    // New round starting — clear stale rounds_won cache from a possibly
    // earlier match. AppendRoundEnd repopulates it as rounds tick by.
    if (round_idx == 1) {
        s_last_seen_rounds_won_p1 = 0;
        s_last_seen_rounds_won_p2 = 0;
    }
    SessionEvent ev{};
    ev.type = SessionEventType::ROUND_START;
    ev.u.round_start.round_idx     = round_idx;
    ev.u.round_start.p1_hp_max     = p1_hp_max;
    ev.u.round_start.p2_hp_max     = p2_hp_max;
    ev.u.round_start.timer_seconds = timer_seconds;
    AppendOpAndFlush(ev);
}

void SpectatorNode_AppendRoundEnd(uint8_t  winner_idx,
                                  uint16_t p1_hp_remaining,
                                  uint16_t p2_hp_remaining) {
    const uint32_t frames =
        (g_state.total_input_count >= s_round_start_input_frame)
            ? (g_state.total_input_count - s_round_start_input_frame)
            : 0;
    SessionEvent ev{};
    ev.type = SessionEventType::ROUND_END;
    ev.u.round_end.winner_idx       = winner_idx;
    ev.u.round_end.p1_hp_remaining  = p1_hp_remaining;
    ev.u.round_end.p2_hp_remaining  = p2_hp_remaining;
    ev.u.round_end.frames_elapsed   = frames;
    AppendOpAndFlush(ev);

    // Cache live rounds_won AT THIS MOMENT — accurate snapshot for
    // AppendMatchEnd to use later. The match-over path resets these
    // counters before Netplay_EndBattle's read fires.
    s_last_seen_rounds_won_p1 = (uint8_t)*(uint32_t*)0x4DFC6D;
    s_last_seen_rounds_won_p2 = (uint8_t)*(uint32_t*)0x4EDCAC;

    // C10 — also push this round's result into SharedMem so the launcher
    // can include it in the hub match_result JSON's "rounds[]" array.
    SharedMem_PublishRoundResult(winner_idx, p1_hp_remaining,
                                 p2_hp_remaining, frames);
}

// =============================================================================
// FINGERPRINT (C9) — diagnostic state hash for desync detection
// =============================================================================
//
// Both host and spectator sample the same set of FM2K state fields, hash
// them with classic Fletcher-32, and the host appends the result as a
// FINGERPRINT op every 30 sim frames. Spectator's ApplySessionEvent
// computes its own hash on its current state at the same logical frame
// (drain-at-head ordering ensures it's the same frame the host hashed)
// and logs WARN on mismatch, including both values. Replaces the manual
// [HOST-FP] / [SPEC-FP] log-grep diagnostic once enabled.
//
// Gated on FM2K_SPEC_FINGERPRINT=1 — off by default so the wire stays
// quiet for normal play.

bool SpectatorFingerprint_Enabled() {
    static int s_state = -1;  // 0=off, 1=on
    if (s_state < 0) {
        const char* v = std::getenv("FM2K_SPEC_FINGERPRINT");
        s_state = (v && v[0] == '1' && v[1] == '\0') ? 1 : 0;
    }
    return s_state != 0;
}

uint32_t SpectatorFingerprint_Compute() {
    // Same fields the [HOST-FP]/[SPEC-FP] logs already pin. If we ever add
    // fields, both sides update together — divergent samples would yield
    // a hash mismatch that the spectator catches at runtime.
    constexpr uintptr_t POOL = 0x4701E0;
    constexpr size_t    SLOT = 382;
    struct Sample {
        uint32_t rng;
        uint32_t buf_idx;
        uint32_t p1_hp, p2_hp;
        uint32_t timer;
        int32_t  p1_x, p1_y, p2_x, p2_y;
        int32_t  p1_script, p2_script;
    } s;
    s.rng       = *(uint32_t*)0x41FB1C;
    s.buf_idx   = *(uint32_t*)0x447EE0;
    s.p1_hp     = *(uint32_t*)0x4DFC85;
    s.p2_hp     = *(uint32_t*)0x4EDCC4;
    s.timer     = *(uint32_t*)0x470044;
    s.p1_x      = *(int32_t*)(POOL + 0 * SLOT + 0x08);
    s.p1_y      = *(int32_t*)(POOL + 0 * SLOT + 0x0C);
    s.p2_x      = *(int32_t*)(POOL + 1 * SLOT + 0x08);
    s.p2_y      = *(int32_t*)(POOL + 1 * SLOT + 0x0C);
    s.p1_script = *(int32_t*)(POOL + 0 * SLOT + 0x30);
    s.p2_script = *(int32_t*)(POOL + 1 * SLOT + 0x30);

    return Fletcher32(reinterpret_cast<const uint8_t*>(&s), sizeof(s));
}

// Snapshot at MATCH_START for the C7 frames_total computation in
// AppendMatchEnd. Reset on every MATCH_START so back-to-back matches
// each get an accurate per-match input-frame delta.
static uint32_t s_match_start_input_frame = 0;

void SpectatorNode_AppendMatchStart(const uint8_t header[96]) {
    // Stash the 96-byte header in the side table and reference it by index
    // from the SessionEvent (keeps the in-memory event size at 5 B).
    MatchHeader hdr_copy;
    std::memcpy(hdr_copy.data(), header, hdr_copy.size());
    g_state.match_headers.push_back(hdr_copy);

    s_match_start_input_frame = g_state.total_input_count;

    SessionEvent ev{};
    ev.type = SessionEventType::MATCH_START;
    ev.u.match_start_idx =
        static_cast<uint16_t>(g_state.match_headers.size() - 1);
    const size_t match_start_idx = g_state.session_events.size();
    g_state.last_match_start_idx = static_cast<int64_t>(match_start_idx);

    // Backward-scan through PIN_RNG / RESET_INPUT_STATE / SOUND_INIT /
    // SESSION_ID events that precede this MATCH_START, so the per-battle
    // .fm2krep slice can include the full state-init prefix and play
    // back without depending on prior state. Stops at the first
    // non-state-init event (typically the last CSS-phase INPUT, but
    // could also be the prior match's MATCH_END / final ROUND_END).
    auto is_pre_match_init = [](SessionEventType t) {
        return t == SessionEventType::PIN_RNG
            || t == SessionEventType::RESET_INPUT_STATE
            || t == SessionEventType::SOUND_INIT
            || t == SessionEventType::SESSION_ID;
    };
    size_t pre_init_idx = match_start_idx;
    while (pre_init_idx > 0 &&
           is_pre_match_init(g_state.session_events[pre_init_idx - 1].type)) {
        --pre_init_idx;
    }
    g_state.last_pre_match_init_idx = static_cast<int64_t>(pre_init_idx);

    // C10 — bump the per-session match index and publish to SharedMem
    // so the launcher can include {session_id, match_index_in_session}
    // in its match_result JSON to the hub.
    if (s_match_index_in_session < 255) ++s_match_index_in_session;
    SharedMem_PublishMatchSession(g_state.session_id, s_match_index_in_session);

    AppendOpAndFlush(ev);
}

void SpectatorNode_AppendMatchEnd(const MatchEndPayload& p) {
    SessionEvent ev{};
    ev.type        = SessionEventType::MATCH_END;
    ev.u.match_end = p;
    // Override caller's rounds_won with the cached values from the most
    // recent ROUND_END. Netplay_EndBattle reads from the live FM2K
    // counters but those get reset by the match-over object's update
    // before the read. Take the max of (cache, passed) — the cache is
    // reliable, but if for any reason the cache is stale (no
    // AppendRoundEnd fired yet) we fall back to whatever Netplay
    // passed.
    if (s_last_seen_rounds_won_p1 > p.rounds_won_p1) {
        ev.u.match_end.rounds_won_p1 = s_last_seen_rounds_won_p1;
    }
    if (s_last_seen_rounds_won_p2 > p.rounds_won_p2) {
        ev.u.match_end.rounds_won_p2 = s_last_seen_rounds_won_p2;
    }
    // Caller passes frames_total=0; we compute the actual value here so
    // hook code (Netplay_EndBattle) doesn't need access to the private
    // total_input_count counter.
    ev.u.match_end.frames_total =
        (g_state.total_input_count >= s_match_start_input_frame)
            ? (g_state.total_input_count - s_match_start_input_frame)
            : 0;
    AppendOpAndFlush(ev);
}

void SpectatorNode_AppendSessionId(uint64_t session_id) {
    g_state.session_id = session_id;
    // C10 — new session, restart match numbering. The first
    // AppendMatchStart for this session bumps to 1.
    s_match_index_in_session = 0;
    SessionEvent ev{};
    ev.type          = SessionEventType::SESSION_ID;
    ev.u.session_id  = session_id;
    AppendOpAndFlush(ev);
}

uint64_t SpectatorNode_GetSessionId() {
    return g_state.session_id;
}

// =============================================================================
// SNAPSHOT CACHE (task #18 phase 2)
// =============================================================================
//
// Capture a fresh SaveState blob at battle entry so a CURRENT_MATCH-mode
// spectator joining mid-set can SaveState_Load directly to the current
// match's start instead of replaying every previous battle's events.
//
// Why call SaveState_Save(0) ourselves: the GekkoNet driver normally
// triggers Save in its first AdvanceEvent (a few sim frames after
// Netplay_StartBattle), but we want the snapshot CAPTURED at the same
// logical instant the host emitted MATCH_START — before any battle
// frame has run, so the spectator's state on Load is "match just
// starting, frame 0 input pending." That keeps the wire-anchor clean:
// snapshot.input_frame == g_state.total_input_count, and the very next
// INPUT event the host appends becomes the spectator's first popped
// frame after Load.

void SpectatorNode_StashSnapshot() {
    // Skip during a rollback rewind — the FM2K state at this moment isn't
    // the canonical battle-start state we want to capture. In practice
    // StashSnapshot is called from Netplay_StartBattle which is the seam
    // frame between CSS and battle (no preceding battle inputs to roll
    // back), so g_is_rolling_back should never be true here. Belt-and-
    // suspenders: if a future caller invokes us mid-battle, this gate
    // prevents handing spectators a partially-rewound state. Match cache
    // stays as the previous match's (or empty if first match).
    if (g_is_rolling_back) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "SpectatorNode: StashSnapshot skipped — called during rollback "
            "rewind (snapshot cache stays %s)",
            g_state.current_snapshot.valid ? "previous match's" : "empty");
        return;
    }

    // Force a Save to populate the rollback buffer's slot 0 with the
    // current FM2K state. Sets g_initial_sync_done in savestate.cpp;
    // GekkoNet's later first-Save is a no-op for the initial-sync reset.
    if (!SaveState_Save(0)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "SpectatorNode: StashSnapshot — SaveState_Save(0) failed; "
            "snapshot cache stays %s",
            g_state.current_snapshot.valid ? "the previous match's" : "empty");
        return;
    }

    const uint8_t* slot_bytes = SaveState_PeekLastSavedSlotBytes();
    const size_t   slot_size  = SaveState_GetSlotByteSize();
    if (!slot_bytes || slot_size == 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "SpectatorNode: StashSnapshot — slot bytes unavailable post-Save");
        return;
    }

    auto& cache = g_state.current_snapshot;
    cache.blob.assign(slot_bytes, slot_bytes + slot_size);
    cache.input_frame = g_state.total_input_count;
    // match_index = 0-based count of MATCH_STARTs emitted so far.
    // AppendMatchStart pushes to match_headers BEFORE StashSnapshot
    // is called (Netplay_StartBattle calls OnMatchStart → AppendMatchStart
    // → THEN StashSnapshot), so size already reflects this match.
    cache.match_index = g_state.match_headers.empty() ? 0u
                        : (uint32_t)(g_state.match_headers.size() - 1);
    cache.checksum    = Fletcher32(cache.blob.data(), cache.blob.size());
    // Phase E: record game_mode at capture time so the spec-side apply
    // can wait for a matching mode. CSS captures get applied during the
    // spec's CSS; battle captures wait for battle.
    cache.captured_game_mode = *(const uint32_t*)FM2K::ADDR_GAME_MODE;
    cache.valid       = true;

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "SpectatorNode: snapshot cached (match=%u, %zu bytes, "
        "input_frame=%u, fletcher32=0x%08X, captured_game_mode=%u)",
        cache.match_index, cache.blob.size(),
        cache.input_frame, cache.checksum, cache.captured_game_mode);
}

bool SpectatorNode_HasSnapshot() {
    return g_state.current_snapshot.valid;
}

void SpectatorNode_ApplyPendingPinRng() {
    if (!g_state.pending_pin_rng_valid) return;
    *(uint32_t*)0x41FB1C = g_state.pending_pin_rng_seed;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "SpectatorNode: applied deferred PIN_RNG=0x%08X at battle entry",
        g_state.pending_pin_rng_seed);
    g_state.pending_pin_rng_valid = false;
}

void SpectatorNode_ApplyPendingSnapshot() {
    auto& inbox = g_state.pb_snapshot_inbox;
    if (!inbox.pending_apply) return;

    // Wait until the spectator's local engine has reached the SAME
    // phase the snapshot was captured at. The savestate captures the
    // engine state — object pool, character data, DDraw surfaces, etc.
    // — and applying it before the local engine has done its own
    // init-for-that-phase lands the captured bytes into structurally-
    // incompatible memory:
    //   - DDraw/D3D9 surfaces sized for the wrong phase layout
    //   - Audio sample handles for the wrong phase's audio set
    //   - Char-data block has content the local engine hasn't loaded
    // Symptom: spectator crashes on the next render frame after apply.
    //
    // By waiting for game_mode >= captured_game_mode the local engine
    // has already performed its own init for the captured phase; the
    // apply just overlays dynamic state on top.
    //
    // Phase E (v0.2.42+): host writes captured_game_mode into the
    // SnapshotMetadata so the spec knows whether to wait for CSS (2000)
    // or battle (3000). Pre-Phase-E hosts (v0.2.41) left this field 0
    // and always captured at battle entry; the captured==0 fallback
    // keeps that wire compat with the v0.2.41 default of
    // `game_mode >= 3000`.
    const uint32_t game_mode = *(uint32_t*)FM2K::ADDR_GAME_MODE;
    const uint32_t captured  = inbox.meta.captured_game_mode;
    const uint32_t apply_gate = (captured == 0u) ? 3000u : captured;
    if (game_mode < apply_gate) return;

    if (!SaveState_LoadFromBytes(inbox.blob.data(), inbox.blob.size())) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "SpectatorNode: SaveState_LoadFromBytes failed at deferred "
            "apply (match=%u, %zu bytes, mode=%u) — discarding snapshot",
            inbox.meta.match_index, inbox.blob.size(), game_mode);
        inbox = State::SnapshotInbox{};
        return;
    }

    const uint32_t anchor = inbox.anchor_frame;

    // Anchor the EVENT_BATCH stream cursor at the snapshot's INPUT-frame
    // position. Subsequent batches start at this frame index — see
    // SendSessionBackfillFromFrame on the host side.
    //
    // 2026-05-17 fix: don't reflexively clear pb_queue + reset
    // next_expected_frame. The CURRENT_MATCH bind path DOES pre-send
    // anchor-onward events (SendSessionBackfillFromFrame fires right
    // after SendSnapshotTo on the host), and live EVENT_BATCH broadcasts
    // continue flowing through the deferred-apply window. Those events
    // are EXACTLY what we want: pb_queue holds anchor..live, ready for
    // sim catch-up the moment snapshot applies. Clearing them wiped
    // ~750 events of valid backfill+live → spec stuck at expected=anchor
    // with all subsequent live batches "out-of-order" (host at frame
    // 1100+, spec expecting 333).
    //
    // Only reset state when our cursor is BEHIND the anchor — that's the
    // FULL_SESSION → CURRENT_MATCH renegotiation case where pb_queue
    // holds stale pre-anchor frames the snapshot supersedes.
    if (!g_state.have_frame_baseline ||
        g_state.next_expected_frame < anchor)
    {
        g_state.pb_queue.clear();
        g_state.pb_match_headers.clear();
        g_state.next_expected_frame = anchor;
    }
    g_state.have_frame_baseline    = true;
    g_state.highest_consumed_frame = 0;
    g_state.playing_back           = true;

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "SpectatorNode: SNAPSHOT applied (match=%u, %zu bytes) — "
        "anchor INPUT-frame=%u, local game_mode=%u",
        inbox.meta.match_index, inbox.blob.size(),
        anchor, game_mode);

    inbox = State::SnapshotInbox{};
}

SpectatorSnapshotInfo SpectatorNode_GetSnapshotInfo() {
    SpectatorSnapshotInfo out = {};
    if (!g_state.current_snapshot.valid) return out;
    out.input_frame = g_state.current_snapshot.input_frame;
    out.match_index = g_state.current_snapshot.match_index;
    out.total_bytes = (uint32_t)g_state.current_snapshot.blob.size();
    out.checksum    = g_state.current_snapshot.checksum;
    return out;
}

// =============================================================================
// SESSION FILE FORMAT (C7) — .fm2kset / .fm2krep
// =============================================================================
//
// On-disk layout: [SessionFileHeader (32 B)] [packed SessionEvent[] bytes]
//
// Same wire encoding as EVENT_BATCH payload (SessionEvent_Encode*), so
// loaders can reuse SessionEvent_Decode without an intermediate format.
// Events are self-describing (1-byte tag + variant payload), so no
// separate side table is needed — MATCH_START's 96-byte ReplayHeader is
// inline in the encoded byte stream.

namespace {

constexpr uint32_t SESSION_FILE_MAGIC   = 0x53534D46;  // 'FMSS' little-endian
constexpr uint16_t SESSION_FILE_VERSION = 2;           // 256 B header (C7)

#pragma pack(push, 1)
// C7 — 256 B enriched header.
//
// Carries everything the launcher's replay-browser tree needs without
// rescanning the body bytes: nicks, character/color, winner + per-side
// rounds_won, session grouping (session_id + match_index), and a
// round_offsets[] table of byte positions so round-level seek is a
// single fread+fseek instead of a body walk.
//
// Body layout unchanged: same packed SessionEvent[] bytes after the header.
struct FM2KSessionFileHeader {
    // wire envelope (preserves first 8 bytes from v1 layout)
    uint32_t magic;              // 'FMSS' (0x53534D46)
    uint16_t version;            // = 2
    uint16_t flags;              // bit 0: 1=battle slice (.fm2krep), 0=full session (.fm2kset)
                                 // bit 1: 1=round_offsets[] populated

    // descriptive
    uint64_t started_at_unix;
    uint64_t finished_at_unix;
    uint32_t event_count;
    uint32_t input_count;
    char     game_id[32];        // e.g. "pkmncc"
    char     p1_nick[32];
    char     p2_nick[32];

    // character / outcome
    uint8_t  p1_char_id;
    uint8_t  p2_char_id;
    uint8_t  p1_color;
    uint8_t  p2_color;
    uint8_t  rounds_won_p1;      // from latest MATCH_END payload
    uint8_t  rounds_won_p2;
    uint8_t  match_count;        // .fm2kset: total in session; .fm2krep: 1
    uint8_t  match_index;        // .fm2krep: 1-based; .fm2kset: 0
    uint64_t session_id;         // shared across .fm2krep slices of one .fm2kset

    // seek anchors — body-relative byte offsets pointing at ROUND_START
    // tag bytes. Unused slots = 0. Capped at 8 (best-of-15 doesn't exist).
    uint8_t  round_count;        // 0..8
    uint8_t  reserved0[3];
    uint32_t round_offsets[8];

    // future-proofing — all zeros for v2 readers
    uint8_t  reserved[76];
};
static_assert(sizeof(FM2KSessionFileHeader) == 256,
              "FM2KSessionFileHeader must be 256 bytes");
#pragma pack(pop)

// Encode events[first..last) into a vector<uint8_t> of packed wire bytes.
// MATCH_START events look up their 96-byte header from the host's
// match_headers side table. While encoding, record the body-relative byte
// offset of each ROUND_START tag we emit so the C7 header can populate
// round_offsets[].
void EncodeEventSliceToBytes(const std::vector<SessionEvent>& events,
                             const std::vector<MatchHeader>& headers,
                             size_t first, size_t last,
                             std::vector<uint8_t>& out_bytes,
                             uint32_t& out_input_count,
                             std::vector<uint32_t>& out_round_offsets) {
    out_input_count = 0;
    out_round_offsets.clear();
    out_bytes.reserve((last - first) * SESSION_EVENT_MAX_WIRE_SIZE);
    for (size_t i = first; i < last; i++) {
        const SessionEvent& ev = events[i];
        const size_t off_pre = out_bytes.size();
        AppendEventToWire(out_bytes, ev, headers);
        if (ev.type == SessionEventType::INPUT) ++out_input_count;
        if (ev.type == SessionEventType::ROUND_START) {
            out_round_offsets.push_back(static_cast<uint32_t>(off_pre));
        }
    }
}

// Locate the most-recent MATCH_START event preceding the given index in the
// slice and return its 96-byte header bytes (empty if none in slice). Used
// at write time to populate the C7 header's char IDs.
bool ResolveMatchHeader(const std::vector<SessionEvent>& events,
                        const std::vector<MatchHeader>& headers,
                        size_t first, size_t last,
                        MatchHeader& out_hdr) {
    for (size_t i = last; i-- > first; ) {
        const SessionEvent& ev = events[i];
        if (ev.type == SessionEventType::MATCH_START &&
            ev.u.match_start_idx < headers.size()) {
            out_hdr = headers[ev.u.match_start_idx];
            return true;
        }
    }
    return false;
}

// Find the most-recent MATCH_END payload in the slice (used to populate the
// header's winner / per-side rounds_won fields for .fm2krep). For .fm2kset
// (full session) this is the LAST match's MATCH_END which is the right
// summary for the session.
bool ResolveLatestMatchEnd(const std::vector<SessionEvent>& events,
                           size_t first, size_t last,
                           MatchEndPayload& out) {
    for (size_t i = last; i-- > first; ) {
        if (events[i].type == SessionEventType::MATCH_END) {
            out = events[i].u.match_end;
            return true;
        }
    }
    return false;
}

uint8_t CountMatchesInSlice(const std::vector<SessionEvent>& events,
                            size_t first, size_t last) {
    size_t n = 0;
    for (size_t i = first; i < last; i++) {
        if (events[i].type == SessionEventType::MATCH_START) ++n;
    }
    return n > 255 ? (uint8_t)255 : (uint8_t)n;
}

bool WriteSessionFileImpl(const char* path,
                          const std::vector<SessionEvent>& events,
                          const std::vector<MatchHeader>& headers,
                          size_t first, size_t last,
                          bool is_battle_slice) {
    if (first >= last) return false;
    if (last > events.size()) return false;

    std::vector<uint8_t> body;
    uint32_t input_count = 0;
    std::vector<uint32_t> round_offsets;
    EncodeEventSliceToBytes(events, headers, first, last,
                            body, input_count, round_offsets);
    if (body.empty()) return false;

    FM2KSessionFileHeader hdr = {};
    hdr.magic            = SESSION_FILE_MAGIC;
    hdr.version          = SESSION_FILE_VERSION;
    hdr.flags            = is_battle_slice ? 1u : 0u;
    if (!round_offsets.empty()) hdr.flags |= (1u << 1);

    const uint64_t now_unix = static_cast<uint64_t>(std::time(nullptr));
    hdr.started_at_unix  = now_unix;   // best-effort: use write time as
    hdr.finished_at_unix = now_unix;   //   both anchors when .fm2kset is
                                       //   written at session end.
                                       //   (Full session walks already
                                       //   emit at shutdown — close enough
                                       //   for chronological sort.)
    hdr.event_count      = static_cast<uint32_t>(last - first);
    hdr.input_count      = input_count;

    // Char/color from the most-recent MATCH_START header in this slice.
    MatchHeader mh;
    if (ResolveMatchHeader(events, headers, first, last, mh)) {
        hdr.p1_char_id = mh[28];
        hdr.p1_color   = mh[29];
        hdr.p2_char_id = mh[30];
        hdr.p2_color   = mh[31];
    }

    // Latest MATCH_END payload populates winner + per-side rounds.
    MatchEndPayload me{};
    if (ResolveLatestMatchEnd(events, first, last, me)) {
        hdr.rounds_won_p1 = me.rounds_won_p1;
        hdr.rounds_won_p2 = me.rounds_won_p2;
    }

    hdr.match_count  = is_battle_slice ? 1
                                       : CountMatchesInSlice(events, first, last);
    hdr.match_index  = is_battle_slice ? 1 : 0;
    hdr.session_id   = g_state.session_id;

    // Populate p1_nick / p2_nick / game_id from SharedMem + exe path.
    // Previously these were left at the memset-zeroed default which made
    // the launcher's replay browser show every match as "?" vs "?" and
    // the stats/hub couldn't distinguish matches by participant. The
    // launcher writes ui_my_nick + ui_peer_nick to SharedMem at HELLO
    // exchange time; we read them here and assign to p1/p2 based on
    // which side we are (host = player_index 0 = p1).
    // Only host (0) and joiner (1) participate in the match — spec
    // (player_index = 2 sentinel) has its own nick but neither matches
    // P1 nor P2. For spec-written .fm2kset / .fm2krep we leave nicks
    // zero rather than write spec's-own-nick as one of the participants.
    // TODO: relay P1/P2 nicks via a SESSION_NICKS or MATCH_START extension
    // so spec-written files can also carry the correct nicks.
    if (FM2KSharedMemData* sm = GetSharedMemory();
        sm && (g_player_index == 0 || g_player_index == 1)) {
        const char* my_nick   = sm->ui_my_nick;
        const char* peer_nick = sm->ui_peer_nick;
        if (g_player_index == 0) {
            std::strncpy(hdr.p1_nick, my_nick,   sizeof(hdr.p1_nick) - 1);
            std::strncpy(hdr.p2_nick, peer_nick, sizeof(hdr.p2_nick) - 1);
        } else {  // joiner (1)
            std::strncpy(hdr.p1_nick, peer_nick, sizeof(hdr.p1_nick) - 1);
            std::strncpy(hdr.p2_nick, my_nick,   sizeof(hdr.p2_nick) - 1);
        }
    }

    // game_id from exe basename (matches the convention used by
    // upload_queue manifests at netplay.cpp:198-209). Strip dirs + .exe.
    {
        char exe_path[MAX_PATH] = {};
        DWORD n = GetModuleFileNameA(nullptr, exe_path, sizeof(exe_path));
        if (n > 0 && n < sizeof(exe_path)) {
            const char* basename = exe_path;
            for (const char* p = exe_path; *p; ++p) {
                if (*p == '\\' || *p == '/') basename = p + 1;
            }
            std::strncpy(hdr.game_id, basename, sizeof(hdr.game_id) - 1);
            // Strip ".exe" suffix — zero from dot to end (not just dot itself,
            // otherwise trailing "exe" bytes after the inline NUL show up in
            // downstream consumers that don't stop at first NUL).
            if (char* dot = std::strrchr(hdr.game_id, '.')) {
                std::memset(dot, 0,
                            sizeof(hdr.game_id) - (size_t)(dot - hdr.game_id));
            }
        }
    }

    const size_t n_rounds = std::min<size_t>(round_offsets.size(),
                                             sizeof(hdr.round_offsets) / sizeof(hdr.round_offsets[0]));
    hdr.round_count = static_cast<uint8_t>(n_rounds);
    for (size_t i = 0; i < n_rounds; i++) {
        hdr.round_offsets[i] = round_offsets[i];
    }

    FILE* fp = std::fopen(path, "wb");
    if (!fp) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "SpectatorNode: failed to open %s for write", path);
        return false;
    }
    std::fwrite(&hdr, 1, sizeof(hdr), fp);
    std::fwrite(body.data(), 1, body.size(), fp);
    std::fclose(fp);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "SpectatorNode: wrote %s v2 (%u events, %u INPUTs, %u rounds, "
        "session=0x%016llX, %zu bytes, %s)",
        path, hdr.event_count, hdr.input_count, hdr.round_count,
        (unsigned long long)hdr.session_id,
        sizeof(hdr) + body.size(),
        is_battle_slice ? "battle slice" : "full session");
    return true;
}

} // namespace

bool SpectatorNode_WriteSessionFile(const char* path) {
    return WriteSessionFileImpl(path,
                                g_state.session_events,
                                g_state.match_headers,
                                0, g_state.session_events.size(),
                                /*is_battle_slice=*/false);
}

bool SpectatorNode_WriteCurrentBattleFile(const char* path) {
    if (g_state.last_match_start_idx < 0) return false;
    // Slice from the start of the state-init prefix (PIN_RNG / RESET /
    // SOUND_INIT / SESSION_ID block immediately preceding MATCH_START)
    // not from MATCH_START itself. Without the prefix the file isn't
    // self-replayable: a spectator that loads it would inherit stale
    // RNG seed / input edge state / sound layer state from whatever
    // ran in their FM2K before the load. With the prefix, the events
    // drain through ApplySessionEvent and rebuild the engine to the
    // exact state the host had at battle entry.
    const size_t first = (g_state.last_pre_match_init_idx >= 0)
        ? static_cast<size_t>(g_state.last_pre_match_init_idx)
        : static_cast<size_t>(g_state.last_match_start_idx);
    const size_t last  = g_state.session_events.size();
    return WriteSessionFileImpl(path,
                                g_state.session_events,
                                g_state.match_headers,
                                first, last,
                                /*is_battle_slice=*/true);
}

// State-init event types — emitted unconditionally during a Pass-1 walk
// up to the seek anchor. These rebuild engine state (RNG seed, input ring
// reset, sound layer init, match header) so playback resumes correctly at
// the anchor without replaying every prior INPUT. ROUND_END is included
// so the post-round banner clean-up state is consistent at the moment the
// next ROUND_START fires; ROUND_START itself is what the seek lands on,
// so it's NOT emitted in Pass 1 (Pass 2 starts at the ROUND_START tag and
// pushes it as the first event).
static bool IsStateInitForSeek(SessionEventType t) {
    return t == SessionEventType::PIN_RNG
        || t == SessionEventType::RESET_INPUT_STATE
        || t == SessionEventType::SOUND_INIT
        || t == SessionEventType::MATCH_START
        || t == SessionEventType::SESSION_ID;
}

bool SpectatorNode_LoadSessionFile(const char* path, const SeekTarget& seek) {
    FILE* fp = std::fopen(path, "rb");
    if (!fp) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "SpectatorNode: failed to open %s for read", path);
        return false;
    }

    FM2KSessionFileHeader hdr = {};
    if (std::fread(&hdr, 1, sizeof(hdr), fp) != sizeof(hdr)) {
        std::fclose(fp);
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "SpectatorNode: %s truncated header", path);
        return false;
    }
    if (hdr.magic != SESSION_FILE_MAGIC) {
        std::fclose(fp);
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "SpectatorNode: %s bad magic 0x%08X", path, hdr.magic);
        return false;
    }
    if (hdr.version != SESSION_FILE_VERSION) {
        std::fclose(fp);
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "SpectatorNode: %s unsupported version %u (expected %u)",
            path, hdr.version, SESSION_FILE_VERSION);
        return false;
    }

    // Resolve the seek anchor against header round_offsets[] before
    // touching pb_queue. If the seek is unsatisfiable (round idx out of
    // range, header missing round table, etc.) bail without disturbing
    // playback state — caller can retry without seek.
    size_t anchor_offset = 0;  // 0 = no seek, walk from body start
    if (seek.kind == SeekEventKind::ROUND_START) {
        if (hdr.round_count == 0 ||
            (hdr.flags & (1u << 1)) == 0) {
            std::fclose(fp);
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                "SpectatorNode: %s has no round_offsets — seek unavailable",
                path);
            return false;
        }
        if (seek.idx == 0 || seek.idx > hdr.round_count) {
            std::fclose(fp);
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                "SpectatorNode: %s seek round=%u out of range (have %u)",
                path, seek.idx, hdr.round_count);
            return false;
        }
        anchor_offset = hdr.round_offsets[seek.idx - 1];
    }

    const uint32_t reported_event_count = hdr.event_count;
    const uint64_t loaded_session_id    = hdr.session_id;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "SpectatorNode: %s loading v2 (events=%u, %u rounds, session=0x%016llX)",
        path, hdr.event_count, hdr.round_count,
        (unsigned long long)hdr.session_id);

    std::fseek(fp, 0, SEEK_END);
    long total = std::ftell(fp);
    if (total < (long)sizeof(hdr)) {
        std::fclose(fp);
        return false;
    }
    const size_t body_len = static_cast<size_t>(total) - sizeof(hdr);
    std::fseek(fp, static_cast<long>(sizeof(hdr)), SEEK_SET);
    std::vector<uint8_t> body(body_len);
    if (std::fread(body.data(), 1, body_len, fp) != body_len) {
        std::fclose(fp);
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "SpectatorNode: %s body read failed", path);
        return false;
    }
    std::fclose(fp);

    if (anchor_offset > body_len) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "SpectatorNode: %s anchor offset %zu past body end %zu",
            path, anchor_offset, body_len);
        return false;
    }

    // Fresh playback: clear receiver state, then walk events.
    g_state.pb_queue.clear();
    g_state.pb_match_headers.clear();
    g_state.pb_current_p1 = 0;
    g_state.pb_current_p2 = 0;
    g_state.have_frame_baseline = false;
    g_state.next_expected_frame = 0;
    g_state.playing_back = true;
    if (loaded_session_id != 0) g_state.session_id = loaded_session_id;

    auto push_event = [&](SessionEvent& ev, const uint8_t* hdr_buf) {
        if (ev.type == SessionEventType::MATCH_START) {
            MatchHeader hdr_copy;
            std::memcpy(hdr_copy.data(), hdr_buf, hdr_copy.size());
            g_state.pb_match_headers.push_back(hdr_copy);
            ev.u.match_start_idx =
                static_cast<uint16_t>(g_state.pb_match_headers.size() - 1);
        }
        g_state.pb_queue.push_back(ev);
    };

    size_t off = 0;
    uint32_t pushed_inputs = 0;
    uint32_t pre_anchor_skipped = 0;

    // Pass 1 — body[0..anchor_offset). Emit only state-init events
    // (skipped if anchor_offset == 0, i.e. no-seek).
    while (off < anchor_offset) {
        SessionEvent ev{};
        uint8_t hdr_buf[SESSION_EVENT_MATCH_HDR_SIZE] = {};
        size_t r = SessionEvent_Decode(body.data() + off, body_len - off,
                                       &ev, hdr_buf);
        if (r == 0) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                "SpectatorNode: %s pass1 decode failed at off=%zu",
                path, off);
            return false;
        }
        if (off + r > anchor_offset) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                "SpectatorNode: %s anchor offset %zu mid-event "
                "(event ends at %zu)",
                path, anchor_offset, off + r);
            return false;
        }
        if (IsStateInitForSeek(ev.type)) {
            push_event(ev, hdr_buf);
        } else {
            ++pre_anchor_skipped;
        }
        off += r;
    }

    // Pass 2 — body[anchor_offset..body_len). Emit everything.
    while (off < body_len) {
        SessionEvent ev{};
        uint8_t hdr_buf[SESSION_EVENT_MATCH_HDR_SIZE] = {};
        size_t r = SessionEvent_Decode(body.data() + off, body_len - off,
                                       &ev, hdr_buf);
        if (r == 0) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                "SpectatorNode: %s pass2 decode failed at off=%zu",
                path, off);
            return false;
        }
        push_event(ev, hdr_buf);
        if (ev.type == SessionEventType::INPUT) ++pushed_inputs;
        off += r;
    }

    if (seek.kind == SeekEventKind::ROUND_START) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "SpectatorNode: loaded %s — seek=ROUND_START %u, "
            "%u state-init kept + %u skipped pre-anchor, "
            "%u INPUTs queued post-anchor (%u total events)",
            path, seek.idx,
            (uint32_t)(g_state.pb_queue.size() - pushed_inputs),
            pre_anchor_skipped, pushed_inputs, reported_event_count);
    } else {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "SpectatorNode: loaded %s — %u events (%u INPUTs) into pb_queue",
            path, reported_event_count, pushed_inputs);
    }
    return true;
}

// -----------------------------------------------------------------------------
// SPECTATOR-SIDE OP APPLY (C3)
// -----------------------------------------------------------------------------
//
// Called from PopFrameInputs head-drain loop. Each non-INPUT event at the
// head dispatches here before the next INPUT pops; the local memory write
// happens at the same logical frame the host's write happened, eliminating
// the game_mode-driven mirror race that lived in CheckGameModeTransition.

namespace {

void ApplyResetInputState() {
    // Mirror Netplay_StartBattle's first-call SaveState_Save reset
    // (savestate.cpp:223-237). FM2K addresses; FM95 will need its own
    // mapping if/when spectator support extends to CPW.
    *(uint32_t*)0x447EE0 = 0;            // g_input_buffer_index
    *(uint32_t*)0x4456FC = 0;            // render frame counter
    std::memset((void*)0x447F00, 0, 0x20);    // g_prev_input_state
    std::memset((void*)0x447F40, 0, 0x20);    // g_processed_input
    std::memset((void*)0x447F60, 0, 0x20);    // g_input_changes
    std::memset((void*)0x4280D8, 0, 0x2008);  // input_history rings (P1+P2)
}

void ApplySessionEvent(const SessionEvent& ev) {
    switch (ev.type) {
        case SessionEventType::PIN_RNG:
            // The host emitted PIN_RNG at battle-entry, AFTER title/CSS
            // sim had already consumed RNG. If we apply it eagerly here
            // (at first replay tick = title screen), then title/CSS run
            // RNG-consuming code starting FROM the pinned seed, mutating
            // it further → first battle frame's rng != host's first
            // battle frame's rng. Visual / engine state matches (since
            // title/CSS rng draws don't affect chars), but the parity
            // recorder's rng field diverges.
            //
            // Defer: write rng AT battle-entry (game_mode flip to 3000)
            // instead of immediately. Stash the seed; SpectatorSimOneFrame's
            // initial-sync block applies it at the same logical instant
            // host's PIN_RNG fired.
            g_state.pending_pin_rng_seed  = ev.u.pin_rng_seed;
            g_state.pending_pin_rng_valid = true;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "SpectatorNode: queued PIN_RNG=0x%08X (will apply at battle entry)",
                ev.u.pin_rng_seed);
            break;
        case SessionEventType::RESET_INPUT_STATE:
            ApplyResetInputState();
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "SpectatorNode: applied RESET_INPUT_STATE");
            break;
        case SessionEventType::SOUND_INIT:
            SoundRollback::Init();
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "SpectatorNode: applied SOUND_INIT");
            break;
        case SessionEventType::MATCH_START: {
            // Look up the cached 96-byte header by side-table index.
            // Header layout matches Replay::ReplayHeader on-disk; pull
            // seed/state-hash/char/color and re-publish into the playback
            // metadata so any UI consumers (HUD, replay loader handoff)
            // see the live values.
            if (ev.u.match_start_idx < g_state.pb_match_headers.size()) {
                const uint8_t* h = g_state.pb_match_headers[ev.u.match_start_idx].data();
                uint32_t seed = 0, state_hash = 0;
                std::memcpy(&seed,       h + 20, 4);
                std::memcpy(&state_hash, h + 24, 4);
                g_state.pb_initial_seed  = seed;
                g_state.pb_initial_state = state_hash;
                g_state.pb_p1_char       = h[28];
                g_state.pb_p1_color      = h[29];
                g_state.pb_p2_char       = h[30];
                g_state.pb_p2_color      = h[31];
                g_state.pb_stage_id      = h[80];
                // Mirror the legacy INITIAL_MATCH packet path so the
                // initial-match cache stays valid for relay-to-sub-spectator.
                std::memcpy(g_state.initial_match.header_bytes, h, 96);
                g_state.initial_match.valid = true;
            }
            g_state.playing_back = true;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "SpectatorNode: applied MATCH_START seed=0x%08X p1=%u/%u p2=%u/%u stage=%u",
                g_state.pb_initial_seed,
                g_state.pb_p1_char, g_state.pb_p1_color,
                g_state.pb_p2_char, g_state.pb_p2_color,
                g_state.pb_stage_id);
            // For offline replay (FM2K_REPLAY_FILE set), arm the CSS auto-
            // lock-and-confirm hook so the local game pins to the recorded
            // chars/stage when CSS opens. Live-spec (host stream) walks CSS
            // via the upstream input log and doesn't need this. Cached
            // once — getenv hits an env block walk every call otherwise.
            {
                static int s_offline_replay_cached = -1;
                if (s_offline_replay_cached < 0) {
                    const char* v = std::getenv("FM2K_REPLAY_FILE");
                    s_offline_replay_cached = (v && v[0]) ? 1 : 0;
                }
                if (s_offline_replay_cached == 1) {
                    CssAutoConfirm_OnReplayMatchStart(
                        g_state.pb_p1_char, g_state.pb_p1_color,
                        g_state.pb_p2_char, g_state.pb_p2_color,
                        g_state.pb_stage_id);
                }
            }
            break;
        }
        case SessionEventType::MATCH_END: {
            // Don't clear pb_queue — let queued post-MATCH_END frames drain
            // (they render the final battle frames). The next MATCH_START
            // resets metadata and flips playing_back back on.
            g_state.playing_back = false;
            const auto& p = ev.u.match_end;
            const char* who = (p.winner_idx == 0) ? "P1"
                            : (p.winner_idx == 1) ? "P2" : "DRAW";
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "SpectatorNode: applied MATCH_END winner=%s rounds=%u-%u "
                "frames=%u (queued=%zu)",
                who, p.rounds_won_p1, p.rounds_won_p2, p.frames_total,
                g_state.pb_queue.size());
            break;
        }
        case SessionEventType::FINGERPRINT: {
            // C9: diagnostic mismatch detection. Host emits its hash here;
            // spectator computes the same hash on its current state and
            // compares. Drain-at-head ordering ensures both sides sample
            // at the same logical frame.
            if (SpectatorFingerprint_Enabled()) {
                const uint32_t host_hash = ev.u.fingerprint_hash;
                const uint32_t local_hash = SpectatorFingerprint_Compute();
                if (host_hash != local_hash) {
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[SPEC-FP-MISMATCH] host=0x%08X spectator=0x%08X — "
                        "DESYNC at this logical frame (next INPUT is the "
                        "first divergent sim step)",
                        host_hash, local_hash);
                } else {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[SPEC-FP-OK] host=0x%08X (matches local)", host_hash);
                }
            }
            break;
        }
        case SessionEventType::ROUND_START: {
            // C3.5 — informational on the spectator. Simulation drives banner
            // and round-reset state from INPUT events; ROUND_START is a marker
            // for replay-file slicing (round_offsets[]) and overlay diagnostics.
            const auto& p = ev.u.round_start;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "SpectatorNode: ROUND_START round=%u p1_hp_max=%u p2_hp_max=%u timer=%us",
                p.round_idx, p.p1_hp_max, p.p2_hp_max, p.timer_seconds);
            break;
        }
        case SessionEventType::ROUND_END: {
            const auto& p = ev.u.round_end;
            const char* who = (p.winner_idx == 0) ? "P1"
                            : (p.winner_idx == 1) ? "P2" : "DRAW";
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "SpectatorNode: ROUND_END winner=%s p1_hp=%u p2_hp=%u frames=%u",
                who, p.p1_hp_remaining, p.p2_hp_remaining, p.frames_elapsed);
            break;
        }
        case SessionEventType::SESSION_ID:
            // C7 — informational on the spectator. The host's session_id
            // already lives at the head of the event stream by the time
            // we apply this; nothing else to do beyond logging. Spectator
            // recordings (.fm2kset / .fm2krep) will inherit this id when
            // C7's writer pulls it from g_state.
            g_state.session_id = ev.u.session_id;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "SpectatorNode: applied SESSION_ID=0x%016llX",
                (unsigned long long)ev.u.session_id);
            break;
        case SessionEventType::INPUT:
            // Should not reach here — INPUT is handled by the pop path in
            // PopFrameInputs, not the drain.
            break;
    }
}

} // namespace

// -----------------------------------------------------------------------------
// JOIN / REDIRECT
// -----------------------------------------------------------------------------

void SpectatorNode_HandleJoinReq(const sockaddr_in& from, SpecJoinMode mode) {
    char addr_buf[48] = {};
    FormatAddr(from, addr_buf, sizeof(addr_buf));

    // Helper: build SPEC_JOIN_ACK with the host's current session kind so
    // the spectator knows which GekkoSpectateSession config to create.
    auto BuildJoinAck = []() {
        CtrlPacket ack = {};
        ack.header.type = CtrlMsg::SPEC_JOIN_ACK;
        const NetplaySessionKind k = Netplay_GetSessionKind();
        ack.data.spec_join_ack.host_session_kind = static_cast<uint8_t>(k);
        // Tell the spectator which TCP port to dial for the INPUT_BATCH
        // stream. Zero would mean the listener failed at startup, in which
        // case the spectator refuses the subscription.
        ack.data.spec_join_ack.host_tcp_port = SpectatorTCP::GetListenPort();
        // Default "unknown" — only valid when host is in battle.
        ack.data.spec_join_ack.host_p1_char = 0xFF;
        ack.data.spec_join_ack.host_p2_char = 0xFF;
        ack.data.spec_join_ack.host_stage   = 0xFF;
        if (k == NetplaySessionKind::BATTLE) {
            // Read the engine's current post-CSS-confirm chars + stage so
            // the spec can /F-boot with the RIGHT character files. These
            // live at ADDR_P1_SELECTED_CHAR / ADDR_P2_SELECTED_CHAR (the
            // same addresses Netplay_StartBattle reads for its "match
            // chars p1=N(...) p2=N(...)" log) and ADDR_SELECTED_STAGE.
            //
            // Note: g_config_value1/3 (0x4300E0/0x4300F0) are only
            // populated when the HOST itself was /F-launched — for a
            // normal CSS walk they stay at 0, which is why the previous
            // read gave us p1=0/p2=0 and pkmncc crashed loading a
            // mirror Blaziken matchup.
            const uint32_t p1 = *(const uint32_t*)FM2K::ADDR_P1_SELECTED_CHAR;
            const uint32_t p2 = *(const uint32_t*)FM2K::ADDR_P2_SELECTED_CHAR;
            const uint32_t st = (FM2K::ADDR_SELECTED_STAGE != 0)
                                  ? *(const uint32_t*)FM2K::ADDR_SELECTED_STAGE
                                  : 0u;
            if (p1 < 50u) ack.data.spec_join_ack.host_p1_char = (uint8_t)p1;
            if (p2 < 50u) ack.data.spec_join_ack.host_p2_char = (uint8_t)p2;
            if (st < 50u) ack.data.spec_join_ack.host_stage   = (uint8_t)st;
        }
        return ack;
    };

    // Helper: if there's a live GekkoNet session on this node (player slot),
    // add the joining spectator as a GekkoSpectator actor so confirmed-input
    // events and (battle) Save/Load events reach them natively. Both CSS
    // and BATTLE sessions get spectators added — CSS doesn't emit Save/Load
    // (lockstep suppresses them) but it does emit GekkoAdvanceEvent per
    // confirmed frame, and that's the source of truth that drives the
    // spectator's local sim 1:1 with the host.
    auto AddSpectatorToSession = [](const sockaddr_in& spec_from) {
        const NetplaySessionKind k = Netplay_GetSessionKind();
        if (k != NetplaySessionKind::CSS && k != NetplaySessionKind::BATTLE) {
            return;
        }
        char ip_str[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, (void*)&spec_from.sin_addr, ip_str, sizeof(ip_str));
        char addr_str[64];
        snprintf(addr_str, sizeof(addr_str), "%s:%u",
                 ip_str, ntohs(spec_from.sin_port));
        GekkoSession* sess = Netplay_GetActiveSession();
        if (!sess) return;

        // Dedup against this-session's previously-added spec addrs. Without
        // this, a spec stuck in a 5s retry loop (e.g. TCP punch failing on
        // symmetric NAT) re-fires SPEC_JOIN_REQ every cycle and we'd
        // gekko_add_actor on each, leaking one actor per retry. GekkoNet
        // has no remove_actor counterpart, so dedup-on-add is the only
        // bound. Set is cleared at session boundaries from netplay.cpp.
        const std::string addr_key(addr_str);
        if (g_gekko_spectator_addrs.count(addr_key)) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "SpectatorNode: GekkoSpectator already on %s session for %s -- skipping re-add",
                k == NetplaySessionKind::CSS ? "CSS" : "BATTLE",
                addr_str);
            return;
        }
        GekkoNetAddress addr = {};
        addr.data = (void*)addr_str;
        addr.size = (int)strlen(addr_str);
        gekko_add_actor(sess, GekkoSpectator, &addr);
        g_gekko_spectator_addrs.insert(addr_key);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "SpectatorNode: Late-joiner added as GekkoSpectator on %s session -> %s",
            k == NetplaySessionKind::CSS ? "CSS" : "BATTLE",
            addr_str);
    };

    // Already subscribed? Reset the slot's TCP-bound state so the new
    // JOIN_REQ re-fires the bind + backfill path. Without this, a previous
    // spectator session whose TCP read-errored leaves the slot with
    // tcp_bound=true; the next JOIN_REQ from same UDP source treats it as
    // a duplicate and never re-ships snapshot/backfill — symptom is the
    // spectator's silence-failover triggering every 5s in a reconnect
    // loop with the host accepting TCP but never sending data.
    //
    // Also refreshes join_mode in case the spectator switched modes (e.g.
    // CURRENT_MATCH on first connect, FULL_SESSION on retry after fallback)
    // and bumps last_seen_ms so the host's own subscriber-expiry sweep
    // doesn't cull this slot mid-rebind.
    for (auto& sub : g_state.subscribers) {
        if (AddrEqual(sub.addr, from)) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "SpectatorNode: JOIN_REQ from existing subscriber %s — "
                        "resetting bind state for fresh backfill (mode=%s)",
                        addr_buf,
                        mode == SpecJoinMode::CURRENT_MATCH ? "CURRENT_MATCH"
                                                            : "FULL_SESSION");
            sub.tcp_bound    = false;
            sub.ack_frame    = 0;
            sub.join_mode    = mode;
            sub.last_seen_ms = GetTickCount64();
            CtrlPacket ack = BuildJoinAck();
            ControlChannel_SendTo(ack, from);
            return;
        }
    }

    if (g_state.subscribers.size() < g_state.capacity) {
        Subscriber sub = {};
        sub.addr         = from;
        sub.last_seen_ms = GetTickCount64();
        sub.ack_frame    = 0;
        sub.tcp_bound    = false;
        sub.join_mode    = mode;
        g_state.subscribers.push_back(sub);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: Accepted subscriber %s (%zu/%zu, mode=%s) — "
                    "deferring INITIAL_MATCH until TCP binds",
                    addr_buf, g_state.subscribers.size(), g_state.capacity,
                    mode == SpecJoinMode::CURRENT_MATCH ? "CURRENT_MATCH"
                                                       : "FULL_SESSION");

        CtrlPacket ack = BuildJoinAck();
        ControlChannel_SendTo(ack, from);

        // If we already have a live GekkoNet session, add this late joiner
        // as a GekkoSpectator actor so the input stream reaches them.
        AddSpectatorToSession(from);

        // INITIAL_MATCH + SendSessionBackfillTo are sent by TickHealth's
        // TryBindPendingTCP path the first time the spectator's accepted
        // TCP connection gets paired with this subscriber slot.
        return;
    }

    // At capacity — random redirect à la CCCaster.
    if (!g_state.subscribers.empty()) {
        const size_t i = static_cast<size_t>(std::rand()) % g_state.subscribers.size();
        const sockaddr_in& target = g_state.subscribers[i].addr;

        CtrlPacket redir = {};
        redir.header.type = CtrlMsg::SPEC_JOIN_REDIRECT;
        redir.data.spec_redirect.redirect_ip   = target.sin_addr.s_addr;
        redir.data.spec_redirect.redirect_port = ntohs(target.sin_port);
        ControlChannel_SendTo(redir, from);

        char target_buf[48] = {};
        FormatAddr(target, target_buf, sizeof(target_buf));
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: At capacity, redirecting %s -> %s",
                    addr_buf, target_buf);
        return;
    }

    // Capacity=0, no subscribers — reject with null redirect. Viewer gives up.
    CtrlPacket redir = {};
    redir.header.type = CtrlMsg::SPEC_JOIN_REDIRECT;
    redir.data.spec_redirect.redirect_ip   = 0;
    redir.data.spec_redirect.redirect_port = 0;
    ControlChannel_SendTo(redir, from);
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "SpectatorNode: Rejected JOIN_REQ from %s (capacity=0)", addr_buf);
}

void SpectatorNode_HandleLeave(const sockaddr_in& from) {
    // First check: was this from our upstream telling us to leave (it's
    // shutting down)? If so, immediately fail over to root rather than
    // waiting out the silence timer.
    if (g_state.subscribed_upstream && AddrEqual(g_state.upstream_addr, from)) {
        char buf[48] = {}; FormatAddr(from, buf, sizeof(buf));
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: upstream %s sent SPEC_LEAVE — failing over to root",
                    buf);
        g_state.subscribed_upstream = false;
        // TickHealth will pick up the disconnected state and trigger
        // RequestJoin(root) on its next call (rate-limited).
        return;
    }
    // Otherwise, treat as a downstream subscriber leaving us.
    for (auto it = g_state.subscribers.begin(); it != g_state.subscribers.end(); ++it) {
        if (AddrEqual(it->addr, from)) {
            char buf[48] = {}; FormatAddr(from, buf, sizeof(buf));
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "SpectatorNode: Subscriber %s left", buf);
            SpectatorTCP::DisconnectSubscriber(it->addr);
            g_state.subscribers.erase(it);
            return;
        }
    }
}

void SpectatorNode_HandleHeartbeat(const sockaddr_in& from) {
    for (auto& sub : g_state.subscribers) {
        if (AddrEqual(sub.addr, from)) {
            sub.last_seen_ms = GetTickCount64();
            return;
        }
    }
}

// -----------------------------------------------------------------------------
// STATUS
// -----------------------------------------------------------------------------

size_t SpectatorNode_GetSubscriberCount() { return g_state.subscribers.size(); }
bool   SpectatorNode_IsBroadcasting()     { return g_state.broadcasting;      }

std::vector<sockaddr_in> SpectatorNode_GetSubscriberAddrs() {
    std::vector<sockaddr_in> out;
    out.reserve(g_state.subscribers.size());
    for (const auto& sub : g_state.subscribers) {
        out.push_back(sub.addr);
    }
    return out;
}

// -----------------------------------------------------------------------------
// VIEWER-SIDE
// -----------------------------------------------------------------------------

bool SpectatorNode_RequestJoin(const sockaddr_in& upstream, SpecJoinMode mode) {
    g_state.upstream_addr       = upstream;
    g_state.subscribed_upstream = false;
    g_state.last_requested_mode = mode;  // sticky — see comment in State decl
    // Bump reconnect timestamp so TickHealth's failover backoff covers the
    // INITIAL JOIN_REQ too. Without this, last_reconnect_attempt_ms stays
    // 0 after the first send, the next TickHealth pass sees
    // `now - 0 >= BACKOFF_MS`, and fires a second RequestJoin within
    // milliseconds — clobbering the spectator's declared mode before the
    // host's first JOIN_ACK even arrives.
    g_state.last_reconnect_attempt_ms = GetTickCount64();
    g_state.join_req_pending    = true;  // Gates HandleJoinAck so a stray
                                         // JOIN_ACK from the wire doesn't
                                         // promote a non-spectator client
                                         // into spectator mode.
    CtrlPacket req = {};
    req.header.type            = CtrlMsg::SPEC_JOIN_REQ;
    req.data.spec_join_req.mode = static_cast<uint8_t>(mode);
    // reserved bytes are zero from the {} init above.
    ControlChannel_SendTo(req, upstream);
    return true;
}

void SpectatorNode_HandleJoinAck(const sockaddr_in& from, uint8_t host_session_kind,
                                 uint16_t host_tcp_port,
                                 uint8_t host_p1_char, uint8_t host_p2_char,
                                 uint8_t host_stage) {
    // If host advertised real chars (in-battle), forward to the BTB
    // runtime-override channel so the slot-0 /F dispatcher loads the
    // host's actual character files. We CAN'T use SetEnvironmentVariableA
    // + getenv here — Win32 SetEnv updates the process env block but
    // not the CRT's _environ cache, so getenv() in BTB returns the stale
    // launcher-provided placeholder (char 0). PerGamePatches keeps a
    // hook-internal struct that BTB reads first; this bypasses the CRT
    // cache entirely.
    if (host_session_kind == 2 /* BATTLE */) {
        PerGamePatches_SetRuntimeBtbOverrides(host_p1_char,
                                              host_p2_char,
                                              host_stage);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "SpectatorNode: seeded runtime BTB from JOIN_ACK "
            "(p1=%u p2=%u stage=%u)",
            (unsigned)host_p1_char, (unsigned)host_p2_char,
            (unsigned)host_stage);
    }

    // SPEC_JOIN_ACK is dual-purpose:
    //   1. First-time arrival (initial subscribe): completes the JOIN_REQ
    //      handshake, pins RNG, marks subscribed_upstream, opens TCP up.
    //   2. Re-broadcast on host session-kind change (host crosses a
    //      session boundary like CSS->battle or first-CSS-create): the
    //      payload's host_session_kind tells the spectator which kind to
    //      mirror. Treated as authoritative current-host-kind.
    //
    // Both paths funnel through the same handler so the spectator can
    // recover cleanly if the host transitions before the JOIN_REQ ack
    // round-trip lands. Stray ACKs from non-upstream peers are still
    // dropped (sender addr check below).
    const bool first_time = g_state.join_req_pending;

    if (!first_time && !AddrEqual(g_state.upstream_addr, from)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: ignoring SPEC_JOIN_ACK from non-upstream peer");
        return;
    }

    g_state.join_req_pending      = false;
    g_state.upstream_addr         = from;
    g_state.subscribed_upstream   = true;

    // RNG sync + queue clear: ONLY apply on FIRST-TIME subscribe (spectator
    // is still at title/pre-CSS, no game state to lose). On reconnect-after-
    // silence-failover or on re-broadcast ACK, spectator's local sim is
    // mid-match — clobbering RNG / wiping the queue would erase in-progress
    // state. Gate both on first-time only.
    if (first_time) {
        *(uint32_t*)0x41FB1C = 0x12345678;
        g_state.pb_queue.clear();
        g_state.pb_current_p1 = 0;
        g_state.pb_current_p2 = 0;
    }
    g_state.playing_back = true;

    // Dial the host's TCP port. Bulk INPUT_BATCH / INITIAL_MATCH /
    // MATCH_END flow over TCP exclusively; if the host didn't advertise
    // a port, this peer has nothing to listen to and the subscription
    // is unusable. Bail.
    if (host_tcp_port == 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "SpectatorNode: JOIN_ACK from host has no TCP port — refusing");
        g_state.subscribed_upstream = false;
        return;
    }
    {
        char host_ip[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, (void*)&from.sin_addr, host_ip, sizeof(host_ip));
        if (!SpectatorTCP::ConnectUpstream(host_ip, host_tcp_port)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                "SpectatorNode: SpectatorTCP::ConnectUpstream(%s:%u) failed",
                host_ip, (unsigned)host_tcp_port);
            g_state.subscribed_upstream = false;
            return;
        }
    }

    char buf[48] = {}; FormatAddr(from, buf, sizeof(buf));
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "SpectatorNode: JOIN_ACK from %s (host_kind=%u, tcp_port=%u) — subscribed",
                buf, (unsigned)host_session_kind, (unsigned)host_tcp_port);

    // host_session_kind == 0 (NONE): host has no session active yet
    // (e.g. JOIN_REQ landed during host's title-skip phase before its
    // first CSS session was created). DO NOT create a SpectateSession
    // with a guessed config — that caused config mismatch deadlocks
    // before. Wait for host's follow-up SPEC_JOIN_ACK after session create.
    if (host_session_kind == 0) {
        char buf2[48] = {}; FormatAddr(from, buf2, sizeof(buf2));
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: host has no session yet — waiting (from=%s)", buf2);
        return;
    }

    NetplaySessionKind kind = (host_session_kind == 1)
        ? NetplaySessionKind::CSS
        : NetplaySessionKind::BATTLE;

    char host_addr_str[64];
    snprintf(host_addr_str, sizeof(host_addr_str), "%s:%u",
             inet_ntoa(from.sin_addr), ntohs(from.sin_port));

    // If a SpectateSession is already alive, only act on a kind CHANGE.
    // Same-kind re-broadcasts (host re-acks for liveness / new spectator
    // joining the same session) are no-ops via Netplay_StartSpectateSession's
    // own idempotency guard.
    const NetplaySessionKind current = Netplay_GetSessionKind();
    if (current == NetplaySessionKind::SPECTATE) {
        // Wrong-kind alive — treat as a phase swap. swap_frame=0 fires
        // immediately on the next AdvanceEvent.
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: host kind changed mid-session, requesting swap to kind=%d",
                    (int)kind);
        if (kind == NetplaySessionKind::BATTLE) {
            Netplay_OnHostBattleEntering(0);
        } else {
            Netplay_OnHostBattleEnd(0);
        }
        return;
    }

    Netplay_StartSpectateSession(kind, host_addr_str);
}

void SpectatorNode_HandleJoinRedirect(const sockaddr_in& from,
                                      uint32_t redirect_ip,
                                      uint16_t redirect_port)
{
    (void)from;
    if (redirect_ip == 0 && redirect_port == 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: Redirect with no target — giving up");
        g_state.subscribed_upstream = false;
        return;
    }
    sockaddr_in target = {};
    target.sin_family      = AF_INET;
    target.sin_addr.s_addr = redirect_ip;
    target.sin_port        = htons(redirect_port);
    char buf[48] = {}; FormatAddr(target, buf, sizeof(buf));
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "SpectatorNode: Redirecting to %s (mode=%s)", buf,
                g_state.last_requested_mode == SpecJoinMode::CURRENT_MATCH
                    ? "CURRENT_MATCH" : "FULL_SESSION");
    SpectatorNode_RequestJoin(target, g_state.last_requested_mode);
}

void SpectatorNode_HandleSpecData(const uint8_t* buf, size_t len,
                                  const sockaddr_in& from)
{
    (void)from;
    if (len < sizeof(SpecDataHeader)) return;
    SpecDataHeader hdr;
    std::memcpy(&hdr, buf, sizeof(hdr));
    if (hdr.magic != SPEC_DATA_MAGIC) return;

    const uint8_t* payload = buf + sizeof(hdr);
    const size_t   payload_len = len - sizeof(hdr);

    switch (hdr.type) {
        case SpecDataType::INITIAL_MATCH:
        case SpecDataType::INPUT_BATCH:
        case SpecDataType::INPUT_REQUEST:
            // Legacy wire types — retired in C12. Production hosts ship
            // MATCH_START / MATCH_END / FINGERPRINT / per-INPUT events
            // inside EVENT_BATCH datagrams. The enum values stay in
            // spectator_node.h for ABI but receivers no longer act on them
            // (silently drop instead of warn — old peers shouldn't exist
            // in practice and a stale packet during a hub-driven session
            // swap shouldn't spam the log).
            break;
        case SpecDataType::MATCH_END:
            // Same: legacy stand-alone MATCH_END packet retired. The
            // SessionEvent::MATCH_END op flowing through EVENT_BATCH
            // handles match-end on the new path.
            break;
        case SpecDataType::SNAPSHOT_BEGIN: {
            // CURRENT_MATCH-mode bind path opener. Wire layout (see
            // SendSnapshotTo): hdr.start_frame = anchor INPUT-frame for
            // the post-snapshot event stream, hdr.flags = sizeof(meta) =
            // 16, payload = SnapshotMetadata.
            if (payload_len < sizeof(SnapshotMetadata)) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: SNAPSHOT_BEGIN truncated (payload=%zu, need %zu)",
                    payload_len, sizeof(SnapshotMetadata));
                g_state.pb_snapshot_inbox = State::SnapshotInbox{};
                break;
            }
            SnapshotMetadata meta = {};
            std::memcpy(&meta, payload, sizeof(meta));

            if (meta.version != SPECTATOR_SNAPSHOT_VERSION) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: SNAPSHOT_BEGIN version mismatch "
                    "(got %u, want %u) — dropping snapshot, peer must "
                    "rejoin with FULL_SESSION",
                    meta.version, SPECTATOR_SNAPSHOT_VERSION);
                g_state.pb_snapshot_inbox = State::SnapshotInbox{};
                break;
            }

            const size_t expected_slot_size = SaveState_GetSlotByteSize();
            if (meta.total_bytes == 0 ||
                meta.total_bytes != expected_slot_size) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: SNAPSHOT_BEGIN size mismatch "
                    "(blob=%u, slot=%zu) — engine variant mismatch?",
                    meta.total_bytes, expected_slot_size);
                g_state.pb_snapshot_inbox = State::SnapshotInbox{};
                break;
            }

            auto& inbox = g_state.pb_snapshot_inbox;
            if (inbox.active) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: SNAPSHOT_BEGIN restart "
                    "(prior inbox had %zu/%u bytes, dropping)",
                    inbox.bytes_received, inbox.meta.total_bytes);
            }
            inbox.meta           = meta;
            inbox.anchor_frame   = hdr.start_frame;
            inbox.bytes_received = 0;
            inbox.blob.assign(meta.total_bytes, 0);
            inbox.active         = true;

            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "SpectatorNode: SNAPSHOT_BEGIN match=%u, %u bytes, "
                "anchor INPUT-frame=%u, captured_game_mode=%u",
                meta.match_index, meta.total_bytes, inbox.anchor_frame,
                meta.captured_game_mode);

            // Battle-snapshot fast-walk for spec: when the snapshot is
            // captured at battle (game_mode=3000) but spec is still in
            // boot/title/CSS, the pb_queue starts at a post-CSS frame
            // and contains no CSS-confirm inputs. CssAutoConfirm gives
            // us programmatic "pick chars + lock in" so the spec's
            // local engine can transition CSS → battle on its own.
            // Targets are arbitrary (0/0/stage 0); the snapshot apply
            // at game_mode=3000 overwrites the entire char-data pool
            // with the host's actual matchup state.
            //
            // For CSS snapshots (captured_game_mode=2000), DON'T arm —
            // the snapshot itself will populate cursor positions +
            // selected chars when it applies at game_mode==2000, and
            // CssAutoConfirm's pinning logic would fight the loaded
            // state.
            if (g_spectator_mode && meta.captured_game_mode == 3000u) {
                CssAutoConfirm_OnReplayMatchStart(
                    /*p1_char=*/0, /*p1_color=*/0,
                    /*p2_char=*/0, /*p2_color=*/0,
                    /*stage_id=*/0);
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: armed CssAutoConfirm for spec "
                    "(battle snapshot — drive CSS forward to game_mode "
                    "3000 with placeholder chars; snapshot apply will "
                    "overwrite)");
            }
            break;
        }
        case SpecDataType::SNAPSHOT_CHUNK: {
            // hdr.start_frame = byte offset, hdr.flags = chunk byte count.
            auto& inbox = g_state.pb_snapshot_inbox;
            if (!inbox.active) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: SNAPSHOT_CHUNK without active inbox — dropping");
                break;
            }
            const size_t   off     = hdr.start_frame;
            const size_t   chunk_n = hdr.flags;
            if (chunk_n == 0 || chunk_n > payload_len) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: SNAPSHOT_CHUNK bad length (flags=%zu, payload=%zu)",
                    chunk_n, payload_len);
                inbox = State::SnapshotInbox{};
                break;
            }
            if (off + chunk_n > inbox.blob.size()) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: SNAPSHOT_CHUNK overruns blob "
                    "(off=%zu chunk=%zu, total=%zu)",
                    off, chunk_n, inbox.blob.size());
                inbox = State::SnapshotInbox{};
                break;
            }
            std::memcpy(inbox.blob.data() + off, payload, chunk_n);
            inbox.bytes_received += chunk_n;
            break;
        }
        case SpecDataType::SNAPSHOT_END: {
            // hdr.flags = 4, payload = uint32 fletcher32 over the host's blob.
            auto& inbox = g_state.pb_snapshot_inbox;
            if (!inbox.active) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: SNAPSHOT_END without active inbox — dropping");
                break;
            }
            if (payload_len < sizeof(uint32_t)) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: SNAPSHOT_END truncated (payload=%zu)",
                    payload_len);
                inbox = State::SnapshotInbox{};
                break;
            }
            uint32_t expected_checksum = 0;
            std::memcpy(&expected_checksum, payload, sizeof(uint32_t));

            if (inbox.bytes_received != inbox.meta.total_bytes) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: SNAPSHOT_END byte-count mismatch "
                    "(received %zu, want %u) — discarding",
                    inbox.bytes_received, inbox.meta.total_bytes);
                inbox = State::SnapshotInbox{};
                break;
            }
            const uint32_t local_checksum =
                Fletcher32(inbox.blob.data(), inbox.blob.size());
            if (local_checksum != expected_checksum) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: SNAPSHOT_END checksum mismatch "
                    "(local=0x%08X, expected=0x%08X) — discarding",
                    local_checksum, expected_checksum);
                inbox = State::SnapshotInbox{};
                break;
            }

            // Validation passed. DON'T apply SaveState_LoadFromBytes yet —
            // the spectator's local game may still be in pre-WinMain init
            // (game_mode == 0). Smashing battle-state bytes into uninit'd
            // engine memory crashes the next frame's render. Mark the inbox
            // pending_apply; ApplyPendingSnapshot polls each spec tick and
            // runs the actual apply once the engine has progressed past
            // mode 0. inbox.blob + meta + anchor stay populated until then.
            inbox.pending_apply = true;
            inbox.active        = false;  // assembly is done; no more chunks

            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "SpectatorNode: SNAPSHOT_END validated (match=%u, "
                "%zu bytes, fletcher32=0x%08X) — anchor INPUT-frame=%u "
                "(deferred apply pending game-mode != 0)",
                inbox.meta.match_index, inbox.blob.size(),
                local_checksum, inbox.anchor_frame);
            break;
        }
        case SpecDataType::EVENT_BATCH: {
            // Primary C2+ ingest. Payload is a packed SessionEvent[] stream
            // (1-byte tag + variant payload per event). Walk the stream
            // sequentially; for INPUT events apply the contiguous-frame
            // dedup gate; for non-INPUT events push if and only if the
            // batch as a whole is at-or-after next_expected_frame (i.e. we
            // haven't already consumed past the INPUTs they precede).
            //
            // Receive-side gate: subscription is the only thing that
            // matters here. `playing_back` was the legacy
            // "between INITIAL_MATCH and MATCH_END" state — but
            // MATCH_END now arrives in-band as an event and flips
            // playing_back=false at apply time (drain-at-head). If we
            // gated receive on playing_back we'd drop the CSS frames
            // + next-match MATCH_START that flow through the same
            // EVENT_BATCH stream right after MATCH_END.
            if (!g_state.subscribed_upstream) return;

            const uint32_t expected_at_entry = g_state.have_frame_baseline
                ? g_state.next_expected_frame : 0xFFFFFFFFu;

            // Establish baseline on first batch.
            if (!g_state.have_frame_baseline) {
                g_state.have_frame_baseline = true;
                g_state.next_expected_frame = hdr.start_frame;
            }

            // If the batch's first INPUT is already behind the cursor, the
            // whole batch is a redundant retransmit (e.g. backfill arriving
            // after live frames during the C5 race) — skip it.
            const bool batch_already_consumed =
                hdr.start_frame + hdr.frame_count <= g_state.next_expected_frame;
            if (batch_already_consumed) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: skipping fully-consumed EVENT_BATCH "
                    "(start=%u count=%u, expected=%u)",
                    hdr.start_frame, hdr.frame_count, expected_at_entry);
                break;
            }

            size_t off = 0;
            uint32_t  cursor_input  = hdr.start_frame;  // next INPUT in this batch
            uint32_t  pushed_inputs = 0;
            uint32_t  skipped_inputs = 0;
            uint32_t  gap_first      = 0xFFFFFFFFu;
            while (off < payload_len) {
                SessionEvent ev{};
                uint8_t hdr_buf[SESSION_EVENT_MATCH_HDR_SIZE];
                size_t consumed = SessionEvent_Decode(payload + off, payload_len - off,
                                                      &ev, hdr_buf);
                if (consumed == 0) {
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "SpectatorNode: EVENT_BATCH decode failed at off=%zu (len=%zu)",
                        off, payload_len);
                    break;
                }

                if (ev.type == SessionEventType::INPUT) {
                    if (cursor_input < g_state.next_expected_frame) {
                        ++skipped_inputs;          // already consumed
                    } else if (cursor_input > g_state.next_expected_frame) {
                        if (gap_first == 0xFFFFFFFFu) gap_first = cursor_input;
                    } else {
                        g_state.pb_queue.push_back(ev);
                        g_state.next_expected_frame = cursor_input + 1;
                        ++pushed_inputs;
                        // Hop-1 relay.
                        SpectatorNode_OnFrameConfirmed(ev.u.input.p1, ev.u.input.p2);
                    }
                    ++cursor_input;
                } else {
                    // Non-INPUT event. Apply when its position (= cursor_input,
                    // i.e. the index of the next INPUT) is at or after the
                    // current dedup cursor — meaning the INPUT it logically
                    // precedes hasn't been consumed yet. Otherwise the op is
                    // stale (the spectator has already executed past that
                    // boundary in a previous batch).
                    if (cursor_input >= g_state.next_expected_frame) {
                        if (ev.type == SessionEventType::MATCH_START) {
                            // Stash header in the side table so the playback
                            // driver can look it up by match_start_idx.
                            MatchHeader hdr_copy;
                            std::memcpy(hdr_copy.data(), hdr_buf, hdr_copy.size());
                            g_state.pb_match_headers.push_back(hdr_copy);
                            ev.u.match_start_idx =
                                static_cast<uint16_t>(g_state.pb_match_headers.size() - 1);
                        }
                        g_state.pb_queue.push_back(ev);

                        // Hop-1 relay: forward non-INPUT ops to sub-spectators
                        // via the same Append* helpers the host uses. AppendOp
                        // pushes to local session_events + flushes if we have
                        // subscribers. INPUT relay is handled separately above
                        // via SpectatorNode_OnFrameConfirmed.
                        switch (ev.type) {
                            case SessionEventType::PIN_RNG:
                                SpectatorNode_AppendPinRng(ev.u.pin_rng_seed);
                                break;
                            case SessionEventType::RESET_INPUT_STATE:
                                SpectatorNode_AppendResetInputState();
                                break;
                            case SessionEventType::SOUND_INIT:
                                SpectatorNode_AppendSoundInit();
                                break;
                            case SessionEventType::FINGERPRINT:
                                SpectatorNode_AppendFingerprint(ev.u.fingerprint_hash);
                                break;
                            case SessionEventType::MATCH_START:
                                // hdr_buf was populated by SessionEvent_Decode;
                                // re-append on this relay node so its sub-
                                // spectators see the same MATCH_START in
                                // their own backfill.
                                SpectatorNode_AppendMatchStart(hdr_buf);
                                break;
                            case SessionEventType::MATCH_END:
                                SpectatorNode_AppendMatchEnd(ev.u.match_end);
                                break;
                            case SessionEventType::SESSION_ID:
                                SpectatorNode_AppendSessionId(ev.u.session_id);
                                break;
                            case SessionEventType::ROUND_START: {
                                const auto& p = ev.u.round_start;
                                SpectatorNode_AppendRoundStart(
                                    p.round_idx, p.p1_hp_max, p.p2_hp_max,
                                    p.timer_seconds);
                                break;
                            }
                            case SessionEventType::ROUND_END: {
                                const auto& p = ev.u.round_end;
                                SpectatorNode_AppendRoundEnd(
                                    p.winner_idx, p.p1_hp_remaining,
                                    p.p2_hp_remaining);
                                break;
                            }
                            default:
                                break;
                        }
                    }
                }
                off += consumed;
            }
            if (gap_first != 0xFFFFFFFFu) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: EVENT_BATCH out-of-order INPUT %u (expected=%u, "
                    "batch start=%u count=%u, pushed=%u skipped=%u)",
                    gap_first, expected_at_entry,
                    hdr.start_frame, hdr.frame_count, pushed_inputs, skipped_inputs);
            }
            // pb_queue / batch / subs status — diagnostic (~1 line/sec).
            // Routed via SDL_LOG_CATEGORY_CUSTOM into quill's backtrace
            // ring; only flushed to disk on a subsequent LOG_ERROR or
            // when FM2K_SPECTATOR_DEBUG=1 is set for live diagnostic.
            {
                static uint32_t last_log_tick = 0;
                uint32_t now = (uint32_t)GetTickCount64();
                if (now - last_log_tick > 1000) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_CUSTOM,
                        "SpectatorNode: pb_queue=%zu (batch inputs=%u, start=%u, subs=%zu)",
                        g_state.pb_queue.size(), hdr.frame_count, hdr.start_frame,
                        g_state.subscribers.size());
                    last_log_tick = now;
                }
            }
            break;
        }
    }
}

bool SpectatorNode_IsSubscribedUpstream() { return g_state.subscribed_upstream; }

// -----------------------------------------------------------------------------
// PLAYBACK DRIVER API (called from main_loop_trampoline + Hook_GetPlayerInput)
// -----------------------------------------------------------------------------

bool SpectatorNode_IsPlayingBack() {
    // Sticky once subscribed: stays true from JOIN_ACK through everything
    // (active matches, MATCH_END drains, post-match idle, between-match
    // CSS), only resetting on shutdown / leave. This is what makes
    // Hook_GetPlayerInput unconditionally route through the
    // spectator-cached values — important because the spectator is
    // marked Netplay_IsConnected() (we set it in InitAsSpectator), so
    // without this gate the CSS branch of Hook_GetPlayerInput would
    // serve garbage from the spectator's empty CSS input buffers.
    return g_state.subscribed_upstream
        || g_state.playing_back
        || !g_state.pb_queue.empty();
}

bool SpectatorNode_PopFrameInputs(uint16_t* p1_input, uint16_t* p2_input) {
    // Drain non-INPUT events from the head before popping the next INPUT.
    // Each non-INPUT event dispatches to ApplySessionEvent — RNG pin,
    // input-state reset, sound dedup init, etc. The dispatch happens at
    // the moment the spectator's local sim is about to consume the next
    // INPUT, which is the same logical-frame moment the host's pin
    // happened. Eliminates the game_mode-driven mirror race.
    while (!g_state.pb_queue.empty() &&
           g_state.pb_queue.front().type != SessionEventType::INPUT) {
        ApplySessionEvent(g_state.pb_queue.front());
        g_state.pb_queue.erase(g_state.pb_queue.begin());
    }
    if (g_state.pb_queue.empty()) return false;
    if (g_state.pb_queue.front().type != SessionEventType::INPUT) return false;

    // Offline-replay gate (FM2K only for now).
    //
    // The .fm2krep file is sliced from MATCH_START → MATCH_END — its INPUTs
    // are battle-phase inputs, not CSS-traversal inputs. If we pop them
    // during the spectator's own CSS phase (driven by the auto-CSS hook's
    // direct memory writes rather than these INPUTs), they get applied to
    // the wrong logical frames and the input timeline misaligns with the
    // host's recording by the count of frames CSS took (~134 in practice).
    // Symptom: rounds may coincidentally match (BATTLE_INIT inputs are
    // mostly neutral), but mid-round positions/scripts are visibly off.
    //
    // Live-spec doesn't have this issue: host streams CSS-traversal inputs
    // from session start, so they consume during the spectator's CSS phase
    // as intended. Gate only fires when FM2K_REPLAY_FILE is set.
    //
    // Pre-battle: feed neutral inputs (p1=p2=0) so PGI+UG still runs and
    // the local CSS state machine advances under the auto-CSS hook's pins;
    // the pb_queue's first real INPUT stays at the head until the local
    // game crosses into mode==3000.
    if constexpr (FM2K::kIsFM2K) {
        static int s_offline_replay_cached = -1;
        if (s_offline_replay_cached < 0) {
            const char* v = std::getenv("FM2K_REPLAY_FILE");
            s_offline_replay_cached = (v && v[0]) ? 1 : 0;
        }
        if (s_offline_replay_cached == 1) {
            // Latch: gate is active only UNTIL the first mode==3000 entry.
            // The gate's purpose is to keep the queue's first INPUT at the
            // head until the local game crosses into battle so spectator's
            // bf=0 reads host's bf=0 input. Once we've entered battle once,
            // misalignment can't happen anymore — and post-match phases
            // (mode dropping back below 3000 for results / CSS rematch /
            // game-over screens) need queue inputs to drain so trailing
            // ROUND_END / MATCH_END / next match's MATCH_START events
            // can apply. Without this latch, the 6 post-R3 INPUTs in the
            // file's tail would block MATCH_END from ever applying and
            // the spec would freeze with q:7 in the queue.
            static bool s_battle_entered = false;
            const uint32_t mode = *(uint32_t*)FM2K::ADDR_GAME_MODE;
            if (mode >= 3000u) {
                s_battle_entered = true;
            }
            if (!s_battle_entered && mode < 3000u) {
                // Pre-battle: don't pop the queue. Synthesize a sentinel
                // input. Title (mode==1000) needs a confirm-button press
                // edge each frame to advance the menu — alternate
                // 0x010/0x000 so the edge detector fires repeatedly.
                // CSS (mode==2000) gets neutral — CssAutoConfirm pins
                // cursor + action_state directly.
                uint16_t synthetic = 0;
                if (mode == 1000u) {
                    static uint32_t s_title_tick = 0;
                    synthetic = (s_title_tick++ & 1u) ? 0x010u : 0u;
                }
                g_state.pb_current_p1 = synthetic;
                g_state.pb_current_p2 = synthetic;
                if (p1_input) *p1_input = synthetic;
                if (p2_input) *p2_input = synthetic;
                return true;
            }
        }
    }

    const SessionEvent ev = g_state.pb_queue.front();
    g_state.pb_queue.erase(g_state.pb_queue.begin());
    g_state.pb_current_p1 = ev.u.input.p1;
    g_state.pb_current_p2 = ev.u.input.p2;
    if (p1_input) *p1_input = ev.u.input.p1;
    if (p2_input) *p2_input = ev.u.input.p2;
    return true;
}

uint16_t SpectatorNode_GetCurrentP1Input() { return g_state.pb_current_p1; }
uint16_t SpectatorNode_GetCurrentP2Input() { return g_state.pb_current_p2; }
size_t   SpectatorNode_PendingFrameCount() { return g_state.pb_queue.size(); }

void SpectatorNode_ResetCurrentInputs() {
    g_state.pb_current_p1 = 0;
    g_state.pb_current_p2 = 0;
}

void SpectatorNode_LeaveUpstream() {
    if (!g_state.subscribed_upstream) return;
    CtrlPacket leave = {};
    leave.header.type = CtrlMsg::SPEC_LEAVE;
    ControlChannel_SendTo(leave, g_state.upstream_addr);
    g_state.subscribed_upstream = false;
}

void SpectatorNode_SetRootAddr(const sockaddr_in& root) {
    g_state.root_addr = root;
}

// Periodic health tick. Three jobs:
//   1. Heartbeat to current upstream every HEARTBEAT_INTERVAL_MS — lets
//      upstream's expiry sweep know we're still alive (otherwise it'd
//      drop us after SUBSCRIBER_EXPIRY_MS of silence).
//   2. Failover on silence: if no inbound from upstream for
//      SILENCE_FAILOVER_MS, assume upstream died. Send SPEC_LEAVE
//      (best-effort) to current upstream, then re-RequestJoin against
//      root_addr. Throttle reconnect attempts to once per RECONNECT_BACKOFF.
//   3. Upstream-side sweep: drop subscribers that haven't sent a
//      heartbeat in SUBSCRIBER_EXPIRY_MS (notify via SPEC_LEAVE so they
//      fail over instantly instead of waiting for their own silence
//      timer).
//
// Cheap to call every iter; the work is gated on the time deltas.
void SpectatorNode_TickHealth() {
    const uint64_t now = (uint64_t)GetTickCount64();

    // ---- Subscriber-side: heartbeat + silence failover ------------------
    if (g_state.subscribed_upstream) {
        if (now - g_state.last_heartbeat_send_ms >= SPECTATOR_HEARTBEAT_INTERVAL_MS) {
            CtrlPacket hb = {};
            hb.header.type = CtrlMsg::SPEC_HEARTBEAT;
            ControlChannel_SendTo(hb, g_state.upstream_addr);
            g_state.last_heartbeat_send_ms = now;
        }

        // (Removed: periodic INPUT_REQUEST poll — under TCP the kernel
        // handles retransmit transparently, and there's no host-side
        // RespondToInputRequest anymore.)

        // Silence-based failover. TCP receive activity is the only
        // liveness signal — the bulk stream lives entirely there.
        //
        // Suppressed when:
        //   (a) Catchup is active. The catchup loop runs sim+disk-IO at
        //       full speed and may legitimately go quiet for seconds —
        //       especially during CSS replay when each cursor move blocks
        //       on a .player file load. Tearing down here would force a
        //       backfill replay that re-incurs the same cost, never
        //       finishing.
        //   (b) pb_queue still has events to drain. recv_ms only updates
        //       on NEW TCP bytes; if the host already sent us 16k events
        //       of backfill in one burst and we're slowly chewing through
        //       them, recv_ms goes stale even though we're actively
        //       processing data. The previous gate (a alone) wasn't
        //       enough — once initial catchup ended (s_initial_catchup_done
        //       latched true), reconnect-and-backfill paths bypassed
        //       catchup entirely and the failover here would fire mid-
        //       drain → tear down the connection we're using → reconnect
        //       → re-receive backfill → same cycle. Death loop observed
        //       in P3 logs after a host network blip.
        extern bool g_spectator_catchup;
        const uint64_t recv_ms     = SpectatorTCP::LastUpstreamRecvMs();
        const bool     queue_idle  = SpectatorNode_PendingFrameCount() == 0;
        if (!g_spectator_catchup &&
            queue_idle &&
            recv_ms > 0 &&
            now - recv_ms >= SPECTATOR_SILENCE_FAILOVER_MS)
        {
            char buf[48] = {}; FormatAddr(g_state.upstream_addr, buf, sizeof(buf));
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "SpectatorNode: upstream %s silent for %llu ms — failing over",
                        buf,
                        (unsigned long long)(now - recv_ms));
            CtrlPacket leave = {};
            leave.header.type = CtrlMsg::SPEC_LEAVE;
            ControlChannel_SendTo(leave, g_state.upstream_addr);
            g_state.subscribed_upstream = false;
            SpectatorTCP::DisconnectUpstream();
        }
    }

    // Reconnect path: not subscribed, but we have a root we can fall
    // back to. Throttle so we don't spam JOIN_REQ.
    if (!g_state.subscribed_upstream &&
        g_state.root_addr.sin_port != 0 &&
        now - g_state.last_reconnect_attempt_ms >= SPECTATOR_RECONNECT_BACKOFF_MS)
    {
        char buf[48] = {}; FormatAddr(g_state.root_addr, buf, sizeof(buf));
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: reconnecting to root %s (mode=%s)", buf,
                    g_state.last_requested_mode == SpecJoinMode::CURRENT_MATCH
                        ? "CURRENT_MATCH" : "FULL_SESSION");
        g_state.last_reconnect_attempt_ms = now;
        SpectatorNode_RequestJoin(g_state.root_addr, g_state.last_requested_mode);
    }

    SpectatorNode_TickHostMaintenance();
}

void SpectatorNode_TickHostMaintenance() {
    const uint64_t now = (uint64_t)GetTickCount64();

    // ---- Spectator-incoming NAT punch poll --------------------------------
    // The launcher's hub-event handler (on_spectator_punch_target) writes
    // an external UDP addr into shared mem when the hub forwards a
    // spectator_incoming WS event. Poll spectator_punch_seq for changes;
    // each bump is a new spectator that needs us to fire an outbound
    // packet to open our NAT mapping for them. Without this their first
    // SPEC_JOIN_REQ gets dropped at our NAT and they sit on
    // "Connecting..." through every reconnect cycle.
    //
    // We send a small burst of SPEC_HEARTBEAT packets — harmless on the
    // spectator side (they're not subscribed yet, packets get logged +
    // dropped) but enough to traverse our NAT and create the inbound
    // hole. The spectator's existing 2-second reconnect will then
    // succeed on its next attempt.
    {
        FM2KSharedMemData* shm = GetSharedMemory();
        static uint32_t s_last_punch_seq = 0;
        if (shm && shm->magic == FM2K_SHARED_MEM_MAGIC &&
            shm->spectator_punch_seq != s_last_punch_seq) {
            s_last_punch_seq = shm->spectator_punch_seq;
            const uint32_t ip_be = shm->spectator_punch_ip_be;
            const uint16_t port  = shm->spectator_punch_port;
            if (ip_be != 0 && port != 0) {
                char ip_str[INET_ADDRSTRLEN] = {};
                inet_ntop(AF_INET, &ip_be, ip_str, sizeof(ip_str));
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: hub-coordinated NAT punch toward "
                    "spectator %s:%u (seq=%u)",
                    ip_str, (unsigned)port, (unsigned)s_last_punch_seq);

                sockaddr_in target{};
                target.sin_family      = AF_INET;
                target.sin_addr.s_addr = ip_be;
                target.sin_port        = htons(port);
                CtrlPacket hb{};
                hb.header.type = CtrlMsg::SPEC_HEARTBEAT;
                // 5-pack burst to ride out single-packet UDP loss; total
                // ~250 B at typical Ctrl size, negligible cost.
                for (int i = 0; i < 5; ++i) {
                    ControlChannel_SendTo(hb, target);
                }

                // TCP simultaneous-open punch (v0.2.35). UDP heartbeat
                // above only opens our NAT for inbound UDP; the
                // INPUT_BATCH stream rides TCP, which uses an entirely
                // separate NAT mapping. Without a TCP-side punch, spec's
                // TCP SYN to our listener gets dropped at our NAT and
                // they sit on "Connecting..." through every reconnect.
                //
                // Strategy: create a temporary raw TCP socket, set
                // SO_REUSEADDR so we can bind to the same port our
                // listener already holds, bind to that port, mark
                // non-blocking, and call connect() toward the spec's
                // external TCP addr. The connect almost certainly fails
                // (spec hasn't punched their side yet, or even if they
                // have the simultaneous-open negotiation usually doesn't
                // succeed in time) — that's fine. The point is the SYN
                // we send out registers an outbound flow in our NAT's
                // state table from listener_port -> spec_ext_ip:tcp_port.
                // When spec's connect SYN arrives at listener_port from
                // that exact remote endpoint, our NAT lets it through.
                // Listener accept() picks it up normally.
                //
                // Skip when spec_tcp_port == 0 (older spec client without
                // TCP-port reporting) — UDP-only path is what they had.
                const uint16_t tcp_port = shm->spectator_punch_tcp_port;
                const uint16_t our_listen = SpectatorTCP::GetListenPort();
                if (tcp_port != 0 && our_listen != 0) {
                    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
                    if (s != INVALID_SOCKET) {
                        BOOL reuse = TRUE;
                        ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR,
                                     (const char*)&reuse, sizeof(reuse));
                        sockaddr_in local{};
                        local.sin_family      = AF_INET;
                        local.sin_addr.s_addr = INADDR_ANY;
                        local.sin_port        = htons(our_listen);
                        if (::bind(s, (sockaddr*)&local, sizeof(local)) == 0) {
                            // Non-blocking so connect() returns
                            // immediately with WSAEWOULDBLOCK. We let
                            // the SYN actually leave the kernel before
                            // closing — see g_pending_punch_sockets
                            // below.
                            u_long nb = 1;
                            ::ioctlsocket(s, FIONBIO, &nb);
                            sockaddr_in dst{};
                            dst.sin_family      = AF_INET;
                            dst.sin_addr.s_addr = ip_be;
                            dst.sin_port        = htons(tcp_port);
                            ::connect(s, (sockaddr*)&dst, sizeof(dst));
                            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                "SpectatorNode: TCP punch %s:%u from local "
                                "port %u (listener) — SYN out, deferred close",
                                ip_str, (unsigned)tcp_port,
                                (unsigned)our_listen);
                            // Defer close. Closing immediately races the
                            // kernel's SYN emission (non-blocking
                            // connect just queues; close + linger=0
                            // would abort the unsent SYN). Stash with
                            // a 2-second cleanup deadline; the bottom
                            // of TickHostMaintenance sweeps expired
                            // entries on every tick. By then the SYN
                            // is long-gone and our NAT mapping is
                            // established for ~30 s.
                            g_pending_punch_sockets.push_back(
                                {s, now + 2000});
                        } else {
                            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                                "SpectatorNode: TCP punch bind to :%u failed "
                                "(WSA=%d) — punch skipped",
                                (unsigned)our_listen, WSAGetLastError());
                            ::closesocket(s);
                        }
                    } else {
                        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "SpectatorNode: TCP punch socket() failed "
                            "(WSA=%d)", WSAGetLastError());
                    }
                }
            }
        }
    }

    // ---- Upstream-side: bind newly-arrived TCP clients to subscribers ----
    // Spectator's async TCP dial completes some frames after JOIN_ACK; the
    // accept queue carries a fresh socket waiting to be paired by IP. On
    // first successful pair, ship INITIAL_MATCH + backfill — those bytes
    // were deferred at JOIN_REQ accept time because the socket didn't
    // exist yet.
    for (auto& sub : g_state.subscribers) {
        if (sub.tcp_bound) continue;
        if (SpectatorTCP::RegisterAcceptedClient(sub.addr)) {
            sub.tcp_bound = true;
            char buf[48] = {}; FormatAddr(sub.addr, buf, sizeof(buf));

            // C5 backfill ordering fence:
            //   1. Send the chosen backfill payload (EVENT_BATCH chunks
            //      and/or SNAPSHOT_BEGIN/CHUNK/END). Refreshes
            //      sub.last_seen_ms post-completion so the
            //      SUBSCRIBER_EXPIRY_MS sweep can't reap mid-backfill.
            //   2. MarkBackfillComplete flips the TCP-layer fence so
            //      future BroadcastToAll calls finally include this sub.
            // Until step 2 fires, BroadcastToAll skips this sub — any
            // live FlushBatch firing in this gap is silently elided and
            // the sub catches up via the backfill instead.

            // Phase 3 branch: CURRENT_MATCH-mode sub WITH a valid cached
            // snapshot → ship snapshot + tail events from snapshot's
            // anchor frame. Otherwise (FULL_SESSION, OR no snapshot yet
            // because this is the first match before its StashSnapshot
            // ran) fall back to legacy from-frame-0 backfill.
            const bool use_snapshot =
                sub.join_mode == SpecJoinMode::CURRENT_MATCH &&
                g_state.current_snapshot.valid;

            if (use_snapshot) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: TCP bound for %s — CURRENT_MATCH "
                    "(snapshot match=%u + tail from INPUT-frame=%u)",
                    buf, g_state.current_snapshot.match_index,
                    g_state.current_snapshot.input_frame);
                // Push current HOST_CONFIG over the UDP ctrl channel
                // BEFORE the snapshot. Live broadcasts only fire at
                // match-start moments (Netplay_StartBattle) — a spec
                // joining mid-match would otherwise run on whatever
                // stale settings the engine spawned with (wrong stage,
                // default SOCD, etc) until the next round-end.
                extern void Netplay_SendHostConfigToSpec(const sockaddr_in& to);
                Netplay_SendHostConfigToSpec(sub.addr);
                SendSnapshotTo(sub.addr);
                SendSessionBackfillFromFrame(sub.addr,
                    g_state.current_snapshot.input_frame);
            } else {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: TCP bound for %s — %s "
                    "(legacy from-frame-0 backfill)",
                    buf,
                    sub.join_mode == SpecJoinMode::CURRENT_MATCH
                        ? "CURRENT_MATCH requested but no snapshot yet"
                        : "FULL_SESSION");
                SendSessionBackfillTo(sub.addr);
            }

            sub.last_seen_ms = now;            // post-backfill liveness anchor
            SpectatorTCP::MarkBackfillComplete(sub.addr);
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "SpectatorNode: backfill complete for %s — live broadcasts engaged",
                        buf);
        }
    }

    // ---- Upstream-side: expire silent subscribers -----------------------
    for (auto it = g_state.subscribers.begin(); it != g_state.subscribers.end(); ) {
        if (now - it->last_seen_ms >= SPECTATOR_SUBSCRIBER_EXPIRY_MS) {
            char buf[48] = {}; FormatAddr(it->addr, buf, sizeof(buf));
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "SpectatorNode: subscriber %s silent — expiring + sending LEAVE",
                        buf);
            // Notify so they fail over fast instead of waiting their own timer.
            CtrlPacket leave = {};
            leave.header.type = CtrlMsg::SPEC_LEAVE;
            ControlChannel_SendTo(leave, it->addr);
            SpectatorTCP::DisconnectSubscriber(it->addr);
            it = g_state.subscribers.erase(it);
        } else {
            ++it;
        }
    }

    // ---- Sweep deferred TCP-punch sockets --------------------------------
    // 2 s after each punch the SYN has long since left the kernel and our
    // NAT mapping is established for the typical 30 s+ TCP-NAT TTL. Safe
    // to close. Linger=0 so Windows sends an RST instead of waiting in
    // FIN_WAIT (which we don't need — the spec's incoming SYN goes to
    // our LISTENER, not this transient connect socket).
    for (auto it = g_pending_punch_sockets.begin();
         it != g_pending_punch_sockets.end(); ) {
        if (now >= it->close_after_ms) {
            struct linger lng = { 1, 0 };
            ::setsockopt(it->handle, SOL_SOCKET, SO_LINGER,
                         (const char*)&lng, sizeof(lng));
            ::closesocket(it->handle);
            it = g_pending_punch_sockets.erase(it);
        } else {
            ++it;
        }
    }
}
