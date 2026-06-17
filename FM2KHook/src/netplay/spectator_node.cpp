#include "spectator_node.h"
#include "spec_wire.h"            // zero-RLE codec (SessionEvent_* live in spectator_node.h)
#include "spec_relay_queue.h"     // hub-relay outbound queue (Phase 2c)
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
#include <unordered_map>
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
    // Phase F: viewer advertised SPEC_JOIN_UDP_OK in its JOIN_REQ
    // reserved bits. Gates BOTH UDP_INPUT_BATCH datagrams and the
    // TCP-borne OP_BASELINE (an old build's framer drops the connection
    // on an unknown SpecDataType).
    bool         udp_ok = false;
    // Light re-join (SPEC_JOIN_RESUME): the viewer's next_expected_frame
    // from its JOIN_REQ. Non-zero = the viewer is mid-stream; the bind
    // path skips the snapshot and backfills exactly the gap from here.
    uint32_t     resume_frame = 0;
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
    // PerformTcpStun are skipped entirely; spec data flows out via the
    // shared-mem ring below instead.
    bool                      spec_transport_relay = false;
    // Outbound spec-relay ring (Phase 2c). Non-null only when relay mode
    // is active. Hook produces (Enqueue) when shipping spec data; the
    // launcher process (which opened the same kernel mapping by our
    // pid) drains and forwards each Slot as a WS binary frame to the
    // hub. Layout + ordering documented in spec_relay_queue.h.
    fm2k::spec_relay::Ring*   spec_relay_out       = nullptr;
    // Inbound spec-relay ring (Phase 3). Launcher writes WS binary
    // frames forwarded from hub here; hook drains in TickHealth and
    // dispatches each Slot's payload through the existing
    // SpectatorNode_HandleSpecData path (same handler the TCP-arrived
    // bytes feed). Only meaningful when this process is acting as a
    // spec; harmless empty ring otherwise.
    fm2k::spec_relay::Ring*   spec_relay_in        = nullptr;
    // Pending (ip:port) -> spec_user_id from spec_incoming events that
    // arrived before the matching SPEC_JOIN_REQ. Populated by the punch-
    // target poll in TickHostMaintenance; consumed (and erased) by
    // HandleJoinReq's new-subscriber branch which assigns the user_id
    // onto the Subscriber. Entries that never get a matching JOIN_REQ
    // age out passively when the next punch for the same addr
    // overwrites them.
    std::unordered_map<std::string, std::string> pending_spec_user_ids;

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

    // ─── HOST SIDE: UDP input accelerator (Phase F) ─────────────────────
    // Ring of the most recent confirmed (p1,p2) pairs, indexed by
    // session-relative INPUT-frame % SPEC_UDP_WINDOW. Maintained by
    // OnFrameConfirmed; read by SendUdpInputBatches every
    // SPEC_UDP_SEND_INTERVAL confirmed frames. total_op_count is the
    // running count of non-INPUT events appended (AppendOpAndFlush is
    // the single op choke point) -- shipped as op_seq in every datagram
    // so the spectator's admission gate can order inputs after ops.
    uint16_t                  udp_ring_p1[SPEC_UDP_WINDOW] = {};
    uint16_t                  udp_ring_p2[SPEC_UDP_WINDOW] = {};
    uint32_t                  total_op_count         = 0;
    // Redundant ops tail: the last few non-INPUT events, pre-encoded, so
    // UDP datagrams can deliver boundary ops when the TCP stream is dead
    // (this box's loopback kills established TCP connections at will --
    // both ends see "forcibly closed"; inputs already survive via the
    // accelerator, ops were the remaining hostage).
    struct OpWire {
        uint32_t op_index;
        uint32_t input_pos;   // total_input_count at append = inputs before this op
        uint8_t  len;
        uint8_t  bytes[100];
    };
    static constexpr size_t   OPS_RING = 8;
    OpWire                    udp_ops_ring[OPS_RING] = {};

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
    // Join-grace: false from JOIN until the first EVENT_BATCH applies.
    // The silence failover uses a 30s budget until then -- at real loss
    // rates the dial + 1MB snapshot + backfill exceed the steady-state
    // 5s budget and failover used to guillotine the join repeatedly.
    bool live_established = false;
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
    // RESET_INPUT_STATE / SOUND_INIT deferred the same way, but only when
    // they arrive during a match-boundary seam (pb_boundary != NONE). In
    // lockstep replay (FULL_SESSION from frame 0) the immediate apply is
    // correct -- the spec consumes the same frames the host did. During a
    // seam the spec runs EXTRA local frames (results screens + CSS pin
    // walk) between the op's queue position and its own battle entry;
    // applying RESET early would let those frames write back into the
    // freshly-zeroed input history ring, leaving garbage at slots the
    // host has as zeros (the task-#34 ring-shift failure class).
    bool                      pending_reset_input   = false;
    bool                      pending_sound_init    = false;

    // ─── VIEWER SIDE: UDP input accelerator (Phase F) ───────────────────
    // ops_seen mirrors the host's total_op_count: incremented for EVERY
    // non-INPUT event decoded from the current TCP connection (exactness
    // is what matters -- the gate compares it against op_seq in incoming
    // datagrams). udp_epoch_armed gates admission per-connection: cleared
    // on every (re)join so a stale stream can't poison the count, re-armed
    // when the host's OP_BASELINE for the new connection arrives.
    uint32_t                  ops_seen              = 0;
    // Per-TCP-connection op cursor: the stream delivers ops in global
    // order starting at the OP_BASELINE, so conn_ops_baseline +
    // conn_ops_decoded = the global index of the next op this connection
    // will deliver. Ops the UDP tail already accepted (index < ops_seen)
    // are skipped as duplicates.
    uint32_t                  conn_ops_baseline     = 0;
    uint32_t                  conn_ops_decoded      = 0;
    // TCP died but the subscription lives on: UDP (inputs + ops tail)
    // keeps feeding pb_queue while TickHealth re-JOINs in the background
    // to restore the TCP side. Cleared when the new connection's
    // OP_BASELINE lands.
    bool                      tcp_rejoin_pending    = false;
    // Last validated UDP datagram from the upstream -- the PRIMARY
    // liveness signal. With inputs and ops riding UDP, a quiet TCP
    // means nothing while datagrams flow.
    uint64_t                  last_udp_recv_ms      = 0;
    // Viewer chose natural boot (pre-battle JOIN_ACK aborted /F): it
    // replays the session from frame 0 and can NEVER accept a snapshot
    // (battle-captured state into a title/CSS engine = phase-wait
    // deadlock: pending_apply blocks pops, pops are the only road to
    // the apply gate).
    bool                      natural_boot          = false;
    // Broadcast delay bank built once at CSS open: the mirror start is
    // held until the queue holds the full bank, so playback never rides
    // the live edge (riding the edge = every arrival gap stalls the
    // picture). Players are browsing during the hold -- invisible.
    bool                      pb_bank_built         = false;
    // True between MATCH_START apply and MATCH_END apply. When the local
    // game reaches CSS while this is still set, the local sim has outrun
    // the stream's results tail -- those queued inputs belong to results
    // screens we already finished and MUST NOT feed the fresh CSS (8
    // leaked frames displaced the cursors before the seam engaged; the
    // whole mirrored dance then ran offset = wrong chars AND colors at
    // the rematch, 2026-06-11 15:09).
    bool                      pb_awaiting_match_end = false;
    // True once the LOCAL game has been in battle mode (>=3000) since the
    // last MATCH_START apply. Distinguishes the rematch boundary (local
    // played the battle, returned to CSS early -> queued inputs are the
    // results tail, discard) from match ENTRY (local still at CSS while
    // the stream just started the battle -> queued inputs ARE the battle,
    // keep). Without it the results-tail guard ate the entire offline
    // replay at boot: .fm2krep starts with MATCH_START, the pin walk
    // reaches mode 2000, and all 3616 battle INPUTs were discarded as a
    // "results tail" (2026-06-11 16:24, replay parity 0 rows).
    bool                      pb_local_battle_seen  = false;
    bool                      udp_epoch_armed       = false;
    // Post-CSS-open confirm-mask countdown (lean seam): pops remaining
    // during which CssAutoConfirm's hold eats confirm bits, so the edge
    // echo of the last pre-CSS input can't lock the carried cursors.
    uint32_t                  pb_post_css_mask_pops = 0;
    // Seam mirror split marker: set when the seam stream's CSS_ENTERED
    // op applies. Before it, seam INPUTs are the host's results-screen
    // presses (discarded -- the viewer advances its own results on
    // synthetic edges); after it, seam INPUTs are the host's CSS dance
    // and mirror 1:1 from CSS frame 0 on both sides.
    bool                      pb_css_marker_seen = false;
    // True once a snapshot has applied this playback session. The
    // rewind-discard guard in ApplyPendingSnapshot is strictly for
    // RE-JOIN re-ships; the FIRST snapshot must always apply (the BTB
    // boot's local battle state is garbage until it does).
    bool                      pb_snapshot_applied_once = false;
    // True once the viewer has popped its first INPUT this session --
    // the real "we are mid-stream" signal. Snapshot applies must be
    // refused from then on: a naturally-joined viewer has
    // pb_snapshot_applied_once == false, so a re-join's re-shipped
    // snapshot passed the old guard as a "first apply" and rewound the
    // live sim to the match anchor (battle restarted, 2026-06-11).
    bool                      pb_started               = false;
    // Highest op_seq announced by any received UDP datagram. Drives the
    // silent-TCP-death detector in TickHealth: a persistent gap vs
    // ops_seen while TCP is quiet means the op stream is wedged.
    uint32_t                  udp_highest_op_seq    = 0;
    // [SPEC-UDP] diagnostics (rate-limited 1Hz log).
    uint32_t                  udp_admitted_total    = 0;
    uint32_t                  udp_paused_on_gate    = 0;
    uint32_t                  udp_dropped_on_gap    = 0;

    // C7 — host's session_id for this peer connection. Generated lazily
    // (first AppendSessionId call) and stays stable until the SpectatorNode
    // is shut down or the next AppendSessionId overwrites it. The wire
    // event sources g_state.session_events; the file writer sources this
    // field; spectator-side application copies the wire payload here so
    // a spectator's own .fm2kset/.fm2krep recordings carry the same id
    // as the upstream's.
    uint64_t                  session_id           = 0;

    // Match-boundary playback state (A4 wrong-characters fix). Raw replay
    // of the host's seam frames (results screens + CSS traversal) cannot
    // align on the spectator: the spec's own 3000->2000 transition timing
    // is driven by ITS local result-screen processing, and a CURRENT_MATCH
    // joiner's CSS init state never matched the host's to begin with. So
    // a spectator that replayed CSS inputs raw locked whatever its
    // diverged cursor walk landed on -- match 2 started with the wrong
    // characters (observed 2026-06-11, A4 multi-match run). The robust
    // pattern is the one offline replay already uses: characters come
    // from MATCH_START + CssAutoConfirm, never from CSS input replay.
    //   NONE    -- normal battle playback; INPUT pops feed the sim 1:1.
    //   SEAM    -- between MATCH_END apply and MATCH_START apply: INPUT
    //              events at the queue head are consumed-and-DISCARDED
    //              (host seam frames are presentation-only), ops apply
    //              as they reach head, and the local sim runs on
    //              synthetic inputs (confirm-edge to leave the results
    //              screen, neutral at CSS).
    //   PINNING -- MATCH_START applied (CssAutoConfirm armed with the
    //              new picks): battle INPUTs stay AT the head while the
    //              local game walks CSS under the pin; pops resume the
    //              moment game_mode crosses 3000 so spec battle frame 0
    //              consumes host battle frame 0's input.
    enum class PbBoundary : uint8_t { NONE, SEAM, PINNING };
    PbBoundary                pb_boundary          = PbBoundary::NONE;
    // PINNING release requires a mode EDGE, not a level: the local game
    // must first LEAVE battle (results screen is still mode 3000 -- with
    // the UDP accelerator MATCH_START arrives ~2s after MATCH_END, long
    // before the local game walks off the old match's results screen) and
    // only then re-enter >= 3000. Releasing on the stale 3000 fed the new
    // match's inputs into the old results screen and let CssAutoConfirm
    // disengage before CSS even opened (wrong characters at UDP speed).
    bool                      pb_boundary_left_battle = false;
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

// FormatAddr is defined below; forward-decl so the Phase 2c send
// helpers can use it for their warning logs without reordering the
// whole helper block.
void FormatAddr(const sockaddr_in& a, char* out, size_t out_sz);

// Spec data send helpers (Phase 2c). Branch on the active spec
// transport: TCP (legacy P2P) or relay (hub-mediated WS binary).
//
// `buf` carries the full TCP-wire payload: SpecDataHeader (10 B) +
// inner payload. In TCP mode it's blasted to the socket(s) as-is. In
// relay mode it becomes the SPDB envelope's `payload` (the launcher
// drain wraps it with magic + routing fields before WS send).
//
// Failure modes:
//   - relay mode with no spec_relay_out (mapping creation failed):
//     silently drop. Logged once at Init.
//   - relay mode SendTo with no spec_user_id on the matching sub:
//     drop with warn log. Means hub didn't forward user_id (older
//     hub) or this sub came in via a pre-Phase-2c spec_incoming.
//   - ring full: counted in Ring.total_dropped, no inline log.
void OutboundBroadcast(const void* buf, size_t len) {
    if (g_state.spec_transport_relay) {
        if (!g_state.spec_relay_out) return;
        // Per-bound-sub direct send rather than hub-side broadcast.
        // Why: the TCP path's BroadcastToAll has a backfill_done fence
        // (g_subs[i].backfill_done gate at spectator_tcp.cpp:285) that
        // suppresses live events to a sub until snapshot+backfill have
        // landed. The hub doesn't know about backfill_done state, so a
        // TARGET_BROADCAST would race pre-bind specs into receiving
        // EVENT_BATCH frames AHEAD of their snapshot -- bad ordering,
        // spec applies events on uninitialized state.
        //
        // Iterating bound subs and Enqueuing TARGET_DIRECT per spec
        // exactly mirrors the TCP path's gate. Pre-bind specs (no
        // tcp_bound or no spec_user_id yet) get skipped here; they'll
        // catch up via snapshot + backfill in TickHostMaintenance.
        //
        // Cost: O(N) Enqueue calls instead of one. Hub does the same
        // O(N) sends either way, so net throughput is identical;
        // ring slot consumption goes up by N -- factored into the
        // 128-slot capacity sizing.
        for (const auto& sub : g_state.subscribers) {
            if (!sub.tcp_bound) continue;
            if (sub.spec_user_id.empty()) continue;
            fm2k::spec_relay::Enqueue(
                g_state.spec_relay_out,
                fm2k::spec_relay::TARGET_DIRECT,
                sub.spec_user_id.c_str(),
                /*spec_data_type=*/0,
                /*frame_count=*/0,
                /*spec_data_flags=*/0,
                buf, static_cast<uint32_t>(len));
        }
        return;
    }
    SpectatorTCP::BroadcastToAll(buf, len);
}

void OutboundSendTo(const sockaddr_in& to, const void* buf, size_t len) {
    if (g_state.spec_transport_relay) {
        if (!g_state.spec_relay_out) return;
        // Look up spec_user_id from the addr. Subscriber list is short
        // (capacity-bounded; SPECTATOR_DEFAULT_CAPACITY = 4 today), so
        // linear scan is fine.
        const char* spec_uid = nullptr;
        for (const auto& sub : g_state.subscribers) {
            if (sub.addr.sin_family == to.sin_family &&
                sub.addr.sin_port   == to.sin_port &&
                sub.addr.sin_addr.s_addr == to.sin_addr.s_addr) {
                if (!sub.spec_user_id.empty()) spec_uid = sub.spec_user_id.c_str();
                break;
            }
        }
        if (!spec_uid) {
            // No user_id available; can't route through relay. Log
            // rarely so a misconfigured pair doesn't spam.
            static uint64_t s_warn_count = 0;
            if ((s_warn_count++ % 64) == 0) {
                char addr_buf[48] = {};
                FormatAddr(to, addr_buf, sizeof(addr_buf));
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: relay SendTo %s -- no spec_user_id "
                    "on Subscriber, dropping (warns:%llu)",
                    addr_buf, (unsigned long long)s_warn_count);
            }
            return;
        }
        fm2k::spec_relay::Enqueue(
            g_state.spec_relay_out,
            fm2k::spec_relay::TARGET_DIRECT,
            spec_uid,
            /*spec_data_type=*/0,
            /*frame_count=*/0,
            /*spec_data_flags=*/0,
            buf, static_cast<uint32_t>(len));
        return;
    }
    SpectatorTCP::SendTo(to, buf, len);
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
        case SessionEventType::CSS_ENTERED:
            w = SessionEvent_EncodeCssEntered(buf, sizeof(buf));
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

    OutboundBroadcast(buf.data(), buf.size());
}

// ─── Phase F: UDP input accelerator (host side) ──────────────────────────

// Kill switch: FM2K_SPEC_UDP=0 disables both the host sender and the
// viewer admission. Default ON.
bool SpecUdpEnabled() {
    static int s_state = -1;
    if (s_state < 0) {
        const char* v = std::getenv("FM2K_SPEC_UDP");
        s_state = (v && v[0] == '0' && v[1] == '\0') ? 0 : 1;
    }
    return s_state != 0;
}

// Broadcast the last-K-confirmed-inputs window as a raw datagram to every
// direct subscriber that advertised SPEC_JOIN_UDP_OK. Pure accelerator:
// the same inputs also flow via the TCP EVENT_BATCH stream; the viewer's
// positional dedup makes double delivery free. See spectator_node.h
// (UDP_INPUT_BATCH) for the wire layout and admission invariant.
void SendUdpInputBatches() {
    if (!SpecUdpEnabled()) return;
    if (g_state.spec_transport_relay) return;   // no direct UDP addressing
    if (g_state.subscribers.empty()) return;
    const uint32_t total = g_state.total_input_count;
    if (total == 0) return;

    const uint32_t window = (uint32_t)std::min<uint64_t>(SPEC_UDP_WINDOW, total);
    const uint32_t start  = total - window;

    // Per-op tail overhead is 9 bytes (u32 op_index + u32 input_pos +
    // u8 len) -- the original 5-byte budget undersized this array and the
    // emitter wrote up to 32 bytes past it once the ring held 8 ops
    // (exactly at battle entry, when MATCH_START joins the ring): silent
    // host-side stack corruption, observed as the UDP feed going dark at
    // 14:57:58 2026-06-11.
    uint8_t buf[sizeof(SpecDataHeader) + 4 + SPEC_UDP_WINDOW * 4
                + 1 + State::OPS_RING * (9 + sizeof(State::OpWire::bytes))];
    SpecDataHeader hdr = {};
    hdr.magic       = SPEC_DATA_MAGIC;
    hdr.type        = SpecDataType::UDP_INPUT_BATCH;
    hdr.start_frame = start;
    hdr.frame_count = (uint16_t)window;
    std::memcpy(buf, &hdr, sizeof(hdr));
    const uint32_t op_seq = g_state.total_op_count;
    std::memcpy(buf + sizeof(hdr), &op_seq, 4);
    uint8_t* w = buf + sizeof(hdr) + 4;
    for (uint32_t f = start; f < total; ++f) {
        const size_t slot = f % SPEC_UDP_WINDOW;
        std::memcpy(w, &g_state.udp_ring_p1[slot], 2); w += 2;
        std::memcpy(w, &g_state.udp_ring_p2[slot], 2); w += 2;
    }
    // Redundant ops tail: [u8 count] then per op [u32 op_index][u8 len]
    // [len bytes]. The last few ops re-ship in every datagram; the
    // receiver accepts them strictly in order (op_index == ops_seen), so
    // boundary clusters survive a dead TCP stream.
    {
        const uint32_t total_ops = g_state.total_op_count;
        const uint32_t n = (uint32_t)std::min<uint64_t>(State::OPS_RING, total_ops);
        uint8_t* count_at = w; *w++ = 0;
        uint8_t emitted = 0;
        for (uint32_t i = total_ops - n; i < total_ops; ++i) {
            const auto& slot = g_state.udp_ops_ring[i % State::OPS_RING];
            if (slot.len == 0 || slot.op_index != i) continue;  // overwritten
            // MTU guard: never let the datagram exceed ~1400B -- a
            // fragmented datagram under loss dies as a unit.
            if ((size_t)(w - buf) + 9 + slot.len > 1400) break;
            std::memcpy(w, &slot.op_index, 4); w += 4;
            std::memcpy(w, &slot.input_pos, 4); w += 4;
            *w++ = slot.len;
            std::memcpy(w, slot.bytes, slot.len); w += slot.len;
            ++emitted;
        }
        *count_at = emitted;
    }
    const size_t len = (size_t)(w - buf);
    hdr.flags = (uint16_t)(len - sizeof(SpecDataHeader));
    std::memcpy(buf, &hdr, sizeof(hdr));  // re-stamp with final flags

    size_t fanned = 0;
    for (const auto& sub : g_state.subscribers) {
        if (!sub.udp_ok || !sub.tcp_bound) continue;
        ControlChannel_SendRawTo(buf, len, sub.addr);
        if (++fanned >= SPEC_UDP_MAX_FANOUT) break;
    }
}

// TCP-borne op-count baseline for a freshly-bound subscriber. Must ship
// BEFORE the snapshot/backfill on the same connection so the viewer's
// ops_seen starts exact for the events this connection will deliver.
void SendOpBaselineTo(const sockaddr_in& to, uint32_t baseline) {
    uint8_t buf[sizeof(SpecDataHeader) + 4];
    SpecDataHeader hdr = {};
    hdr.magic       = SPEC_DATA_MAGIC;
    hdr.type        = SpecDataType::OP_BASELINE;
    hdr.start_frame = 0;
    hdr.frame_count = 0;
    hdr.flags       = 4;
    std::memcpy(buf, &hdr, sizeof(hdr));
    std::memcpy(buf + sizeof(hdr), &baseline, 4);
    OutboundSendTo(to, buf, sizeof(buf));
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
    // Clamp the backfill at the GLOBAL live-flush cursor, not the vector
    // tip. Live EVENT_BATCH broadcasts resume from last_flushed_event_idx
    // for every fenced subscriber; shipping [cursor..tip) here too sent
    // those events TWICE to a fresh joiner (backfill + next FlushBatch),
    // shifting its input stream at the handoff -- deep mid-battle join
    // diverged at exactly the backfill->live boundary (k=3474,
    // 2026-06-11). The clamped tail reaches the sub via the very next
    // regular FlushBatch instead.
    const size_t total_events =
        (g_state.last_flushed_event_idx < g_state.session_events.size())
            ? g_state.last_flushed_event_idx
            : g_state.session_events.size();
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
                case SessionEventType::CSS_ENTERED:
                    one_w = SessionEvent_EncodeCssEntered(one, sizeof(one));
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

        OutboundSendTo(to, buf.data(), buf.size());

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
// Hoisted index computation: the first session_events index a backfill
// anchored at `anchor_input_frame` would ship (the first INPUT at-or-after
// the anchor, backed up over its preceding non-INPUT op group). Returns
// session_events.size() when nothing is at-or-after the anchor. Shared by
// SendSessionBackfillFromFrame and the OP_BASELINE computation in the bind
// path so both always agree on where the connection's delivery starts.
size_t BackfillFirstIdxForFrame(uint32_t anchor_input_frame) {
    const size_t total = g_state.session_events.size();
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
    if (first_idx >= total) return total;

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
    return first_idx;
}

void SendSessionBackfillFromFrame(const sockaddr_in& to,
                                  uint32_t anchor_input_frame) {
    const size_t total = g_state.session_events.size();
    if (total == 0) return;

    const size_t first_idx = BackfillFirstIdxForFrame(anchor_input_frame);
    if (first_idx >= total) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "SpectatorNode: no events at-or-after INPUT-frame=%u "
            "(total INPUTs=%u, snapshot is at the live edge)",
            anchor_input_frame, g_state.total_input_count);
        return;
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
        meta.flags              = 0;
        meta.compressed_bytes   = (uint32_t)cache.blob.size();
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
        // Zero-RLE the blob; ship compressed when it actually wins.
        static std::vector<uint8_t> s_wire;   // reused scratch
        ZeroRleCompress(cache.blob, s_wire);
        const bool use_rle = s_wire.size() < cache.blob.size();
        const std::vector<uint8_t>& wire = use_rle ? s_wire : cache.blob;
        if (use_rle) {
            meta.flags            |= SNAPSHOT_FLAG_ZERO_RLE;
            meta.compressed_bytes  = (uint32_t)s_wire.size();
        }
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "SpectatorNode: snapshot wire size %zu bytes (%s, raw %zu)",
            wire.size(), use_rle ? "zero-RLE" : "uncompressed",
            cache.blob.size());
        std::memcpy(buf.data(), &hdr, sizeof(hdr));
        std::memcpy(buf.data() + sizeof(hdr), &meta, sizeof(meta));
        OutboundSendTo(to, buf.data(), buf.size());

    // ---- SNAPSHOT_CHUNK xN -------------------------------------------
    size_t emitted = 0;
    while (emitted < wire.size()) {
        const size_t remaining = wire.size() - emitted;
        const size_t chunk_n   = std::min(remaining, SPECTATOR_SNAPSHOT_CHUNK_BYTES);

        std::vector<uint8_t> cbuf(sizeof(SpecDataHeader) + chunk_n);
        SpecDataHeader chdr = {};
        chdr.magic       = SPEC_DATA_MAGIC;
        chdr.type        = SpecDataType::SNAPSHOT_CHUNK;
        chdr.start_frame = (uint32_t)emitted;       // byte offset (running)
        chdr.frame_count = 0;
        chdr.flags       = (uint16_t)chunk_n;
        std::memcpy(cbuf.data(), &chdr, sizeof(chdr));
        std::memcpy(cbuf.data() + sizeof(chdr),
                    wire.data() + emitted, chunk_n);
        OutboundSendTo(to, cbuf.data(), cbuf.size());

        emitted += chunk_n;
    }
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
        OutboundSendTo(to, buf.data(), buf.size());
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
            "listener + TCP-STUN, creating shared-mem queues for "
            "launcher <-> hub forwarding");
        // Create BOTH outbound (hook->launcher) and inbound
        // (launcher->hook) rings unconditionally. Either could be used
        // depending on whether this process ends up acting as host or
        // spec for a given match. ~2 MB of shared mem total per process
        // -- cheap, and avoids late-bound role-detection logic.
        g_state.spec_relay_out = fm2k::spec_relay::CreateOutboundHere();
        g_state.spec_relay_in  = fm2k::spec_relay::CreateInboundHere();
        if (!g_state.spec_relay_out || !g_state.spec_relay_in) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "SpectatorNode: spec_relay mapping(s) failed (out=%s in=%s); "
                "spec data plane degraded",
                g_state.spec_relay_out ? "ok" : "fail",
                g_state.spec_relay_in  ? "ok" : "fail");
        }
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "SpectatorNode: Init (capacity=%zu, batch=%zu frames, "
            "transport=relay, out=%s, in=%s)",
            g_state.capacity, BROADCAST_BATCH_FRAMES,
            g_state.spec_relay_out ? "ok" : "failed",
            g_state.spec_relay_in  ? "ok" : "failed");
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
    g_state.total_op_count         = 0;
    g_state.ops_seen               = 0;
    g_state.udp_epoch_armed        = false;
    g_state.udp_highest_op_seq     = 0;
    g_state.udp_admitted_total     = 0;
    g_state.udp_paused_on_gate     = 0;
    g_state.udp_dropped_on_gap     = 0;
    g_state.broadcasting = false;
    g_state.subscribed_upstream = false;
    g_state.playing_back = false;
    g_state.pb_boundary         = State::PbBoundary::NONE;
    g_state.pending_reset_input = false;
    g_state.pending_sound_init  = false;
    g_state.pb_snapshot_applied_once = false;
    g_state.pb_started               = false;
    CssAutoConfirm_SetSeamHold(false);
    SpectatorTCP::Shutdown();
    // Tear down both relay rings if we created them. Close handles
    // nullptr. Kernel mapping refcount keeps the object alive while
    // launcher still has it mapped; we just drop our side.
    if (g_state.spec_relay_out) {
        fm2k::spec_relay::Close(g_state.spec_relay_out);
        g_state.spec_relay_out = nullptr;
    }
    if (g_state.spec_relay_in) {
        fm2k::spec_relay::Close(g_state.spec_relay_in);
        g_state.spec_relay_in = nullptr;
    }
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
    // Phase F: mirror into the UDP accelerator ring, keyed by this input's
    // session-relative frame index (= total_input_count pre-increment).
    g_state.udp_ring_p1[g_state.total_input_count % SPEC_UDP_WINDOW] = p1_input;
    g_state.udp_ring_p2[g_state.total_input_count % SPEC_UDP_WINDOW] = p2_input;
    ++g_state.total_input_count;

    // Live broadcast batching window — only fan out to existing subscribers.
    // Cadence trigger: every BROADCAST_BATCH_FRAMES new INPUT events.
    const uint32_t pending_inputs =
        g_state.total_input_count - g_state.flushed_input_count;
    if (pending_inputs >= BROADCAST_BATCH_FRAMES) {
        FlushBatch();
    }

    // Phase F: redundant UDP window every SPEC_UDP_SEND_INTERVAL confirmed
    // frames. Internally no-ops when disabled / relay-mode / no udp_ok subs.
    if ((g_state.total_input_count % SPEC_UDP_SEND_INTERVAL) == 0) {
        SendUdpInputBatches();
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
    // Phase F: single choke point for non-INPUT appends -- the running op
    // count ships as op_seq in UDP_INPUT_BATCH so viewers can order
    // inputs after ops (see admission invariant in spectator_node.h).
    // Pre-encode into the redundant ops ring for the datagram tail.
    {
        std::vector<uint8_t> w;
        AppendEventToWire(w, ev, g_state.match_headers);
        if (!w.empty() && w.size() <= sizeof(State::OpWire::bytes)) {
            auto& slot = g_state.udp_ops_ring[g_state.total_op_count % State::OPS_RING];
            slot.op_index  = g_state.total_op_count;
            slot.input_pos = g_state.total_input_count;
            slot.len       = (uint8_t)w.size();
            std::memcpy(slot.bytes, w.data(), w.size());
        }
    }
    ++g_state.total_op_count;
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

void SpectatorNode_AppendCssEntered() {
    SessionEvent ev{};
    ev.type = SessionEventType::CSS_ENTERED;
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

static CtrlPacket BuildJoinAckPacket();  // defined with HandleJoinReq below

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

    // Session-kind-change re-broadcast: battle just started, so the
    // chars/stage in a JOIN_ACK are now real. Viewers that joined during
    // CSS hold their /F boot until this arrives (their first ACK said
    // kind=CSS with no chars); HandleJoinAck's kind==BATTLE path seeds
    // their runtime BTB overrides. Idempotent for already-running
    // viewers (their dial guard skips reconnecting).
    if (!g_state.subscribers.empty()) {
        const CtrlPacket ack = BuildJoinAckPacket();
        for (const auto& sub : g_state.subscribers) {
            ControlChannel_SendTo(ack, sub.addr);
        }
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "SpectatorNode: battle-entry JOIN_ACK re-broadcast to %zu sub(s)",
            g_state.subscribers.size());
    }
}

bool SpectatorNode_HasSnapshot() {
    return g_state.current_snapshot.valid;
}

namespace { void ApplyResetInputState(); }  // defined with ApplySessionEvent below

void SpectatorNode_ApplyPendingPinRng() {
    // Seam-deferred battle-init ops first (match-boundary path only; see
    // pending_reset_input). Order mirrors the host's StartBattle: input
    // state reset + sound layer init, then the RNG pin last so nothing
    // can disturb the seed before frame 0's PGI.
    if (g_state.pending_reset_input) {
        ApplyResetInputState();
        g_state.pending_reset_input = false;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "SpectatorNode: applied deferred RESET_INPUT_STATE at battle entry");
    }
    if (g_state.pending_sound_init) {
        SoundRollback::Init();
        g_state.pending_sound_init = false;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "SpectatorNode: applied deferred SOUND_INIT at battle entry");
    }
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
    // Re-join discard runs BEFORE the phase wait: a snapshot we will
    // never apply must not sit in pending_apply waiting for a phase
    // the held pops can never reach. (Captured-at-3000 snapshot +
    // viewer at CSS = circular deadlock: apply waits for mode 3000,
    // mode 3000 needs pops, pops wait on pending_apply. Froze the
    // viewer at q=395 with the stream healthy, 2026-06-11.)
    // Re-join guard (Phase F): a TCP-death re-JOIN makes the host re-ship
    // its cached snapshot. If our sim has already CONSUMED past the
    // snapshot's anchor, loading it would rewind the engine to the anchor
    // while the queue/cursor stay at the live edge -- corrupted playback.
    // Consumption position = receipt cursor minus the INPUTs still queued
    // (highest_consumed_frame is vestigial -- never updated). Strict >
    // so a fresh join (consumed == anchor, nothing popped yet) applies,
    // and a forward jump (anchor ahead of us, e.g. re-join landing in the
    // NEXT match) also applies.
    if (g_state.pb_snapshot_applied_once || g_state.pb_started ||
        g_state.natural_boot) {
        // Re-join snapshots NEVER re-apply. Backward anchors would rewind
        // the sim under a live-edge queue; forward anchors (re-join lands
        // in a LATER match) would overwrite the char slots with the new
        // match's dynamic data while the locally loaded .player files are
        // still the old characters -- the sprite renderer reads mismatched
        // image descriptors and AVs (observed 2026-06-11, 0x40CD47, 100ms
        // after a fast-forward apply). Proper forward-jump support needs a
        // re-BTB (reload files for the announced chars) -- future work.
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "SpectatorNode: discarding re-join snapshot (anchor=%u, "
            "already initialized this session) -- continuing on the "
            "event stream",
            inbox.anchor_frame);
        inbox = State::SnapshotInbox{};
        return;
    }

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
    g_state.pb_snapshot_applied_once = true;
    // Snapshot supersedes any in-flight boundary state (e.g. a stale SEAM
    // from a renegotiated join) -- the restored state IS the new baseline.
    g_state.pb_boundary         = State::PbBoundary::NONE;
    g_state.pending_reset_input = false;
    g_state.pending_sound_init  = false;
    CssAutoConfirm_SetSeamHold(false);

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
    g_state.pb_boundary         = State::PbBoundary::NONE;
    g_state.pending_reset_input = false;
    g_state.pending_sound_init  = false;
    CssAutoConfirm_SetSeamHold(false);
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
            if (g_state.pb_boundary != State::PbBoundary::NONE) {
                // Seam: defer to battle entry (see pending_reset_input).
                g_state.pending_reset_input = true;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: queued RESET_INPUT_STATE (seam -- will "
                    "apply at battle entry)");
            } else {
                ApplyResetInputState();
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: applied RESET_INPUT_STATE");
            }
            break;
        case SessionEventType::CSS_ENTERED:
            g_state.pb_css_marker_seen = true;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "SpectatorNode: applied CSS_ENTERED (seam mirror split)");
            break;
        case SessionEventType::SOUND_INIT:
            if (g_state.pb_boundary != State::PbBoundary::NONE) {
                g_state.pending_sound_init = true;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: queued SOUND_INIT (seam -- will apply "
                    "at battle entry)");
            } else {
                SoundRollback::Init();
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: applied SOUND_INIT");
            }
            break;
        case SessionEventType::MATCH_START: {
            g_state.pb_awaiting_match_end = true;
            g_state.pb_local_battle_seen  = false;
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
            // Arm the CSS auto-lock-and-confirm hook so the local game pins
            // to the announced chars/stage when CSS opens.
            //   - Offline replay (FM2K_REPLAY_FILE): always -- the file's
            //     INPUTs are battle-phase only, CSS must be driven by pins.
            //   - Live spectator at a match boundary (pb_boundary==SEAM):
            //     same reasoning. The old assumption ("live-spec walks CSS
            //     via the upstream input log") only holds for FULL_SESSION
            //     specs on their FIRST CSS, where the fresh boot matches
            //     the host's initial CSS state. At match 2+ the seam can't
            //     align (see PbBoundary), so picks must come from this
            //     header -- raw CSS replay locked the wrong characters.
            {
                static int s_offline_replay_cached = -1;
                if (s_offline_replay_cached < 0) {
                    const char* v = std::getenv("FM2K_REPLAY_FILE");
                    s_offline_replay_cached = (v && v[0]) ? 1 : 0;
                }
                if (s_offline_replay_cached == 1 ||
                    g_state.pb_boundary == State::PbBoundary::SEAM) {
                    CssAutoConfirm_OnReplayMatchStart(
                        g_state.pb_p1_char, g_state.pb_p1_color,
                        g_state.pb_p2_char, g_state.pb_p2_color,
                        g_state.pb_stage_id);
                }
                if (g_state.pb_boundary == State::PbBoundary::SEAM) {
                    g_state.pb_boundary = State::PbBoundary::PINNING;
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "SpectatorNode: boundary SEAM -> PINNING (holding "
                        "battle inputs until local game_mode reaches 3000)");
                }
            }
            break;
        }
        case SessionEventType::MATCH_END: {
            // Don't clear pb_queue — let queued post-MATCH_END frames drain
            // (they render the final battle frames). The next MATCH_START
            // resets metadata and flips playing_back back on.
            g_state.playing_back = false;
            // Enter the seam: from here until the next MATCH_START, INPUT
            // events are host results/CSS frames -- presentation-only and
            // structurally misaligned with the spec's own seam timing.
            // PopFrameInputs discards them and feeds synthetic inputs; the
            // next MATCH_START re-arms character pinning (see PbBoundary).
            //
            // Offline replay keeps the legacy path: single-match .fm2krep
            // files have no following MATCH_START, so a SEAM would feed
            // synthetic inputs forever and block the queue-drained
            // ExitProcess (observed: replay-phase instance stuck at its
            // results screen after the Phase F boundary rework).
            {
                static int s_live_spec = -1;
                if (s_live_spec < 0) {
                    const char* rp = std::getenv("FM2K_REPLAY_FILE");
                    s_live_spec = (rp && rp[0]) ? 0 : 1;
                }
                if (s_live_spec == 1) {
                    // LEAN seam: pure 1:1 replay through the boundary
                    // with exactly two thin protections --
                    //  (a) discard-until-CSS_ENTERED once the local CSS
                    //      opens, so the mirror starts at the host's CSS
                    //      frame 0 even when the two results screens'
                    //      frame counts drift by a few frames;
                    //  (b) a short confirm mask at CSS open, because CSS
                    //      init clears the input-edge state and the first
                    //      consumed input (battle-tail attack bits)
                    //      otherwise registers as a rising confirm for
                    //      both players at their carried cursors (locked
                    //      7/24 five seconds before the host confirmed
                    //      17/6, 2026-06-11).
                    // No pinning, no synthetic CSS walk, no forced locks:
                    // the players' real confirms drive everything.
                    g_state.pb_boundary = State::PbBoundary::SEAM;
                    g_state.pb_css_marker_seen = false;
                }
            }
            const auto& p = ev.u.match_end;
            const char* who = (p.winner_idx == 0) ? "P1"
                            : (p.winner_idx == 1) ? "P2" : "DRAW";
            g_state.pb_awaiting_match_end = false;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "SpectatorNode: applied MATCH_END winner=%s rounds=%u-%u "
                "frames=%u (queued=%zu) -- boundary SEAM entered",
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

// Build SPEC_JOIN_ACK with the host's current session kind so the
// spectator knows which GekkoSpectateSession config to create. File-scope
// because it's sent from TWO places: the JOIN_REQ reply paths below, and
// the session-kind-change re-broadcast in SpectatorNode_StashSnapshot
// (battle entry) -- HandleJoinAck's receive side always supported the
// re-broadcast, but the send side was never wired, so a viewer that
// joined during the host's CSS never learned the battle characters.
static CtrlPacket BuildJoinAckPacket() {
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
        // Per-slot confirm colors (slot+0xE00B, set by AssignPlayerColor
        // from the confirm button at CSS -- the engine fact that button
        // choice IS the color). The /F boot path on the viewer hardcodes
        // P1=0/P2=1; these let it stamp the real palettes instead.
        ack.data.spec_join_ack.host_p1_color = 0xFF;
        ack.data.spec_join_ack.host_p2_color = 0xFF;
        const int32_t c1 = *(const int32_t*)0x4DFD8Bu;
        const int32_t c2 = *(const int32_t*)(0x4DFD8Bu + 0xE03Fu);
        if (c1 >= 0 && c1 < 8) ack.data.spec_join_ack.host_p1_color = (uint8_t)c1;
        if (c2 >= 0 && c2 < 8) ack.data.spec_join_ack.host_p2_color = (uint8_t)c2;
    }
    return ack;
}

void SpectatorNode_HandleJoinReq(const sockaddr_in& from, SpecJoinMode mode,
                                 uint8_t caps, uint32_t resume_frame) {
    const bool udp_ok = SpecUdpEnabled() && (caps & SPEC_JOIN_UDP_OK) != 0;
    if ((caps & SPEC_JOIN_RESUME) == 0) resume_frame = 0;
    // Pin the mode NOW, from the host's state at this instant -- the
    // same instant the ACK's kind is computed from, so the viewer's
    // natural-boot/battle-boot decision and the host's delivery path
    // can never diverge. (The bind used to decide from its own LATER
    // state: a CSS-time joiner whose bind fired after battle started
    // got a battle snapshot against a title-screen engine = deadlock.)
    if (mode == SpecJoinMode::CURRENT_MATCH && resume_frame == 0 &&
        Netplay_GetSessionKind() != NetplaySessionKind::BATTLE) {
        mode = SpecJoinMode::FULL_SESSION;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "SpectatorNode: pre-battle JOIN -- pinning mode to "
            "FULL_SESSION (from-frame-0 stream, no snapshot)");
    }
    char addr_buf[48] = {};
    FormatAddr(from, addr_buf, sizeof(addr_buf));

    auto BuildJoinAck = []() { return BuildJoinAckPacket(); };

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
            if (sub.tcp_bound && !g_state.spec_transport_relay &&
                SpectatorTCP::HasLiveConnFor(sub.addr)) {
                // Live stream already flowing: this JOIN_REQ is a dup or
                // an over-eager retry. Re-ACK and change NOTHING -- the
                // old reset dropped the conn the previous JOIN opened,
                // and the viewer's heal retried 500ms later: an infinite
                // join storm that DoS'ed this host's main loop and
                // starved its own netplay sends (one-directional 30s
                // blackout -> P1/P2 barrier wedge).
                sub.last_seen_ms = GetTickCount64();
                sub.udp_ok       = udp_ok;
                CtrlPacket ack = BuildJoinAckPacket();
                ControlChannel_SendTo(ack, sub.addr);
                return;
            }
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "SpectatorNode: JOIN_REQ from existing subscriber %s — "
                        "resetting bind state for fresh backfill (mode=%s)",
                        addr_buf,
                        mode == SpecJoinMode::CURRENT_MATCH ? "CURRENT_MATCH"
                                                            : "FULL_SESSION");
            sub.tcp_bound    = false;
            sub.ack_frame    = 0;
            sub.join_mode    = mode;
            sub.udp_ok       = udp_ok;
            sub.resume_frame = resume_frame;
            sub.last_seen_ms = GetTickCount64();
            // Drop the old TCP conn + any stale pending clients from this
            // IP so the bind path pairs the spectator's FRESH dial instead
            // of an abandoned one (deep-join reconnect-loop fix).
            SpectatorTCP::DropConnectionsFromAddr(sub.addr);
            // Phase 2c: late-arriving spec_user_id backfill. If the
            // first JOIN_REQ raced past our spec_incoming poll (common
            // on loopback where UDP RTT is microseconds), sub.spec_user_id
            // stayed empty -- relay-mode SendTo can't address it. The
            // launcher refreshes the punch dict on every WS event for
            // this addr, so a retry JOIN_REQ should find the user_id
            // by now. Pop on success.
            if (sub.spec_user_id.empty()) {
                auto it = g_state.pending_spec_user_ids.find(addr_buf);
                if (it != g_state.pending_spec_user_ids.end()) {
                    sub.spec_user_id = it->second;
                    g_state.pending_spec_user_ids.erase(it);
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "SpectatorNode: backfilled spec_user_id=%s on existing "
                        "sub %s (raced past first JOIN_REQ)",
                        sub.spec_user_id.c_str(), addr_buf);
                }
            }
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
        sub.udp_ok       = udp_ok;
        sub.resume_frame = resume_frame;
        // Phase 2c: pop the cached spec_user_id (if any) for this addr.
        // Punch-target poll wrote it earlier when the hub's
        // spec_incoming forwarded the sub's user_id. Used by relay-mode
        // SendTo to address binary frames; ignored in TCP mode.
        {
            char ip_str[INET_ADDRSTRLEN] = {};
            inet_ntop(AF_INET, (void*)&from.sin_addr, ip_str, sizeof(ip_str));
            char addr_key[64];
            std::snprintf(addr_key, sizeof(addr_key), "%s:%u",
                          ip_str, ntohs(from.sin_port));
            auto it = g_state.pending_spec_user_ids.find(addr_key);
            if (it != g_state.pending_spec_user_ids.end()) {
                sub.spec_user_id = it->second;
                g_state.pending_spec_user_ids.erase(it);
            } else {
                // Race fallback: ControlChannel_Poll runs RawReceive
                // (which dispatched this JOIN_REQ) BEFORE
                // TickHostMaintenance's punch-target poll updates our
                // dict. If launcher published the user_id just before
                // this tick, the dict won't have it yet on this call.
                // Read directly from shared mem as the
                // single-source-of-truth fallback. If the shm punch
                // target matches our `from` addr, use its user_id.
                FM2KSharedMemData* shm = GetSharedMemory();
                if (shm && shm->magic == FM2K_SHARED_MEM_MAGIC &&
                    shm->spectator_punch_ip_be == from.sin_addr.s_addr &&
                    shm->spectator_punch_port  == ntohs(from.sin_port) &&
                    shm->spectator_punch_user_id[0]) {
                    sub.spec_user_id = std::string(
                        shm->spectator_punch_user_id,
                        strnlen(shm->spectator_punch_user_id,
                                sizeof(shm->spectator_punch_user_id)));
                }
            }
        }
        g_state.subscribers.push_back(sub);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: Accepted subscriber %s (%zu/%zu, mode=%s, "
                    "transport=%s, user_id=%s)",
                    addr_buf, g_state.subscribers.size(), g_state.capacity,
                    mode == SpecJoinMode::CURRENT_MATCH ? "CURRENT_MATCH"
                                                       : "FULL_SESSION",
                    g_state.spec_transport_relay ? "RELAY" : "TCP",
                    sub.spec_user_id.empty() ? "(none)" : sub.spec_user_id.c_str());

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
    // Viewer side: an echo from the upstream is the gameplay-independent
    // liveness proof. The session-derived datagram flow stops whenever
    // the host is between sessions (CSS sync under loss can take many
    // seconds) and the silence failover then read a healthy-but-quiet
    // host as dead -- the CSS "disconnect/re-subscribe" the user kept
    // seeing (2026-06-11 14:32).
    if (g_state.subscribed_upstream &&
        AddrEqual(from, g_state.upstream_addr)) {
        g_state.last_udp_recv_ms = GetTickCount64();
        return;
    }
    for (auto& sub : g_state.subscribers) {
        if (AddrEqual(sub.addr, from)) {
            sub.last_seen_ms = GetTickCount64();
            // Echo so the viewer's liveness clock ticks even when no
            // session is confirming frames (1Hz, 16 bytes).
            CtrlPacket hb = {};
            hb.header.type = CtrlMsg::SPEC_HEARTBEAT;
            ControlChannel_SendTo(hb, sub.addr);
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
    // Phase F capability advertisement (remaining reserved bytes stay
    // zero from the {} init above). Old hosts ignore reserved bits.
    if (SpecUdpEnabled()) {
        req.data.spec_join_req.reserved[0] |= SPEC_JOIN_UDP_OK;
    }
    // Light re-join: mid-stream viewers declare where their admission
    // cursor stands so the host backfills exactly the gap (no snapshot).
    if (g_state.have_frame_baseline && g_state.pb_started) {
        req.data.spec_join_req.reserved[0] |= SPEC_JOIN_RESUME;
        const uint32_t resume = g_state.next_expected_frame;
        std::memcpy(&req.data.spec_join_req.reserved[1], &resume, 4);
    }
    ControlChannel_SendTo(req, upstream);
    return true;
}

void SpectatorNode_HandleJoinAck(const sockaddr_in& from, uint8_t host_session_kind,
                                 uint16_t host_tcp_port,
                                 uint8_t host_p1_char, uint8_t host_p2_char,
                                 uint8_t host_stage,
                                 uint8_t host_p1_color, uint8_t host_p2_color) {
    // If host advertised real chars (in-battle), forward to the BTB
    // runtime-override channel so the slot-0 /F dispatcher loads the
    // host's actual character files. We CAN'T use SetEnvironmentVariableA
    // + getenv here — Win32 SetEnv updates the process env block but
    // not the CRT's _environ cache, so getenv() in BTB returns the stale
    // launcher-provided placeholder (char 0). PerGamePatches keeps a
    // hook-internal struct that BTB reads first; this bypasses the CRT
    // cache entirely.
    if (host_session_kind != 2 /* pre-battle: CSS or NONE */) {
        // Tournament flow: the host's players are still at CSS (or
        // between sessions). The viewer must NOT /F-boot -- it walks
        // title->CSS naturally and replays the from-frame-0 stream,
        // watching the lock-ins live.
        PerGamePatches_AbortBtbNaturalBoot();
        g_state.natural_boot = true;
        // The join mode is pinned HOST-side at JOIN_REQ time (a
        // CURRENT_MATCH request reaching a pre-battle host becomes
        // FULL_SESSION there) -- a spec-side re-request raced: it made
        // the host reset bind state and drop the freshly-dialed TCP
        // conn, leaving a 15s zombie (2026-06-11 12:49).
    }
    if (host_session_kind == 2 /* BATTLE */) {
        PerGamePatches_SetRuntimeBtbOverrides(host_p1_char,
                                              host_p2_char,
                                              host_stage,
                                              host_p1_color,
                                              host_p2_color);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "SpectatorNode: seeded runtime BTB from JOIN_ACK "
            "(p1=%u/c%u p2=%u/c%u stage=%u)",
            (unsigned)host_p1_char, (unsigned)host_p1_color,
            (unsigned)host_p2_char, (unsigned)host_p2_color,
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

    // Phase F: a fresh UDP admission epoch starts ONLY when a new TCP
    // connection (whose OP_BASELINE re-arms it exactly) is coming --
    // i.e. when we are NOT currently connected and will dial below.
    // The host re-broadcasts JOIN_ACK informationally at every battle
    // entry (char/color refresh for late joiners); disarming on those
    // killed UDP admission on every viewer at every battle start, with
    // no OP_BASELINE ever coming to re-arm it -- the recurring "UDP
    // dies at battle entry" (2026-06-11 15:04).
    if (!SpectatorTCP::IsUpstreamConnected()) {
        g_state.udp_epoch_armed = false;
    }

    // RNG sync + queue clear: ONLY apply on FIRST-TIME subscribe (spectator
    // is still at title/pre-CSS, no game state to lose). On reconnect-after-
    // silence-failover or on re-broadcast ACK, spectator's local sim is
    // mid-match — clobbering RNG / wiping the queue would erase in-progress
    // state. Gate both on first-time only.
    //
    // have_frame_baseline sub-gate (review hole 9): RequestJoin sets
    // join_req_pending on EVERY reconnect attempt, so first_time is true
    // for genuine reconnects too. If a baseline already exists, the queue
    // holds received-but-unplayed frames the dedup cursor has already
    // counted -- clearing them would skip those frames forever. Only a
    // truly fresh viewer (no baseline yet) gets the clear + RNG pin.
    if (first_time && !g_state.have_frame_baseline) {
        *(uint32_t*)0x41FB1C = 0x12345678;
        g_state.pb_queue.clear();
        g_state.pb_current_p1 = 0;
        g_state.pb_current_p2 = 0;
        g_state.pb_boundary         = State::PbBoundary::NONE;
        g_state.pending_reset_input = false;
        g_state.pending_sound_init  = false;
        CssAutoConfirm_SetSeamHold(false);
    }
    g_state.playing_back = true;

    // Dial the host's TCP port. Bulk INPUT_BATCH / INITIAL_MATCH /
    // MATCH_END flow over TCP exclusively in legacy mode. In relay
    // mode (Phase 3), spec data arrives via hub WS -> launcher -> our
    // inbound ring, so there's no P2P TCP to dial.
    if (g_state.spec_transport_relay) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "SpectatorNode: JOIN_ACK accepted in relay mode -- skipping "
            "ConnectUpstream (spec data arrives via hub via launcher via "
            "inbound shared-mem ring)");
    } else {
        if (host_tcp_port == 0) {
            // Mixed-mode failure: we're a TCP-mode spec but the host
            // didn't advertise a TCP port. Two likely causes:
            //   1. Host is running FM2K_SPEC_TRANSPORT=relay; their hook
            //      skipped the TCP listener, so GetListenPort()=0. Our
            //      launcher should have auto-set FM2K_SPEC_TRANSPORT=relay
            //      via spectate_grant.spec_transport (Phase 4). If we're
            //      here, our launcher is older than that or the env
            //      didn't propagate.
            //   2. Host has a genuinely broken hook init (listener bind
            //      failed on all candidates).
            // Either way the spec won't get data; refuse cleanly with
            // an actionable error message rather than the silent dial.
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                "SpectatorNode: JOIN_ACK from host advertises no TCP port "
                "AND we are in legacy TCP mode -- likely a relay-mode host "
                "but our launcher didn't auto-derive (Phase 4 requires "
                ">= v0.2.58). Workaround: set FM2K_SPEC_TRANSPORT=relay "
                "in the spec's env before launching, OR update the spec's "
                "launcher. Refusing the subscription.");
            g_state.subscribed_upstream = false;
            return;
        }
        char host_ip[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, (void*)&from.sin_addr, host_ip, sizeof(host_ip));
        if (SpectatorTCP::IsUpstreamConnected()) {
            // Session-kind-change re-broadcast over a healthy connection
            // (e.g. the battle-entry JOIN_ACK that seeds BTB chars):
            // nothing to dial, keep the stream.
        } else if (!SpectatorTCP::ConnectUpstream(host_ip, host_tcp_port)) {
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
            // v1 meta = 16 bytes (no flags/compressed_bytes); v2 = 20.
            constexpr size_t META_V1_BYTES = 16;
            if (payload_len < META_V1_BYTES) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: SNAPSHOT_BEGIN truncated (payload=%zu, need %zu)",
                    payload_len, META_V1_BYTES);
                g_state.pb_snapshot_inbox = State::SnapshotInbox{};
                break;
            }
            SnapshotMetadata meta = {};
            std::memcpy(&meta, payload,
                        payload_len >= sizeof(SnapshotMetadata)
                            ? sizeof(SnapshotMetadata) : META_V1_BYTES);
            if (meta.version == 1) {
                // v1 sender: uncompressed, wire size == total.
                meta.flags            = 0;
                meta.compressed_bytes = meta.total_bytes;
            } else if (meta.version != SPECTATOR_SNAPSHOT_VERSION) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: SNAPSHOT_BEGIN version mismatch "
                    "(got %u, want <= %u) — dropping snapshot, peer must "
                    "rejoin with FULL_SESSION",
                    meta.version, SPECTATOR_SNAPSHOT_VERSION);
                g_state.pb_snapshot_inbox = State::SnapshotInbox{};
                break;
            }
            const uint32_t wire_bytes =
                (meta.flags & SNAPSHOT_FLAG_ZERO_RLE) ? meta.compressed_bytes
                                                      : meta.total_bytes;
            if (wire_bytes == 0 || wire_bytes > meta.total_bytes) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: SNAPSHOT_BEGIN bad wire size %u (total %u)",
                    wire_bytes, meta.total_bytes);
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
            inbox.blob.assign(wire_bytes, 0);   // wire (possibly compressed) bytes
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

            const uint32_t expected_wire =
                (inbox.meta.flags & SNAPSHOT_FLAG_ZERO_RLE)
                    ? inbox.meta.compressed_bytes : inbox.meta.total_bytes;
            if (inbox.bytes_received != expected_wire) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: SNAPSHOT_END byte-count mismatch "
                    "(received %zu, want %u) — discarding",
                    inbox.bytes_received, expected_wire);
                inbox = State::SnapshotInbox{};
                break;
            }
            if (inbox.meta.flags & SNAPSHOT_FLAG_ZERO_RLE) {
                std::vector<uint8_t> raw(inbox.meta.total_bytes);
                if (!ZeroRleDecompress(inbox.blob.data(), inbox.blob.size(),
                                       raw.data(), raw.size())) {
                    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                        "SpectatorNode: SNAPSHOT_END zero-RLE decompress "
                        "failed (%zu wire -> %u raw) — discarding",
                        inbox.blob.size(), inbox.meta.total_bytes);
                    inbox = State::SnapshotInbox{};
                    break;
                }
                inbox.blob.swap(raw);
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
        case SpecDataType::OP_BASELINE: {
            // Phase F: op-count baseline for this connection. Seeds
            // ops_seen with the count of non-INPUT events appended before
            // this connection's backfill start (which we will never
            // receive), and (re-)arms the UDP admission epoch. Sent by the
            // host's bind path BEFORE snapshot/backfill, so by TCP
            // ordering it always precedes the first EVENT_BATCH of the
            // fresh connection.
            if (!g_state.subscribed_upstream) return;
            if (payload_len >= 4) {
                uint32_t baseline = 0;
                std::memcpy(&baseline, payload, 4);
                g_state.conn_ops_baseline = baseline;
                g_state.conn_ops_decoded  = 0;
                g_state.tcp_rejoin_pending = false;
                if (baseline > g_state.ops_seen) {
                    g_state.ops_seen = baseline;
                }
                g_state.udp_epoch_armed = SpecUdpEnabled();
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: OP_BASELINE=%u received -- UDP "
                    "admission %s (ops_seen=%u)", baseline,
                    g_state.udp_epoch_armed ? "armed" : "disabled by env",
                    g_state.ops_seen);
            }
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
            g_state.live_established = true;

            const uint32_t expected_at_entry = g_state.have_frame_baseline
                ? g_state.next_expected_frame : 0xFFFFFFFFu;

            // Establish baseline on first batch.
            if (!g_state.have_frame_baseline) {
                g_state.have_frame_baseline = true;
                g_state.next_expected_frame = hdr.start_frame;
            }

            // NOTE: there is deliberately NO "fully-consumed batch" early
            // skip here. AppendOpAndFlush emits op-only batches with
            // frame_count=0 at the live edge (MATCH_END / MATCH_START use
            // the flush-then-append pattern, so the boundary ops always
            // ride such a batch). For a caught-up spectator,
            // start_frame + 0 <= next_expected_frame held, and the old
            // skip silently dropped MATCH_END and the next MATCH_START --
            // the spectator never followed into match 2. The per-event
            // walk below already dedups INPUTs positionally and gates ops
            // on cursor_input >= next_expected_frame, so redundant batches
            // cost ~8 decodes and nothing else.

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
                        SpectatorNode_StampInputAdmit();
                        g_state.next_expected_frame = cursor_input + 1;
                        ++pushed_inputs;
                        // Hop-1 relay.
                        SpectatorNode_OnFrameConfirmed(ev.u.input.p1, ev.u.input.p2);
                    }
                    ++cursor_input;
                } else {
                    // Phase F: op identity = global index. The connection
                    // delivers ops in order from its baseline; ops the UDP
                    // tail already accepted are duplicates -- skip their
                    // push/relay entirely (a second MATCH_START apply
                    // would corrupt the boundary).
                    const uint32_t conn_op_idx =
                        g_state.conn_ops_baseline + g_state.conn_ops_decoded;
                    ++g_state.conn_ops_decoded;
                    if (conn_op_idx < g_state.ops_seen) {
                        off += consumed;
                        continue;  // duplicate of a UDP-accepted op
                    }
                    g_state.ops_seen = conn_op_idx + 1;

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
                            case SessionEventType::CSS_ENTERED:
                                SpectatorNode_AppendCssEntered();
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

void SpectatorNode_HandleUdpInputDatagram(const uint8_t* buf, size_t len,
                                          const sockaddr_in& from) {
    // Narrow by design: only UDP_INPUT_BATCH, only from the current
    // upstream, only while the per-connection admission epoch is armed.
    // Never runs the full HandleSpecData parser -- its dedup/ordering
    // assumptions are TCP-shaped.
    if (!SpecUdpEnabled()) return;
    if (len < sizeof(SpecDataHeader) + 4) return;
    SpecDataHeader hdr;
    std::memcpy(&hdr, buf, sizeof(hdr));
    if (hdr.magic != SPEC_DATA_MAGIC) return;
    if (hdr.type != SpecDataType::UDP_INPUT_BATCH) return;
    if (!g_state.subscribed_upstream) return;
    if (!g_state.have_frame_baseline) return;
    if (!g_state.udp_epoch_armed) return;
    if (!AddrEqual(from, g_state.upstream_addr)) return;  // spoof / stale upstream
    g_state.last_udp_recv_ms = GetTickCount64();

    uint32_t op_seq = 0;
    std::memcpy(&op_seq, buf + sizeof(SpecDataHeader), 4);
    const size_t frames = hdr.frame_count;
    const size_t inputs_end = sizeof(SpecDataHeader) + 4 + frames * 4;
    if (frames == 0 || len < inputs_end) return;  // malformed
    if (op_seq > g_state.udp_highest_op_seq) {
        g_state.udp_highest_op_seq = op_seq;
    }

    // Redundant ops tail: parsed here, accepted via accept_eligible()
    // interleaved with the input admission below. An op enters the queue
    // only when (a) it is the next op in global order and (b) every
    // input that precedes it positionally has been admitted -- accepting
    // early pushed boundary ops ahead of the battle-tail inputs (the
    // seam engaged early, its mask expired into leftover attack bits,
    // carried cursors insta-locked: the 17/13 battle restart,
    // 2026-06-11). The tail reships in every datagram, so deferred
    // acceptance costs nothing.
    struct TailOp { uint32_t idx; uint32_t pos; const uint8_t* w; uint8_t wlen; bool done; };
    TailOp tail_ops[State::OPS_RING] = {};
    int tail_n = 0;
    if (len > inputs_end + 1) {
        const uint8_t* t   = buf + inputs_end;
        const uint8_t* end = buf + len;
        uint8_t n = *t++;
        for (uint8_t i = 0; i < n && t + 9 <= end &&
                            tail_n < (int)State::OPS_RING; ++i) {
            TailOp& o = tail_ops[tail_n];
            std::memcpy(&o.idx, t, 4); t += 4;
            std::memcpy(&o.pos, t, 4); t += 4;
            o.wlen = *t++;
            if (t + o.wlen > end) break;
            o.w = t; o.done = false; t += o.wlen;
            ++tail_n;
        }
    }
    auto accept_eligible = [&]() {
        for (bool progress = true; progress; ) {
            progress = false;
            for (int i = 0; i < tail_n; ++i) {
                TailOp& o = tail_ops[i];
                if (o.done || o.idx != g_state.ops_seen ||
                    g_state.next_expected_frame < o.pos) continue;
                o.done = true;
                SessionEvent ev{};
                uint8_t hdr_buf[SESSION_EVENT_MATCH_HDR_SIZE];
                const size_t consumed =
                    SessionEvent_Decode(o.w, o.wlen, &ev, hdr_buf);
                if (consumed != o.wlen ||
                    ev.type == SessionEventType::INPUT) continue;
                if (ev.type == SessionEventType::MATCH_START) {
                    MatchHeader hdr_copy;
                    std::memcpy(hdr_copy.data(), hdr_buf, hdr_copy.size());
                    g_state.pb_match_headers.push_back(hdr_copy);
                    ev.u.match_start_idx =
                        (uint16_t)(g_state.pb_match_headers.size() - 1);
                }
                g_state.pb_queue.push_back(ev);
                ++g_state.ops_seen;
                progress = true;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[SPEC-UDP] accepted op #%u type=%u at pos=%u via "
                    "datagram tail", o.idx, (unsigned)ev.type, o.pos);
                switch (ev.type) {
                    case SessionEventType::PIN_RNG:
                        SpectatorNode_AppendPinRng(ev.u.pin_rng_seed); break;
                    case SessionEventType::RESET_INPUT_STATE:
                        SpectatorNode_AppendResetInputState(); break;
                    case SessionEventType::SOUND_INIT:
                        SpectatorNode_AppendSoundInit(); break;
                    case SessionEventType::FINGERPRINT:
                        SpectatorNode_AppendFingerprint(ev.u.fingerprint_hash); break;
                    case SessionEventType::MATCH_START:
                        SpectatorNode_AppendMatchStart(hdr_buf); break;
                    case SessionEventType::MATCH_END:
                        SpectatorNode_AppendMatchEnd(ev.u.match_end); break;
                    case SessionEventType::SESSION_ID:
                        SpectatorNode_AppendSessionId(ev.u.session_id); break;
                    case SessionEventType::CSS_ENTERED:
                        SpectatorNode_AppendCssEntered(); break;
                    case SessionEventType::ROUND_START: {
                        const auto& rp = ev.u.round_start;
                        SpectatorNode_AppendRoundStart(rp.round_idx,
                            rp.p1_hp_max, rp.p2_hp_max, rp.timer_seconds);
                        break;
                    }
                    case SessionEventType::ROUND_END: {
                        const auto& rp = ev.u.round_end;
                        SpectatorNode_AppendRoundEnd(rp.winner_idx,
                            rp.p1_hp_remaining, rp.p2_hp_remaining);
                        break;
                    }
                    default: break;
                }
            }
        }
    };
    accept_eligible();

    // [SPEC-UDP] 1Hz heartbeat BEFORE the gates -- pause periods are
    // exactly when this diagnostic matters (the original tail placement
    // went silent the moment the op gate engaged).
    {
        static uint64_t s_last_log_ms = 0;
        const uint64_t now = GetTickCount64();
        if (now - s_last_log_ms >= 1000) {
            s_last_log_ms = now;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[SPEC-UDP] admitted=%u paused_on_gate=%u dropped_on_gap=%u "
                "op_seq=%u ops_seen=%u q=%zu",
                g_state.udp_admitted_total, g_state.udp_paused_on_gate,
                g_state.udp_dropped_on_gap, op_seq, g_state.ops_seen,
                g_state.pb_queue.size());
        }
    }

    // Admission gate: every op the host appended before these inputs must
    // already be TCP-decoded locally (see invariant in spectator_node.h).
    // Boundary ops in flight (MATCH_END .. MATCH_START) pause admission;
    // the TCP burst that delivers them also jumps the cursor, after which
    // UDP re-engages. Self-healing by redundancy -- nothing is lost.
    {
        bool blocking_op_in_tail = false;
        for (int i = 0; i < tail_n; ++i) {
            if (!tail_ops[i].done && tail_ops[i].idx == g_state.ops_seen) {
                blocking_op_in_tail = true;
                break;
            }
        }
        if (op_seq > g_state.ops_seen && !blocking_op_in_tail) {
            // The op we need fell out of the 8-op tail window and TCP
            // hasn't delivered it -- inputs past it must wait (backfill
            // or a later tail recovers).
            ++g_state.udp_paused_on_gate;
            return;
        }
    }
    // Window entirely ahead of the cursor (loss burst exceeded the
    // redundancy window): drop whole datagram; TCP fills the hole.
    if (hdr.start_frame > g_state.next_expected_frame) {
        ++g_state.udp_dropped_on_gap;
        return;
    }

    uint32_t cursor   = hdr.start_frame;
    uint32_t admitted = 0;
    const uint8_t* p = buf + sizeof(SpecDataHeader) + 4;
    for (size_t i = 0; i < frames; ++i, ++cursor) {
        if (cursor != g_state.next_expected_frame) continue;  // already queued
        // In-order invariant per frame: if an op should precede further
        // inputs and it is not recoverable from this tail, stop here --
        // its position is unknown, so any further admit could jump it.
        if (op_seq > g_state.ops_seen) {
            bool next_op_in_tail = false;
            for (int k = 0; k < tail_n; ++k) {
                if (!tail_ops[k].done &&
                    tail_ops[k].idx == g_state.ops_seen) {
                    next_op_in_tail = true;
                    break;
                }
            }
            if (!next_op_in_tail) {
                ++g_state.udp_paused_on_gate;
                break;
            }
        }
        uint16_t p1 = 0, p2 = 0;
        std::memcpy(&p1, p + i * 4,     2);
        std::memcpy(&p2, p + i * 4 + 2, 2);
        SessionEvent ev{};
        ev.type       = SessionEventType::INPUT;
        ev.u.input.p1 = p1;
        ev.u.input.p2 = p2;
        g_state.pb_queue.push_back(ev);
        SpectatorNode_StampInputAdmit();
        SpectatorNode_StampUdpInputAdmit();  // UDP-borne: feeds the
                                             // TCP-only floor pre-arm
        g_state.next_expected_frame = cursor + 1;
        ++admitted;
        accept_eligible();  // an op positioned right after this input
        // Hop-1 relay parity with the TCP path (line ~3318): the late TCP
        // copy of these frames is positionally skipped there, so without
        // this append a relay node's downstream stream would lose them.
        SpectatorNode_OnFrameConfirmed(p1, p2);
    }
    g_state.udp_admitted_total += admitted;
}

void SpectatorNode_OnUpstreamTcpDead() {
    if (!g_state.subscribed_upstream) return;
    if (g_state.tcp_rejoin_pending) return;  // already riding it out
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
        "SpectatorNode: upstream TCP died -- riding on UDP, re-JOIN in "
        "background (q=%zu, ops_seen=%u, boundary=%d)",
        g_state.pb_queue.size(), g_state.ops_seen,
        (int)g_state.pb_boundary);
    // The subscription and UDP admission stay ARMED. The host's node-
    // level subscriber entry survives its TCP-layer erase (heartbeats
    // flow over UDP), so datagrams -- inputs AND the redundant ops tail
    // -- keep arriving through the dead window. Dropping the
    // subscription here used to reject every one of them: an 8-second
    // self-inflicted starvation while 5 re-JOINs begged a host whose
    // control channel was stalled in its own boundary (2026-06-11).
    // Op indexing is global, so ops_seen survives the rebind; the new
    // connection's OP_BASELINE re-seeds only the per-conn dedup cursor.
    g_state.tcp_rejoin_pending = true;
    // TickHealth's reconnect branch (rejoin pending + no live conn)
    // fires RequestJoin on its 2s backoff; the host's existing-sub path
    // resets bind state and re-ships backfill on the fresh socket.
}

bool SpectatorNode_InBoundary() {
    return g_state.pb_boundary != State::PbBoundary::NONE;
}

bool SpectatorNode_QueueHasPendingOp() {
    for (const auto& ev : g_state.pb_queue) {
        if (ev.type != SessionEventType::INPUT) return true;
    }
    return false;
}

// Self-sufficient join kick for the /F dispatch hold: the JOIN_REQ is
// normally sent by Netplay_InitAsSpectator on the DLL-init path, but the
// dispatcher's first-tick hold can win that race -- it would then pump a
// socket with NO join in flight and sit black until the host's battle-
// entry re-broadcast finally arrived (the "black screen until P1/P2
// confirm" failure). Re-requests at 1Hz until subscribed; harmless when
// the original request already landed (host's existing-sub path ACKs
// idempotently).
void SpectatorNode_KickJoin() {
    if (g_state.subscribed_upstream) return;
    if (g_state.root_addr.sin_port == 0) return;  // init hasn't configured us yet
    const uint64_t now = GetTickCount64();
    if (now - g_state.last_reconnect_attempt_ms < 1000) return;
    SpectatorNode_RequestJoin(g_state.root_addr, g_state.last_requested_mode);
}

static uint64_t g_last_input_admit_ms = 0;

// Adaptive delay bank (Phase G). The static 300-frame bank absorbs any
// arrival gap shorter than 3s -- enough for the tested clumsy profile,
// but a link with longer retransmit bursts still drains to q:0 and
// stalls. Measure the real inter-admission gaps (two rotating 30s
// buckets = rolling 30-60s max) and GROW the bank target to fit the
// link: target = max(env floor, observed_max_gap * 1.5), capped at
// 2000 frames (20s). Grow-only per session -- over-buffering after a
// bad patch is the right bias (no-stall beats low-latency here), and
// shrinking mid-stream would oscillate the glide. Gaps above 5s are
// ignored: that's an outage (TCP death, host frozen) owned by the
// failover/rejoin machinery, not jitter for the pacing layer to absorb.
// FM2K_SPEC_ADAPTIVE=0 pins the bank to the static floor.
static uint32_t g_admit_gap_bucket_cur   = 0;   // max gap (ms), current 30s bucket
static uint32_t g_admit_gap_bucket_prev  = 0;
static uint64_t g_admit_gap_bucket_start = 0;
static size_t   g_adaptive_bank_frames   = 0;   // grow-only published target
static uint64_t g_first_input_admit_ms   = 0;   // session's first admission
// Last INPUT admitted via a UDP datagram specifically (heartbeats and
// control traffic don't count). Drives the TCP-only floor pre-arm.
static uint64_t g_last_udp_input_admit_ms = 0;
void SpectatorNode_StampUdpInputAdmit() {
    g_last_udp_input_admit_ms = GetTickCount64();
}

void SpectatorNode_StampInputAdmit() {
    const uint64_t now = GetTickCount64();
    if (g_first_input_admit_ms == 0) g_first_input_admit_ms = now;
    if (g_last_input_admit_ms != 0) {
        const uint64_t gap = now - g_last_input_admit_ms;
        // 10s ceiling: longer silences are outages (TCP death, frozen
        // host) owned by the failover machinery. Everything under it is
        // jitter the bank must absorb -- the first UDP-off control run
        // showed 8.8s TCP retransmit bursts under clumsy that a 5s
        // ceiling wrongly discarded, so the bank stayed at 705 frames
        // while the link needed ~1300 and mid-battle q:0 stalls kept
        // happening (2026-06-11 18:15).
        if (gap <= 10000) {
            if (g_admit_gap_bucket_start == 0) g_admit_gap_bucket_start = now;
            if (now - g_admit_gap_bucket_start >= 30000) {
                g_admit_gap_bucket_prev  = g_admit_gap_bucket_cur;
                g_admit_gap_bucket_cur   = 0;
                g_admit_gap_bucket_start = now;
            }
            if ((uint32_t)gap > g_admit_gap_bucket_cur) {
                g_admit_gap_bucket_cur = (uint32_t)gap;
            }
        }
    }
    g_last_input_admit_ms = now;
}

size_t SpectatorNode_TargetDelayFrames() {
    static size_t s_floor = []() -> size_t {
        const char* e = std::getenv("FM2K_SPEC_DELAY");
        if (e && e[0]) {
            const long n = std::strtol(e, nullptr, 10);
            if (n >= 50 && n <= 2000) return (size_t)n;
        }
        return 300;
    }();
    static int s_adaptive = -1;
    if (s_adaptive < 0) {
        const char* v = std::getenv("FM2K_SPEC_ADAPTIVE");
        s_adaptive = (v && v[0] == '0' && v[1] == '\0') ? 0 : 1;
    }
    size_t floor_eff = s_floor;
    // TCP-only pre-arm: with the UDP accelerator dead (firewalled, or
    // FM2K_SPEC_UDP=0), arrivals come in TCP retransmit bursts that
    // routinely exceed the 3s default under loss -- don't wait for the
    // first stall to teach the adaptive growth; start from a 6s floor.
    // Engages only after 10s of admissions with no UDP-borne INPUT.
    if (s_adaptive == 1 && floor_eff < 600 &&
        g_state.subscribed_upstream && g_first_input_admit_ms != 0) {
        const uint64_t now = GetTickCount64();
        const bool udp_quiet =
            (g_last_udp_input_admit_ms == 0)
                ? (now - g_first_input_admit_ms > 10000)
                : (now - g_last_udp_input_admit_ms > 10000);
        if (udp_quiet) floor_eff = 600;
    }
    if (s_adaptive == 1) {
        const uint32_t max_gap_ms =
            (g_admit_gap_bucket_cur > g_admit_gap_bucket_prev)
                ? g_admit_gap_bucket_cur : g_admit_gap_bucket_prev;
        size_t want = (size_t)(max_gap_ms + max_gap_ms / 4) / 10;  // x1.25, ms -> frames
        if (want > 2000) want = 2000;
        const uint64_t now = GetTickCount64();
        static uint64_t s_last_grow_ms   = 0;
        static uint64_t s_last_shrink_ms = 0;
        if (want > g_adaptive_bank_frames) {
            g_adaptive_bank_frames = want;
            s_last_grow_ms = now;
            if (g_adaptive_bank_frames > floor_eff) {
                static uint64_t s_grow_log_ms = 0;
                if (now - s_grow_log_ms >= 1000) {
                    s_grow_log_ms = now;
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[SPEC-BANK] adaptive bank grew to %zu frames "
                        "(max admission gap %ums x1.25)",
                        g_adaptive_bank_frames, max_gap_ms);
                }
            }
        } else if (g_adaptive_bank_frames > want &&
                   g_adaptive_bank_frames > floor_eff &&
                   s_last_grow_ms != 0 &&
                   now - s_last_grow_ms > 60000 &&
                   now - s_last_shrink_ms >= 100) {
            // Shrink-back: grow-only pinned the session at its WORST
            // moment forever -- one early 9s burst meant 12s+ latency
            // for the rest of the night even on a recovered link. The
            // rolling buckets age the bad gap out within 60s; once no
            // growth has been needed for 60s, drift the target down at
            // 10 frames/s (the gentle 2x drain bleeds the excess cushion
            // as the target falls -- smooth catch-up, no jump cut).
            // Never below the current window's want or the floor.
            s_last_shrink_ms = now;
            --g_adaptive_bank_frames;
            static uint64_t s_shrink_log_ms = 0;
            if (now - s_shrink_log_ms >= 5000) {
                s_shrink_log_ms = now;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[SPEC-BANK] calm link -- bank drifting down: %zu "
                    "frames (window want %zu)",
                    g_adaptive_bank_frames, want);
            }
        }
    }
    return (g_adaptive_bank_frames > floor_eff) ? g_adaptive_bank_frames
                                                : floor_eff;
}

static size_t SpecDelayBankFrames() {
    return SpectatorNode_TargetDelayFrames();
}
uint32_t SpectatorNode_MsSinceLastAdmit() {
    if (g_last_input_admit_ms == 0) return 0;  // nothing admitted yet
    return (uint32_t)(GetTickCount64() - g_last_input_admit_ms);
}

bool SpectatorNode_IsSubscribedUpstream() { return g_state.subscribed_upstream; }

// True while the upstream TCP died but the subscription is riding on UDP
// with a background re-JOIN in flight. Surfaced in the window title as
// "Resyncing..." -- distinct from a cold "Connecting..." (no
// subscription at all) and from a healthy "Subscribed".
bool SpectatorNode_IsTcpRejoinPending() { return g_state.tcp_rejoin_pending; }

// Natural-boot title/menu walk in progress: the synthetic title presses
// live inside PopFrameInputs, so the jitter floor must not gate the tick
// on queue depth while the local game is still pre-CSS (q=0 at boot is
// normal -- the title walk is what gets us to where the stream starts).
bool SpectatorNode_InNaturalBootWalk() {
    if (!g_state.natural_boot) return false;
    return *(uint32_t*)FM2K::ADDR_GAME_MODE < 2000u;
}

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
    // A validated snapshot is waiting for the local engine to reach its
    // capture phase: hold the sim. Popping before the apply consumed real
    // inputs into throwaway pre-snapshot state -- and pushed the consumed
    // cursor past the anchor, which made the rewind guard discard the
    // FIRST snapshot (join-during-CSS run, 2026-06-11: spec played the
    // live stream on a fresh BTB battle, P2 never initialized).
    if (g_state.pb_snapshot_inbox.pending_apply) return false;

    // Drain non-INPUT events from the head before popping the next INPUT.
    // Each non-INPUT event dispatches to ApplySessionEvent — RNG pin,
    // input-state reset, sound dedup init, etc. The dispatch happens at
    // the moment the spectator's local sim is about to consume the next
    // INPUT, which is the same logical-frame moment the host's pin
    // happened. Eliminates the game_mode-driven mirror race.
    //
    // SEAM extension: between MATCH_END and MATCH_START applies, INPUT
    // events at the head are consumed-and-discarded instead of breaking
    // the drain — they are the host's results/CSS frames and must not
    // drive the spectator's local sim (see PbBoundary). The drain
    // naturally reaches the boundary init ops + MATCH_START, whose apply
    // flips the state to PINNING and stops the discard.
    while (!g_state.pb_queue.empty() &&
           g_state.pb_queue.front().type != SessionEventType::INPUT) {
        ApplySessionEvent(g_state.pb_queue.front());
        g_state.pb_queue.erase(g_state.pb_queue.begin());
    }

    // Boundary handling. SEAM: fall through to the normal pop -- the
    // viewer MIRRORS the host's seam (results presses, CSS cursor
    // movements) at the host's own pace; the seam hold masks confirm
    // bits + locks so the rematch flow can never advance early, and the
    // final picks come from the MATCH_START pin. PINNING: battle INPUTs
    // are parked at the head while CssAutoConfirm walks the local CSS to
    // the announced picks on synthetic neutral; exact pops resume when
    // the local game re-enters battle.
    //
    // Release is EDGE-triggered: the local mode must first drop below
    // 3000 (leave the old match's results screen) before a value >= 3000
    // counts as "the new battle". The old level check released instantly
    // when MATCH_START arrived while results were still on screen (UDP
    // live edge), feeding the new match's inputs into the old screen and
    // letting CssAutoConfirm disengage before CSS opened.
    if (g_state.pb_boundary != State::PbBoundary::NONE) {
        const uint32_t mode = *(uint32_t*)FM2K::ADDR_GAME_MODE;
        if (mode < 3000u) g_state.pb_boundary_left_battle = true;
        if (g_state.pb_boundary == State::PbBoundary::PINNING) {
            if (g_state.pb_boundary_left_battle && mode >= 3000u) {
                g_state.pb_boundary = State::PbBoundary::NONE;
                CssAutoConfirm_SetSeamHold(false);
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: boundary PINNING -> NONE (battle entered, "
                    "resuming exact input pops, q=%zu)", g_state.pb_queue.size());
                // fall through to the normal pop below
            } else {
                // Pin walk in progress: park the new match's inputs and
                // feed neutral (CssAutoConfirm drives cursor + confirm
                // directly; the results screen, if still up, advances on
                // a synthetic confirm edge).
                if (!g_state.subscribed_upstream) return false;
                uint16_t synthetic = 0;
                if (mode != 2000u) {
                    static uint32_t s_seam_tick = 0;
                    synthetic = (s_seam_tick++ & 1u) ? 0x010u : 0u;
                }
                g_state.pb_current_p1 = synthetic;
                g_state.pb_current_p2 = synthetic;
                if (p1_input) *p1_input = synthetic;
                if (p2_input) *p2_input = synthetic;
                return true;
            }
        }
        if (g_state.pb_boundary == State::PbBoundary::SEAM) {
            if (mode >= 3000u && !g_state.pb_css_marker_seen) {
                // Our results screens are still running: replay the
                // host's results inputs 1:1 (battle-end state matched,
                // so the pacing matches). Fall through to the normal pop.
            } else if (mode >= 3000u && g_state.pb_css_marker_seen) {
                // Stream already reached the host's CSS but our results
                // overran by a few frames: park the CSS inputs (they
                // mirror from CSS frame 0) and walk the remaining
                // results on synthetic edges.
                if (!g_state.subscribed_upstream) return false;
                static uint32_t s_seam_tick3 = 0;
                const uint16_t synthetic =
                    (s_seam_tick3++ & 1u) ? 0x010u : 0u;
                g_state.pb_current_p1 = synthetic;
                g_state.pb_current_p2 = synthetic;
                if (p1_input) *p1_input = synthetic;
                if (p2_input) *p2_input = synthetic;
                return true;
            } else if (mode == 2000u && !g_state.pb_css_marker_seen) {
                // Our CSS opened before the stream's CSS_ENTERED: the
                // remaining head INPUTs are the host's results tail --
                // discard them (apply ops; one is the marker) and hold
                // neutral so nothing can advance.
                while (!g_state.pb_queue.empty() &&
                       !g_state.pb_css_marker_seen) {
                    const SessionEvent& head = g_state.pb_queue.front();
                    if (head.type != SessionEventType::INPUT) {
                        ApplySessionEvent(head);
                    }
                    g_state.pb_queue.erase(g_state.pb_queue.begin());
                }
                if (!g_state.subscribed_upstream) return false;
                g_state.pb_current_p1 = 0;
                g_state.pb_current_p2 = 0;
                if (p1_input) *p1_input = 0;
                if (p2_input) *p2_input = 0;
                return true;
            } else {
                // CSS open + marker seen: the mirror starts at the host's
                // CSS frame 0. Engage the short confirm mask (eats the
                // edge-detector echo of the last pre-CSS input) and hand
                // the boundary over to pure replay.
                g_state.pb_boundary = State::PbBoundary::NONE;
                g_state.pb_post_css_mask_pops = 10;
                CssAutoConfirm_SetSeamHold(true, 0xFF, 0xFF);  // mask only
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: lean seam -> mirror (CSS aligned at "
                    "host frame 0, confirm mask 10 pops, q=%zu)",
                    g_state.pb_queue.size());
                // fall through to the normal pop
            }
        }
    }

    // Results-tail guard: the local game can reach CSS a few frames
    // before the stream's MATCH_END applies (our results screens run
    // slightly short), so pb_boundary is still NONE and the seam hasn't
    // engaged. The queued head INPUTs in that window are the host's
    // results presses -- feeding them to the fresh CSS displaced the
    // cursors before the seam engaged and the whole mirrored dance ran
    // offset (wrong chars + colors at the rematch, 2026-06-11 15:09).
    // Discard them while applying ops; the MATCH_END op flips
    // pb_awaiting_match_end and engages the SEAM, whose machinery takes
    // over on the next call. Hold neutral throughout. Gated on
    // pb_local_battle_seen: only a sim that ALREADY played this match's
    // battle can be in its results tail -- at match entry (and offline
    // replay boot) mode 2000 + awaiting means the battle hasn't started
    // locally yet and the queued INPUTs are the battle itself.
    {
        const uint32_t mode_now = *(uint32_t*)FM2K::ADDR_GAME_MODE;
        if (mode_now >= 3000u && mode_now < 4000u) {
            g_state.pb_local_battle_seen = true;
        }
    }
    if (g_state.pb_awaiting_match_end &&
        g_state.pb_local_battle_seen &&
        g_state.pb_boundary == State::PbBoundary::NONE &&
        *(uint32_t*)FM2K::ADDR_GAME_MODE == 2000u) {
        while (!g_state.pb_queue.empty() && g_state.pb_awaiting_match_end) {
            const SessionEvent& head = g_state.pb_queue.front();
            if (head.type != SessionEventType::INPUT) {
                ApplySessionEvent(head);
            }
            g_state.pb_queue.erase(g_state.pb_queue.begin());
        }
        if (!g_state.subscribed_upstream) return false;
        g_state.pb_current_p1 = 0;
        g_state.pb_current_p2 = 0;
        if (p1_input) *p1_input = 0;
        if (p2_input) *p2_input = 0;
        return true;
    }

    // Post-CSS confirm-mask countdown -- FUNCTION level, not inside the
    // boundary block: engaging the mirror clears pb_boundary in the same
    // call, so a countdown nested in that scope decremented exactly once
    // and the hold never released -- the mirror traced the host's dance
    // to the exact picks but the players' lock-ins could never register
    // (spec sat at CSS until the transport died, 2026-06-11).
    if (g_state.pb_post_css_mask_pops > 0) {
        if (--g_state.pb_post_css_mask_pops == 0) {
            CssAutoConfirm_SetSeamHold(false);
        }
    }

    // Natural-boot walk + mirrored-CSS guards run BEFORE the queue-empty
    // checks: at boot the queue is legitimately EMPTY (the stream hasn't
    // arrived), and the old ordering made the synthetic title presses
    // unreachable -- the viewer sat in the attract demo at q=0 until the
    // host's backfill happened to land (2026-06-11 12:49).
    if constexpr (FM2K::kIsFM2K) {
        static int s_natboot_offline_cached = -1;
        if (s_natboot_offline_cached < 0) {
            const char* v = std::getenv("FM2K_REPLAY_FILE");
            s_natboot_offline_cached = (v && v[0]) ? 1 : 0;
        }
        if (s_natboot_offline_cached == 0) {
            static bool s_css_reached = false;
            const uint32_t mode_now = *(uint32_t*)FM2K::ADDR_GAME_MODE;
            if (mode_now >= 2000u && !s_css_reached) {
                s_css_reached = true;
                // The title-mash press straddles the title->CSS
                // transition: the engine's edge detector reads it as a
                // rising confirm on CSS frame ~1 for BOTH players --
                // instant 0/0 lock, 100-frame countdown, battle before
                // the players ever confirmed (NATCSS trace 2026-06-11:
                // act=1/1 by pop 10, timer==pop). Engage the confirm-
                // masking hold for the first 60 CSS frames to eat the
                // stray edge; released in the post-release guard below.
                if (mode_now == 2000u) {
                    CssAutoConfirm_SetSeamHold(true, 0xFF, 0xFF);  // mask only
                }
            }
            // Bank-building hold: once our CSS is open, do NOT start the
            // mirror until the queue holds the full delay bank -- the
            // players are browsing during this, so the extra idle
            // seconds are invisible, and playback then runs the entire
            // session a fixed bank behind live (arrival gaps shorter
            // than the bank never reach the picture). 15s safety cap
            // for short host CSS phases.
            if (s_css_reached && g_state.natural_boot &&
                !g_state.pb_bank_built && mode_now == 2000u) {
                static uint64_t s_bank_start_ms = 0;
                const uint64_t bnow = GetTickCount64();
                if (s_bank_start_ms == 0) s_bank_start_ms = bnow;
                if (g_state.pb_queue.size() >= SpecDelayBankFrames() ||
                    bnow - s_bank_start_ms > 15000) {
                    g_state.pb_bank_built = true;
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "SpectatorNode: delay bank built (q=%zu, %llums) "
                        "-- mirror starting",
                        g_state.pb_queue.size(),
                        (unsigned long long)(bnow - s_bank_start_ms));
                } else {
                    g_state.pb_current_p1 = 0;
                    g_state.pb_current_p2 = 0;
                    if (p1_input) *p1_input = 0;
                    if (p2_input) *p2_input = 0;
                    return true;
                }
            }
            if (!s_css_reached) {
                // Keep the boot in the VS context: without the netplay
                // handshake P1/P2 have, the title's attract sequence
                // (title.demo / characterselect.demo) takes over within
                // ~300ms and its auto-CSS locks default chars and starts
                // a demo battle (the 0/0 ryu/ryu "join"). Pin the VS
                // game-mode flag and clear the demo state every tick so
                // the demo can never drive, while the synthetic edges
                // walk the menu.
                *(uint32_t*)0x470058u = 1;   // g_game_mode_flag = VS
                *(uint32_t*)0x47010Cu = 0;   // demo mode state
                uint16_t synthetic = 0;
                if (mode_now == 1000u) {
                    static uint32_t s_nat_title_tick = 0;
                    synthetic = (s_nat_title_tick++ & 1u) ? 0x010u : 0u;
                    static uint64_t s_nat_log_ms = 0;
                    const uint64_t nb_now = GetTickCount64();
                    if (nb_now - s_nat_log_ms > 1000) {
                        s_nat_log_ms = nb_now;
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "[NATBOOT] mode=%u flag=%u demo=%u menu=%u",
                            mode_now, *(uint32_t*)0x470058u,
                            *(uint32_t*)0x47010Cu,
                            *(uint32_t*)0x424F30u);
                    }
                }
                g_state.pb_current_p1 = synthetic;
                g_state.pb_current_p2 = synthetic;
                if (p1_input) *p1_input = synthetic;
                if (p2_input) *p2_input = synthetic;
                return true;
            }
            // Post-release guard: the demo machinery must stay quiet
            // through the mirrored CSS too (it re-engages on input
            // silence; the early replayed CSS frames are mostly idle).
            if (mode_now == 2000u) {
                *(uint32_t*)0x47010Cu = 0;
                static uint32_t s_natcss_pop = 0;
                if (s_natcss_pop == 60) {
                    // Stray title-edge window over; hand CSS to the
                    // live mirror (real confirms must pass from here).
                    CssAutoConfirm_SetSeamHold(false);
                }
                // [NATCSS] every 10th pop until the mechanism that
                // advances a mirrored CSS to battle is identified --
                // logs the state machine's inputs and progression.
                if ((s_natcss_pop++ % 10u) == 0) {
                    const int* p1c = (const int*)0x424E50;
                    const int* p2c = (const int*)0x424E58;
                    const int* sel = (const int*)0x470020;
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[NATCSS] pop=%u p1@(%d,%d) p2@(%d,%d) sel=%d/%d "
                        "act=%u/%u timer=%u q=%zu",
                        s_natcss_pop - 1,
                        p1c[0], p1c[1], p2c[0], p2c[1], sel[0], sel[1],
                        *(uint32_t*)0x47019Cu, *(uint32_t*)0x4701A0u,
                        *(uint32_t*)0x424F00u, g_state.pb_queue.size());
                }
            }
        }
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
        // Live natural-boot alignment gate (tournament-flow CSS join):
        // the host's stream begins at ITS CSS, so a viewer that boots
        // naturally must NOT let its TITLE screen eat those inputs --
        // that shifted the stream cursor and the viewer's CSS started
        // mid-dance with diverged state (locked early, entered battle
        // BEFORE the players). Park the stream and walk the title on
        // synthetic confirm edges until the local CSS opens; from there
        // the dance replays in true lockstep from the host's CSS frame 0.
        // One-shot: once CSS (or any later phase) has been reached, the
        // gate never re-engages (boundaries are mid-session lockstep).
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
    g_state.pb_started    = true;
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

    // ---- Spec hub-relay inbound drain (Phase 3) -------------------------
    // When the launcher has WS binary frames forwarded from the hub, it
    // writes them into the inbound shared-mem ring. Each Slot's payload
    // is a SpecDataHeader-prefixed wire frame -- byte-identical to what
    // SpectatorTCP::PollUpstream would have produced from the TCP path
    // it's replacing. Feed straight into HandleSpecData.
    //
    // Bound work-per-tick so a snapshot burst doesn't monopolize the
    // hook tick. ~32 slots = up to 512 KB per tick which covers most
    // snapshots in 2 ticks. Drain continues next tick if there's more.
    if (g_state.spec_relay_in) {
        constexpr int kMaxPerTick = 32;
        for (int i = 0; i < kMaxPerTick; ++i) {
            const fm2k::spec_relay::Slot* slot =
                fm2k::spec_relay::PeekFront(g_state.spec_relay_in);
            if (!slot) break;
            // payload bytes are exactly what SpectatorTCP's framer
            // would deliver to HandleSpecData via the TCP path. The
            // sockaddr_in second arg is a debug breadcrumb; zero
            // works (TCP path also passes zero).
            sockaddr_in zero_from{};
            zero_from.sin_family = AF_INET;
            SpectatorNode_HandleSpecData(
                slot->payload, slot->payload_len, zero_from);
            fm2k::spec_relay::PopFront(g_state.spec_relay_in);
        }
    }

    // ---- Subscriber-side: heartbeat + silence failover ------------------
    if (g_state.subscribed_upstream) {
        if (now - g_state.last_heartbeat_send_ms >= SPECTATOR_HEARTBEAT_INTERVAL_MS) {
            CtrlPacket hb = {};
            hb.header.type = CtrlMsg::SPEC_HEARTBEAT;
            ControlChannel_SendTo(hb, g_state.upstream_addr);
            g_state.last_heartbeat_send_ms = now;
        }

        // Phase F: silent-TCP-death detector. A connection can die
        // asymmetrically -- the host's side errors out while ours never
        // surfaces anything (observed 2026-06-11: host "subscriber read
        // error" at the match-2 boundary, spec recv just went quiet; the
        // op gate then paused UDP forever and nothing re-joined). The UDP
        // datagrams announce the host's op count: if ops we provably lack
        // (udp_highest_op_seq > ops_seen) stay undeliverable while the
        // TCP stream has been silent for several seconds, the stream is
        // wedged regardless of HOW it died. Re-join is cheap (~2s).
        if (!g_state.spec_transport_relay &&
            g_state.udp_epoch_armed &&
            g_state.udp_highest_op_seq > g_state.ops_seen) {
            const uint64_t tcp_last = SpectatorTCP::LastUpstreamRecvMs();
            if (tcp_last > 0 && now > tcp_last && now - tcp_last >= 4000) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: op-gap stall -- UDP announces op_seq=%u "
                    "but ops_seen=%u and TCP silent %llums; declaring "
                    "upstream TCP dead",
                    g_state.udp_highest_op_seq, g_state.ops_seen,
                    (unsigned long long)(now - tcp_last));
                SpectatorNode_OnUpstreamTcpDead();
            }
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
        //   (c) snapshot transfer in progress. Under real loss the 1MB
        //       snapshot trickles at TCP-throughput pace (~30KB/s at 20%
        //       loss / 140ms RTT) and a single retransmit-backoff stall
        //       can exceed 5s. Failing over mid-transfer drops the inbox
        //       and restarts the 1MB from scratch -- at high loss the
        //       join NEVER completes (observed: failover at 311296/1081196
        //       bytes, repeating forever). While the inbox is mid-
        //       transfer, allow a much longer window (30s without ANY
        //       TCP bytes) before declaring the upstream dead.
        const auto& snap_inbox = g_state.pb_snapshot_inbox;
        const bool snapshot_in_flight =
            snap_inbox.active &&
            snap_inbox.bytes_received < snap_inbox.meta.total_bytes;
        const uint64_t silence_budget_ms =
            (snapshot_in_flight || !g_state.live_established)
                ? (uint64_t)30000
                : (uint64_t)SPECTATOR_SILENCE_FAILOVER_MS;
        // UDP is the primary liveness signal now: inputs + ops ride
        // datagrams, so TCP being quiet is NORMAL (retransmit storms
        // under loss, host swap stalls, nothing bulk to send). The old
        // TCP-only condition fired at every battle->CSS return under
        // clumsy -- queue drained (host stalled production), TCP quiet
        // past budget -> the viewer sent SPEC_LEAVE, unsubscribed
        // ITSELF, killed its own UDP fan-out, and span through a full
        // reconnect against a host mid-swap. Fail over only when BOTH
        // transports are silent.
        const bool udp_alive = g_state.last_udp_recv_ms > 0 &&
            now - g_state.last_udp_recv_ms < 2000;
        if (!g_spectator_catchup &&
            queue_idle &&
            !udp_alive &&
            recv_ms > 0 &&
            now - recv_ms >= silence_budget_ms)
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
            g_state.live_established    = false;
            SpectatorTCP::DisconnectUpstream();
        }
    }

    // Reconnect path: not subscribed, but we have a root we can fall
    // back to. Throttle so we don't spam JOIN_REQ.
    if ((!g_state.subscribed_upstream || g_state.tcp_rejoin_pending) &&
        g_state.root_addr.sin_port != 0 &&
        // Never fire a new JOIN while an upstream connection exists --
        // the host's existing-sub re-JOIN path drops connections from
        // our addr, so a retry during an in-flight handshake/backfill
        // kills its own transfer ("End of stream" loop, 2026-06-11).
        !SpectatorTCP::IsUpstreamConnected() &&
        now - g_state.last_reconnect_attempt_ms >=
            (g_state.tcp_rejoin_pending ? 500u : SPECTATOR_RECONNECT_BACKOFF_MS))
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
                // Phase 2c: stash the spec_user_id from this punch event so
                // HandleJoinReq's new-subscriber branch can assign it onto
                // the Subscriber when the matching JOIN_REQ from this
                // (ip:port) arrives. Empty string when hub didn't include
                // user_id (older hub); harmless -- relay-mode SendTo will
                // just skip subs with no user_id.
                char user_id_buf[33] = {};  // shm has 32; +1 for safety NUL
                std::memcpy(user_id_buf, shm->spectator_punch_user_id,
                            sizeof(shm->spectator_punch_user_id));
                user_id_buf[32] = '\0';
                if (user_id_buf[0]) {
                    char addr_key[64];
                    std::snprintf(addr_key, sizeof(addr_key), "%s:%u",
                                  ip_str, (unsigned)port);
                    g_state.pending_spec_user_ids[addr_key] = user_id_buf;
                }
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: hub-coordinated NAT punch toward "
                    "spectator %s:%u user_id=%s (seq=%u)",
                    ip_str, (unsigned)port,
                    user_id_buf[0] ? user_id_buf : "(none)",
                    (unsigned)s_last_punch_seq);

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
    //
    // Relay mode (Phase 2c): there's no TCP listener and no async dial;
    // the launcher's WS-to-hub data path is already up at JOIN_REQ time.
    // So as soon as we have a spec_user_id (for addressing relay sends),
    // mark the sub "bound" immediately so the snapshot+backfill ship.
    // Without this short-circuit, sub.tcp_bound stayed false forever in
    // relay mode and the spec saw zero data after subscribing.
    for (auto& sub : g_state.subscribers) {
        if (sub.tcp_bound) continue;
        // Never frame-0-backfill a CURRENT_MATCH viewer: defer the bind
        // until a snapshot exists (next StashSnapshot = next battle
        // entry). The legacy fallback replayed the host's title/CSS
        // inputs into a /F-booted battle (join-during-CSS = total state
        // garbage, exposed by the CSS-dwell harness 2026-06-11), and
        // binding at the battle-entry tick raced StashSnapshot by ~50ms.
        // The viewer meanwhile holds at title until the battle-entry
        // JOIN_ACK re-broadcast seeds its BTB chars.
        if (sub.join_mode == SpecJoinMode::CURRENT_MATCH &&
            !g_state.current_snapshot.valid &&
            Netplay_GetSessionKind() == NetplaySessionKind::BATTLE) {
            // Battle just started but StashSnapshot hasn't run yet (the
            // 51ms bind-vs-stash race) -- wait a tick for the snapshot.
            // Pre-battle joins do NOT defer: they get the from-frame-0
            // stream and follow the host's CSS live (tournament flow:
            // spectators connect while the players sit at CSS, then
            // watch the lock-ins happen).
            static uint64_t s_last_defer_log_ms = 0;
            if (now - s_last_defer_log_ms > 2000) {
                s_last_defer_log_ms = now;
                char dbuf[48] = {}; FormatAddr(sub.addr, dbuf, sizeof(dbuf));
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: deferring CURRENT_MATCH bind for %s "
                    "until StashSnapshot (battle-entry race)", dbuf);
            }
            continue;
        }
        bool just_bound = false;
        if (g_state.spec_transport_relay) {
            // Need spec_user_id to address relay sends. If still empty
            // (loopback race -- punch dict hadn't populated when first
            // JOIN_REQ arrived), wait for the existing-sub re-JOIN path
            // to backfill it. Next bind-loop tick succeeds.
            if (!sub.spec_user_id.empty()) {
                sub.tcp_bound = true;
                just_bound = true;
            }
        } else if (SpectatorTCP::RegisterAcceptedClient(sub.addr)) {
            sub.tcp_bound = true;
            just_bound = true;
        }
        if (just_bound) {
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
            // Light re-join: a mid-stream viewer declared its resume
            // position -- ship NOTHING but the gap. No snapshot (it
            // would be discarded viewer-side anyway), no from-anchor
            // re-delivery; one round trip and the stream is whole.
            const bool resume_bind = sub.resume_frame > 0;
            const bool use_snapshot = !resume_bind &&
                sub.join_mode == SpecJoinMode::CURRENT_MATCH &&
                g_state.current_snapshot.valid;

            // Phase F: op-count baseline FIRST on the fresh connection --
            // before snapshot/backfill -- so the viewer's ops_seen starts
            // exact for everything this connection delivers. The
            // connection ships ops from min(backfill_first_idx,
            // live-flush cursor) onward; the baseline counts the ops
            // BELOW that point (which a mid-session joiner never sees).
            // Gated on udp_ok: an old build's TCP framer drops the
            // connection on an unknown SpecDataType.
            if (sub.udp_ok && SpecUdpEnabled() &&
                !g_state.spec_transport_relay) {
                const size_t first_idx = resume_bind
                    ? BackfillFirstIdxForFrame(sub.resume_frame)
                    : use_snapshot
                    ? BackfillFirstIdxForFrame(g_state.current_snapshot.input_frame)
                    : 0;
                const size_t clamp = std::min(g_state.last_flushed_event_idx,
                                              g_state.session_events.size());
                const size_t effective_start = std::min(first_idx, clamp);
                uint32_t baseline = 0;
                for (size_t i = 0; i < effective_start; ++i) {
                    if (g_state.session_events[i].type != SessionEventType::INPUT)
                        ++baseline;
                }
                SendOpBaselineTo(sub.addr, baseline);
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: OP_BASELINE=%u sent (first_idx=%zu, "
                    "clamp=%zu, total_ops=%u)",
                    baseline, first_idx, clamp, g_state.total_op_count);
            }

            const char* xport = g_state.spec_transport_relay ? "RELAY" : "TCP";
            // Host settings (rounds-to-win, timer, SOCD, etc) go to EVERY
            // joiner regardless of bind flavor -- the push lived only in
            // the snapshot branch, so natural/FULL_SESSION viewers ran
            // engine defaults (wrong round settings, 2026-06-11).
            {
                extern void Netplay_SendHostConfigToSpec(const sockaddr_in& to);
                Netplay_SendHostConfigToSpec(sub.addr);
            }
            if (resume_bind) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: %s bound for %s — LIGHT RESUME "
                    "(gap backfill from INPUT-frame=%u, no snapshot)",
                    xport, buf, sub.resume_frame);
                SendSessionBackfillFromFrame(sub.addr, sub.resume_frame);
            } else if (use_snapshot) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: %s bound for %s%s%s — CURRENT_MATCH "
                    "(snapshot match=%u + tail from INPUT-frame=%u)",
                    xport, buf,
                    g_state.spec_transport_relay ? " user_id=" : "",
                    g_state.spec_transport_relay ? sub.spec_user_id.c_str() : "",
                    g_state.current_snapshot.match_index,
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
                    "SpectatorNode: %s bound for %s%s%s — %s "
                    "(legacy from-frame-0 backfill)",
                    xport, buf,
                    g_state.spec_transport_relay ? " user_id=" : "",
                    g_state.spec_transport_relay ? sub.spec_user_id.c_str() : "",
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
