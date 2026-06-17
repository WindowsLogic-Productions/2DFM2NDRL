#include "spectator_node.h"
#include "spectator_node_internal.h"  // shared State model + g_state (split for sibling TUs)
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

// The spectator-internal helpers now live in namespace specnode (split across
// spec_*.cpp). Pull them into scope so call sites here stay unqualified.
using namespace specnode;

// =============================================================================
// MODULE STATE
// =============================================================================

// struct State + member structs/constants now live in spectator_node_internal.h
// so sibling TUs (spec_*.cpp) can share the state. This file owns the single def.
State g_state;

namespace {




// Legacy SendInitialMatchTo / INITIAL_MATCH packet path removed in C12.
// MATCH_START flows as a SessionEvent op interleaved with INPUTs in the
// EVENT_BATCH stream; late joiners get it via SendSessionBackfillTo.


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


void SpectatorNode_SetCapacity(size_t max_direct) {
    g_state.capacity = max_direct;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "SpectatorNode: Capacity set to %zu", max_direct);
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
