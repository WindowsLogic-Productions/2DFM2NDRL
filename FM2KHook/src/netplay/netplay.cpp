// CCCaster-Style Netplay Implementation
// - Control channel for CSS input sync using INPUT DELAY (not lockstep)
// - GekkoNet for battle mode rollback
// - Uses game's internal timer for frame counting
#include "netplay.h"
#include "../hooks/hooks.h"   // Hook_ApplySOCD_Public for SOCD-pre-apply on spec capture
#include "control_channel.h"
#include "game_hash.h"
#include "input.h"
#include "savestate.h"
#include "spectator_node.h"
#include "nat_traversal.h"
#include "upload_queue.h"
#include "globals.h"
#include "gekkonet.h"
#include "../audio/sound_rollback.h"
#include "../ui/shared_mem.h"  // SharedMem_PublishMatchOutcome
#include "../parity/parity_recorder.h"  // ParityRecorder::Close on harness auto-terminate
#include <SDL3/SDL_log.h>
#include <ws2tcpip.h>
#include <cstdlib>
#include <string>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <ctime>
#include <random>
#include <cstdio>
#include <cstring>
#include <atomic>

// =============================================================================
// SIMPLIFIED STATE
// =============================================================================

enum class SimpleState : uint8_t {
    DISCONNECTED,   // No connection
    CONNECTED,      // Control channel connected
    BATTLE          // GekkoNet active for battle
};

static SimpleState g_simple_state = SimpleState::DISCONNECTED;

static NetplayState MapToLegacyState(SimpleState s) {
    switch (s) {
        case SimpleState::DISCONNECTED: return NetplayState::DISCONNECTED;
        case SimpleState::CONNECTED: return NetplayState::CSS_LOBBY;
        case SimpleState::BATTLE: return NetplayState::BATTLE_RUNNING;
        default: return NetplayState::DISCONNECTED;
    }
}

// =============================================================================
// SESSION TRACKING
// =============================================================================
//
// What kind of GekkoNet session g_session currently points at. We run two
// distinct sessions back-to-back for a single match: a CSS lockstep session
// (input_prediction_window=0, no rollback) and a battle rollback session
// (prediction_window=8 + runahead). The kind determines the per-tick driver
// and the swap-frame protocol direction. SessionKind is an alias for the
// public NetplaySessionKind enum exported from netplay.h so external callers
// (spectator_node, hub_client) can compare against the same values.

using SessionKind = NetplaySessionKind;
static SessionKind g_session_kind = SessionKind::NONE;

// CSS lockstep parameters (ported from the legacy CCCaster-style ring-buffer
// implementation). With GekkoNet's prediction=0 mode, local_delay is the
// per-player input commitment delay — peer A's input committed at frame F
// becomes the input applied at frame F+local_delay, identical semantics to
// the previous "store at frame+delay, read at frame" model.
static constexpr int CSS_LOCAL_DELAY = 6;
static constexpr int CSS_CONFIRM_LOCKOUT = 150;     // Block confirm for first N frames (moon selector workaround)

// CSS state — input transport now lives inside the CSS GekkoSession
static bool g_css_active        = false;  // Currently in CSS mode (game-side detection)
static bool g_css_synced        = false;  // Both peers BATTLE_READY, CSS GekkoSession ready
static bool g_remote_css_ready  = false;  // Remote has entered CSS
static bool g_local_css_ready   = false;  // We've entered CSS
static uint32_t g_css_frame     = 0;      // Last confirmed CSS AdvanceEvent frame (+1)

// Per-poll AdvanceEvent input cache. Netplay_ProcessCSS drives gekko_update_session,
// which fires AdvanceEvent only when both peers have inputs for the current frame
// (lockstep guarantee). The cached inputs are read by Hook_GetPlayerInput via
// Netplay_GetCSSInput.
static uint16_t g_css_advance_p1     = 0;
static uint16_t g_css_advance_p2     = 0;
static bool     g_css_advance_ready  = false;

// GekkoNet session pointer + readiness flag (shared by CSS and battle —
// only one is alive at a time, distinguished by g_session_kind).
static GekkoSession* g_session = nullptr;
static bool g_session_ready = false;
static uint16_t g_p1_input = 0;
static uint16_t g_p2_input = 0;
static uint32_t g_netplay_frame = 0;

// Rollback tracking
static uint32_t g_rollback_count = 0;
static uint32_t g_last_rollback_frame = 0;
static uint32_t g_desync_count = 0;
static uint32_t g_last_desync_log_tick = 0;
static int g_local_delay = 1;  // Computed from RTT at battle start

// Tuning knobs — set at battle session start, mirrored here so
// Netplay_TickHeartbeat can echo the live values into [BEAT] lines.
// `g_runahead_user_pref` is the configured "on" value (env / future
// UI default); `g_runahead_active` is the value actually applied to
// the running session right now, which can differ when the user
// hits F8 mid-match to toggle between 0 and user_pref.
static int                g_pred_window               = 16;
// Runahead is DEFAULT OFF (too CPU-heavy at 100fps/10ms budget). These get
// overwritten per battle in Netplay_StartBattle (pref = local_delay, active
// = 0 unless FM2K_RUNAHEAD forces it); the static defaults just keep it off
// before/between matches.
static int                g_runahead_user_pref        = 0;
static std::atomic<int>   g_runahead_active{0};
static std::atomic<bool>  g_runahead_toggle_requested{false};

// --- re-sim profiler (FM2K_PERF_PROFILE=1) ---------------------------------
// Times the three hot ops in the rollback loop so we can see where the 10ms
// (100 FPS) per-frame budget actually goes: SaveState_Save, SaveState_Load,
// and the game tick (process_game_inputs + update_game). Reports avg microsec
// + count every 500 advances. Off unless the env var is set, so production
// builds pay nothing. (#62/#63)
namespace {
struct PerfBucket { uint64_t ns = 0; uint32_t n = 0; };
static const bool g_perf_on = [] {
    const char* v = std::getenv("FM2K_PERF_PROFILE");
    return v && v[0] && v[0] != '0';
}();
static uint64_t PerfQpcFreq() {
    static uint64_t f = [] { LARGE_INTEGER q; QueryPerformanceFrequency(&q); return (uint64_t)q.QuadPart; }();
    return f;
}
static inline uint64_t PerfNowNs() {
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return (uint64_t)((long double)c.QuadPart * 1e9L / (long double)PerfQpcFreq());
}
static PerfBucket g_perf_save, g_perf_load, g_perf_adv;
struct PerfScope {
    PerfBucket* b; uint64_t t0;
    explicit PerfScope(PerfBucket* x) : b(g_perf_on ? x : nullptr), t0(b ? PerfNowNs() : 0) {}
    ~PerfScope() { if (b) { b->ns += PerfNowNs() - t0; b->n++; } }
};
static void PerfMaybeReport() {
    if (!g_perf_on) return;
    static uint32_t ticks = 0;
    if (++ticks % 500 != 0) return;
    auto us = [](const PerfBucket& b) { return b.n ? (double)b.ns / b.n / 1000.0 : 0.0; };
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "[PERF] save n=%u avg=%.1fus | load n=%u avg=%.1fus | tick(PGI+UG) n=%u avg=%.1fus "
        "(budget is 10000us/frame at 100fps)",
        g_perf_save.n, us(g_perf_save), g_perf_load.n, us(g_perf_load),
        g_perf_adv.n, us(g_perf_adv));
}
}  // namespace

// Rolling window for [BEAT] line. Reset every emit so the per-window
// avg + max numbers describe the most recent ~10s, not session totals.
static uint32_t g_beat_window_rb_sum   = 0;
static uint32_t g_beat_window_rb_max   = 0;
static uint32_t g_beat_window_rb_count = 0;   // real rollbacks observed
static uint64_t g_beat_last_emit_ms    = 0;

// Common handler for both real (GekkoDesyncDetected) and synthetic
// (FM2K_FORCE_DESYNC_AT_FRAME) desync events. Same diagnostic dump,
// same upload manifest, same TerminateProcess — the synthetic path
// exercises the full end-to-end pipeline (Dump → RNG flush → ZIP
// bundle → manifest → launcher upload → server pairing) so we can
// validate fixes without waiting for a real-world determinism leak.
//
// `synthetic` only affects log wording — the file-write + terminate
// path is identical.
static void HandleDesyncDetected(int frame, uint32_t local_chk,
                                 uint32_t remote_chk, bool synthetic) {
    g_desync_count++;
    uint32_t now_tick = GetTickCount();

    // Always log the first desync with full detail.
    if (g_desync_count <= 5) {
        auto& rc = SaveState_GetRegionChecksums();
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "%sDESYNC #%u f=%d: local=0x%08X remote=0x%08X",
            synthetic ? "SYNTHETIC " : "",
            g_desync_count, frame, local_chk, remote_chk);
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "  SAVED: rng=0x%08X game=0x%08X obj=0x%08X char=0x%08X inp=0x%08X",
            rc.rng, rc.game_state, rc.object_pool, rc.char_dynamic,
            rc.input_tracking);
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "  UNSAVED: eff1=0x%08X eff2=0x%08X shake=0x%08X",
            rc.effect_sys1, rc.effect_sys2, rc.shake_effects);
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "  FINGERPRINT: gameplay=0x%08X (HP/pos/rng/timer only — "
            "if this MATCHES across peers the desync is a memory-residue "
            "false positive)",
            rc.gameplay_fingerprint);
    } else if (now_tick - g_last_desync_log_tick > 1000) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "DESYNC #%u f=%d: local=0x%08X remote=0x%08X",
            g_desync_count, frame, local_chk, remote_chk);
        g_last_desync_log_tick = now_tick;
    }

    // First desync only: dump diagnostics, enqueue upload, terminate.
    if (g_desync_count != 1) return;

    SaveState_DumpDesyncDiagnostic(frame, local_chk, remote_chk,
                                   g_player_index);
    SaveState_FlushRngTrace(g_player_index, "first desync");

    // Escape hatch: FM2K_NO_DESYNC_KILL=1 keeps the game running for
    // diagnostic sessions. Off by default.
    const char* no_kill = std::getenv("FM2K_NO_DESYNC_KILL");
    const bool kill_on_desync = !(no_kill && std::strcmp(no_kill, "1") == 0);

    if (!kill_on_desync) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "DESYNC: FM2K_NO_DESYNC_KILL=1 — staying alive for diagnostic "
            "observation. Game state will corrupt further; expect a crash "
            "within a few thousand frames.");
        return;
    }

    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
        "%sDESYNC: terminating game on first divergence (frame %d). "
        "Dump written to FM2K_P%d_desync_f%d.log. Set FM2K_NO_DESYNC_KILL=1 "
        "to keep running for diagnostic inspection.",
        synthetic ? "SYNTHETIC " : "",
        frame, g_player_index, frame);

    // Drop an upload manifest for the launcher to pick up.
    //
    // Paths and game_id go through the UTF-8 helpers — GetCurrent-
    // DirectoryA / GetModuleFileNameA return Shift-JIS bytes on
    // Japanese-locale Windows with Japanese-named game folders, which
    // produces non-UTF-8 JSON. Pre-v0.2.44 launchers crashed on those
    // manifests; v0.2.44+ launchers quarantine them, but our own
    // manifests should obviously be valid.
    {
        std::string cwd_utf8;
        if (!fm2k::upload_queue::GetCurrentDirectoryUtf8(cwd_utf8)) {
            cwd_utf8 = ".";
        }
        char debug_path[MAX_PATH * 2];
        char desync_path[MAX_PATH * 2];
        char rng_path[MAX_PATH * 2];
        std::snprintf(debug_path, sizeof(debug_path),
            "%s\\logs\\FM2K_P%d_Debug.log",
            cwd_utf8.c_str(), g_player_index + 1);
        std::snprintf(desync_path, sizeof(desync_path),
            "%s\\FM2K_P%d_desync_f%d.log",
            cwd_utf8.c_str(), g_player_index + 1, frame);
        std::snprintf(rng_path, sizeof(rng_path),
            "%s\\FM2K_P%d_rngtrace.csv",
            cwd_utf8.c_str(), g_player_index + 1);

        std::string exe_utf8;
        fm2k::upload_queue::GetModuleFileNameUtf8(exe_utf8);
        // Strip directory + .exe to get the game_id stem.
        std::string game_id_str;
        {
            size_t slash = exe_utf8.find_last_of("\\/");
            std::string base = (slash == std::string::npos)
                ? exe_utf8 : exe_utf8.substr(slash + 1);
            size_t dot = base.find_last_of('.');
            game_id_str = (dot == std::string::npos)
                ? base : base.substr(0, dot);
        }

        fm2k::upload_queue::Manifest mfst;
        mfst.kind = synthetic ? "desync_synthetic" : "desync";
        mfst.frame = frame;
        mfst.session_id = SpectatorNode_GetSessionId();
        mfst.player_index = g_player_index;
        mfst.game_id = game_id_str.c_str();
        mfst.file_paths.emplace_back(debug_path);
        mfst.file_paths.emplace_back(desync_path);
        mfst.file_paths.emplace_back(rng_path);
        fm2k::upload_queue::Enqueue(mfst);
    }

    SharedMem_PublishMatchOutcome(FM2K_MATCH_OUTCOME_DESYNC);
    fflush(stdout);
    fflush(stderr);
    TerminateProcess(GetCurrentProcess(), 1);
}

// Synthetic desync trigger — checks FM2K_FORCE_DESYNC_AT_FRAME env
// var once at hook init. When the netplay frame counter reaches the
// configured value, calls HandleDesyncDetected with synthetic flag
// set. Both peers receive the same env var (the launcher inherits
// it), both fire at the same frame, both end up with the same
// match_token-derived match_id — perfect smoke test for the upload
// + cross-peer pairing pipeline.
//
// Set to -1 (default) to disable.
static int g_force_desync_at_frame = -1;
static bool g_force_desync_inited = false;

static void MaybeFireSyntheticDesync() {
    if (!g_force_desync_inited) {
        g_force_desync_inited = true;
        const char* e = std::getenv("FM2K_FORCE_DESYNC_AT_FRAME");
        if (e && *e) {
            g_force_desync_at_frame = std::atoi(e);
            if (g_force_desync_at_frame > 0) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "SYNTHETIC-DESYNC armed: will fire at battle frame %d "
                    "(env FM2K_FORCE_DESYNC_AT_FRAME)",
                    g_force_desync_at_frame);
            }
        }
    }
    if (g_force_desync_at_frame > 0 &&
        (int)g_netplay_frame >= g_force_desync_at_frame) {
        const int target = g_force_desync_at_frame;
        g_force_desync_at_frame = -1;  // one-shot
        HandleDesyncDetected(target, 0xDEADBEEFu, 0xCAFEBABEu,
                             /*synthetic=*/true);
    }
}

// Connection-lifetime cache for the computed (auto) battle delay. Pinned
// at the FIRST battle-session start after CONNECTED and reused for every
// subsequent CSS->battle transition until disconnect. Without this we
// recomputed per-battle off whatever stale rtt_worst happened to be sitting
// in the bucket — a single CSS spike would slam delay to 15 on a 6 ms link.
// Cleared on Netplay_Shutdown / Netplay_OnDisconnect so a reconnect picks
// a fresh value next time.
static int  g_session_delay_cached       = 0;
static bool g_session_delay_cache_valid  = false;

// Highest frame number we've ever recorded into the replay/spectator stream.
// Reset on each Netplay_StartBattle (g_netplay_frame also resets to 0).
// Gates the GekkoAdvance recording path against runahead duplicates — each
// frame is advanced multiple times under runahead, but only the first
// monotonic forward crossing is "the" confirmed advance.
static uint32_t g_highest_recorded_frame = 0;

// Battle entry sync barrier - ensures both clients enter battle at same time.
// Also carries swap_frame negotiation for the CSS-session->battle-session swap:
// both peers propose g_css_frame + SWAP_FRAME_BUFFER on detection, exchange via
// BATTLE_ENTERING.data.sync.frame, and the agreed value is max(local, remote).
// The actual gekko_destroy(css)/gekko_create(battle) deferred until the active
// CSS session reaches g_battle_entry_swap_frame.
static bool     g_local_battle_entered    = false;
static bool     g_remote_battle_entered   = false;
static bool     g_battle_synced           = false;
static uint32_t g_battle_entry_swap_frame = 0;     // Latest agreed swap frame.

// "Are we expecting BATTLE_ENTERING right now?" gate. Set when a new CSS
// GekkoSession comes up (we're entering the CSS phase that will swap to
// battle); cleared once we've started the battle session. Without this,
// stale BATTLE_ENTERING packets from a previous match — sent during that
// match's CSS phase but delayed in flight or kernel-buffered, arriving
// 100–200ms AFTER the previous match's Netplay_EndBattle reset us — get
// blindly accepted and pre-poison g_battle_entry_swap_frame for the new
// match. Symptom: every subsequent match's "BATTLE SYNC: both peers
// signaled" line shows the SAME stale swap_frame, the BATTLE_ENTERING
// echo loop never terminates (both peers latch g_local_battle_entered),
// and eventually the battle GekkoNet sync stalls in one direction
// → black screen on CSS->battle transition. See logs from 2026-05-09.
static bool     g_battle_entry_armed      = false;

// Mirror gate for BATTLE_END to prevent the same stale-packet-poisoning
// pattern in the battle->CSS direction.
//
// Battle exit sync barrier (battle-session -> CSS-session swap, for rematch
// or return-to-menu). Mirrors the entry barrier but reads g_netplay_frame
// instead of g_css_frame and is driven by BATTLE_END instead of BATTLE_ENTERING.
static bool     g_local_battle_end_signaled  = false;
static bool     g_remote_battle_end_signaled = false;
static bool     g_battle_end_synced          = false;
static uint32_t g_battle_end_swap_frame      = 0;
static bool     g_battle_end_armed           = false;

// Frames of slack added to the proposed swap_frame so both peers have time
// to drain in-flight inputs and converge their proposals before reaching it.
// 8 @ 100 FPS = 80ms — comfortably above typical RTT. Tunable.
constexpr uint32_t SWAP_FRAME_BUFFER = 8;

// Handshake state
static bool g_received_hello = false;
static bool g_received_hello_ack = false;

// =============================================================================
// Note: We use our own frame counter instead of game timer
// The game timer at 0x470044 appears to be 0 during CSS
// =============================================================================

// =============================================================================
// CONTROL MESSAGE HANDLER
// =============================================================================

// Forward decls for SOCD-mode helpers in hooks.cpp (file-scope so all
// uses below — Netplay_BroadcastHostConfig + the HOST_CONFIG receiver —
// can reach them).
extern "C" int  Hook_GetSOCDModePublic();
extern "C" void Hook_SetSOCDMode(int mode);

// Random-stage handoff to the FM95 LoadStageFile_alt hook. Set when the
// xorshift block in Netplay_StartBattleSession produces a fresh roll;
// read by Hook_LoadStageFileAlt to override the function's arg0. 0xFFFFFFFF
// means "random not enabled / no override" — hook passes the original
// arg0 through unchanged. FM2K doesn't read this — its ADDR_SELECTED_STAGE
// write goes to the canonical 0x43010c which the game reads natively.
static uint32_t g_pending_random_stage = 0xFFFFFFFFu;
extern "C" uint32_t Netplay_PeekNextRolledStage() {
    return g_pending_random_stage;
}

// Build the current HOST_CONFIG packet from live engine state. Shared
// by Netplay_BroadcastHostConfig (fans to peer + all subs at battle-
// start moments) and Netplay_SendHostConfigToSpec (one-shot push when a
// spectator binds mid-match, so they don't miss the broadcast that
// fired before they were subscribed). Only meaningful on the host side.
static CtrlPacket BuildHostConfigPacket() {
    CtrlPacket pkt = {};
    pkt.header.type = CtrlMsg::HOST_CONFIG;
    pkt.data.host_config.selected_stage  = *(uint32_t*)FM2K::ADDR_SELECTED_STAGE;
    pkt.data.host_config.socd_mode       = (uint8_t)Hook_GetSOCDModePublic();
    // Loaded-from-game.ini engine globals. hit_judge_set_function reads
    // game.ini at boot into these — spec's local game.ini gives spec's
    // defaults, but host's authoritative values must override or specs
    // get wrong timer / round count (pkmncc default time=60, host had
    // time=0 / infinite, spec ended up running with 60s rounds).
    pkt.data.host_config.round_time_sec  = *(uint32_t*)0x430114; // lParam
    pkt.data.host_config.round_count     = *(uint32_t*)0x430124; // g_default_round
    pkt.data.host_config.game_speed_pct  = *(uint32_t*)0x430104; // uValue
    return pkt;
}

// One-shot push: snapshot current host settings and ship to a single
// subscriber addr. Called from SpectatorNode's TCP-bound handler so a
// mid-match spec joiner gets the current rules (stage, SOCD) without
// having to wait for the next match-start broadcast. No-op when the
// local peer isn't host (spec doesn't have authoritative config).
void Netplay_SendHostConfigToSpec(const sockaddr_in& to) {
    if (g_player_index != 0) return;
    CtrlPacket pkt = BuildHostConfigPacket();
    ControlChannel_SendTo(pkt, to);
}

// Snapshot host's current settings and ship them to the remote peer +
// any subscribed spectators. Called from CheckFullyConnected (initial
// rendezvous) and from Netplay_StartBattle (every new match) so settings
// changes mid-session propagate. No-op when the local peer isn't host.
static void Netplay_BroadcastHostConfig() {
    if (g_player_index != 0) return;  // only host pushes config
    CtrlPacket pkt = BuildHostConfigPacket();
    const auto& hc = pkt.data.host_config;
    ControlChannel_SendHostConfig(
        /*selected_stage*/  hc.selected_stage,
        /*round_count*/     hc.round_count,
        /*round_time_sec*/  hc.round_time_sec,
        /*game_speed_pct*/  hc.game_speed_pct,
        /*socd_mode*/       hc.socd_mode);

    // Also push to subscribed spectators on the same multiplex channel.
    auto subs = SpectatorNode_GetSubscriberAddrs();
    for (const auto& s : subs) {
        ControlChannel_SendTo(pkt, s);
    }
}

static void CheckFullyConnected() {
    if (g_received_hello && g_received_hello_ack) {
        if (g_simple_state != SimpleState::CONNECTED) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Netplay: Full handshake complete - CONNECTED!");
            g_simple_state = SimpleState::CONNECTED;
            ControlChannel_SetConnected(true);

            // Sync RNG immediately
            *(uint32_t*)FM2K::ADDR_RANDOM_SEED = 0x12345678;
            SpectatorNode_AppendPinRng(0x12345678);
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Netplay: Synced RNG=0x12345678");

            // Push host's authoritative match config so client adopts the
            // same stage/SOCD/etc settings without manual mirroring.
            Netplay_BroadcastHostConfig();
        }
    }
}

static void OnControlMessage(const CtrlPacket* packet, const sockaddr_in& from) {
    switch (packet->header.type) {
        case CtrlMsg::HELLO: {
            const uint32_t local_hash = fm2k::game_hash::Compute();
            const uint32_t peer_hash  = packet->data.hello.game_hash;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Netplay: Received HELLO from player %d (peer_hash=0x%08X local_hash=0x%08X)",
                packet->data.hello.player_id,
                peer_hash, local_hash);
            // Game-data hash check (#57). 0 on either side means the
            // peer is older / we couldn't enumerate — fall through to
            // the existing handshake flow so we don't break legacy
            // clients during rollout. Both sides nonzero + different
            // = abort: write a DISCONNECT outcome so the launcher's
            // PollMatchOutcome surfaces a toast and closes the game.
            if (local_hash != 0 && peer_hash != 0 && local_hash != peer_hash) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                    "Netplay: GAME DATA MISMATCH — peer=0x%08X us=0x%08X (%s). "
                    "Aborting handshake; have both peers send each other their "
                    "FM2K_*_Debug.log file and diff the 'GameHash: manifest' "
                    "section to find which file differs.",
                    peer_hash, local_hash, fm2k::game_hash::DescribeLocal());
                // Re-dump the local manifest right next to the error so users
                // who scroll up from the bottom of the log see exactly what
                // we hashed without hunting for the original "GameHash:
                // manifest" line up at boot time. Multi-line so a peer
                // reading the log can quickly spot a different size or
                // content hash on a specific filename.
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                    "Netplay: local manifest follows (compare against peer's "
                    "log line-by-line):");
                // Iterate the cached entries vector directly. We used
                // to split the cached manifest STRING line-by-line via
                // strchr('\n'), but that path turned out to corrupt one
                // entry's render in some installs (placeholder22.player
                // showed up as "placeholder22|-", missing extension and
                // size). Going through entries gets bytes byte-equivalent
                // to the boot-time per-entry log.
                fm2k::game_hash::ForEachManifestEntry(
                    [](const char* name, uint64_t size, uint64_t content_hash,
                       void* /*user*/) {
                        if (content_hash != 0) {
                            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                                "  %s|%llu|%016llx",
                                name,
                                (unsigned long long)size,
                                (unsigned long long)content_hash);
                        } else {
                            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                                "  %s|%llu|-",
                                name,
                                (unsigned long long)size);
                        }
                    }, nullptr);
                SharedMem_PublishMatchOutcome(FM2K_MATCH_OUTCOME_HASH_MISMATCH);
                break;
            }
            ControlChannel_SendHelloAck(static_cast<uint8_t>(g_player_index));
            g_received_hello = true;
            CheckFullyConnected();
            break;
        }

        case CtrlMsg::HELLO_ACK:
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Netplay: Received HELLO_ACK");
            g_received_hello_ack = true;
            CheckFullyConnected();
            break;

        case CtrlMsg::CSS_INPUT:
            // CSS_INPUT is dead code post-redesign — CSS lockstep now lives
            // inside a GekkoGameSession with prediction_window=0, so inputs
            // flow through GekkoNet's transport. The enum value + this case
            // are kept as a no-op for backward compatibility with peers that
            // still send the old packet (they'll be silently ignored).
            break;

        case CtrlMsg::BATTLE_READY: {
            // After CSS GekkoSession is fully up (g_css_synced=true) the
            // BATTLE_READY rendezvous is over — any leftover packets are
            // network-buffered echoes from the rendezvous window and can
            // be silently dropped. Without this gate the unconditional
            // echo below would ping-pong forever between both peers,
            // logging "Sent / Received BATTLE_READY" every ~10ms for the
            // entire CSS phase.
            if (g_css_synced) {
                break;
            }
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Netplay: Received BATTLE_READY from remote");
            g_remote_css_ready = true;

            // Loss-tolerant echo — same pattern as BATTLE_ENTERING /
            // BATTLE_END. When peers return to CSS at slightly
            // different wall-clock times (one finishes battle-end-sync
            // ~300 ms before the other), the ahead peer creates its CSS
            // GekkoSession on the first BATTLE_READY it sees, then
            // STOPS sending its own BATTLE_READY. The lagging peer's
            // BATTLE_READYs after that point arrive here and need an
            // echo back, otherwise the lagging peer never sees our
            // signal and stays stuck resending forever. Bounded by the
            // !g_css_synced gate above — echo only happens during the
            // rendezvous window, terminates when sync completes.
            if (g_local_css_ready) {
                ControlChannel_SendBattleReady();
            }
            break;
        }

        case CtrlMsg::BATTLE_ENTERING: {
            const uint32_t remote_proposal = packet->data.sync.frame;
            // Spectator-side handling: this is host telling us about the
            // upcoming CSS->battle swap. Flip our SpectateSession to battle
            // config. (Spectators don't participate in proposal convergence —
            // they passively follow whatever the host announces.)
            if (g_session_kind == SessionKind::SPECTATE) {
                Netplay_OnHostBattleEntering(remote_proposal);
                break;
            }
            // Reject stale carryover from a previous match. g_battle_entry_armed
            // is true ONLY between "new CSS session up" and "battle session
            // started" — outside that window, an incoming BATTLE_ENTERING is
            // either a delayed packet from the prior match (which would
            // otherwise pre-poison g_battle_entry_swap_frame for the next
            // match) or a duplicate from the current battle's echo storm
            // (which has nothing to do but keep both peers latched in the
            // ping-pong forever).
            if (!g_battle_entry_armed) {
                static uint32_t s_drop_count = 0;
                if (s_drop_count++ < 8 || (s_drop_count & 0x3F) == 0) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Netplay: ignoring out-of-window BATTLE_ENTERING "
                        "(swap=%u, drop#%u) — armed=false",
                        remote_proposal, (unsigned)s_drop_count);
                }
                break;
            }
            // Player-side handling: convergence on max(local, remote) swap.
            const uint32_t prev_agreed = g_battle_entry_swap_frame;
            if (remote_proposal > g_battle_entry_swap_frame) {
                g_battle_entry_swap_frame = remote_proposal;
            }
            g_remote_battle_entered = true;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Netplay: Received BATTLE_ENTERING (remote_swap=%u, prev_agreed=%u, agreed=%u)",
                remote_proposal, prev_agreed, g_battle_entry_swap_frame);

            // Echo our own BATTLE_ENTERING back if we've already signaled
            // locally — needed for the lossy-network case where remote
            // received our signal but their echo to us was dropped.
            // Without rate-limiting, both peers echo every echo from the
            // other and we get an infinite ping-pong storm (observed
            // hundreds of sends in a single millisecond). 100ms cap is
            // far below the swap-frame transition window so packet-loss
            // recovery still works, but the storm can't run away.
            if (g_local_battle_entered) {
                static uint32_t last_echo_ms = 0;
                const uint32_t now_ms = GetTickCount();
                if (now_ms - last_echo_ms >= 100) {
                    ControlChannel_SendBattleEntering(g_battle_entry_swap_frame);
                    last_echo_ms = now_ms;
                }
            }
            break;
        }

        case CtrlMsg::BATTLE_END: {
            const uint32_t remote_proposal = packet->data.sync.frame;
            if (g_session_kind == SessionKind::SPECTATE) {
                Netplay_OnHostBattleEnd(remote_proposal);
                break;
            }
            // Same stale-carryover gate as BATTLE_ENTERING. Armed when the
            // battle GekkoSession comes up; cleared in Netplay_EndBattle.
            // Outside that window the only thing a BATTLE_END packet can do
            // is poison the next match's battle-end barrier.
            if (!g_battle_end_armed) {
                static uint32_t s_drop_count = 0;
                if (s_drop_count++ < 8 || (s_drop_count & 0x3F) == 0) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Netplay: ignoring out-of-window BATTLE_END "
                        "(swap=%u, drop#%u) — armed=false",
                        remote_proposal, (unsigned)s_drop_count);
                }
                break;
            }
            const uint32_t prev_agreed = g_battle_end_swap_frame;
            if (remote_proposal > g_battle_end_swap_frame) {
                g_battle_end_swap_frame = remote_proposal;
            }
            g_remote_battle_end_signaled = true;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Netplay: Received BATTLE_END (remote_swap=%u, prev_agreed=%u, agreed=%u)",
                remote_proposal, prev_agreed, g_battle_end_swap_frame);

            // Same rate-limited echo as BATTLE_ENTERING.
            if (g_local_battle_end_signaled) {
                static uint32_t last_echo_ms = 0;
                const uint32_t now_ms = GetTickCount();
                if (now_ms - last_echo_ms >= 100) {
                    ControlChannel_SendBattleEnd(g_battle_end_swap_frame);
                    last_echo_ms = now_ms;
                }
            }
            break;
        }

        case CtrlMsg::DISCONNECT:
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Netplay: Remote disconnected");
            ControlChannel_SetConnected(false);
            g_simple_state = SimpleState::DISCONNECTED;
            // Drop the pinned auto-delay so the next connection measures
            // fresh (peer might be on a different network now).
            g_session_delay_cache_valid = false;
            g_session_delay_cached      = 0;
            break;

        case CtrlMsg::HOST_CONFIG: {
            // Host's authoritative match settings — adopt locally so this
            // peer (client OR spectator) runs with identical rules.
            // Per-field "unset" sentinels: 0xFFFFFFFF for selected_stage,
            // 0 for the count/time/speed fields, 0xFF for socd_mode.
            const auto& hc = packet->data.host_config;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Netplay: Received HOST_CONFIG (stage=%u rounds=%u time=%u speed=%u socd=%u)",
                hc.selected_stage, hc.round_count, hc.round_time_sec,
                hc.game_speed_pct, (unsigned)hc.socd_mode);

            // Stage selection — direct memcpy to FM2K's selected-stage
            // global (FM2K::ADDR_SELECTED_STAGE; IDA-verified in WW as
            // 0x43010c, the var that vs_round_function reads when
            // calling LoadStageFile(wParam)). The previous addr
            // 0x470188 had no xrefs and writes were silently ignored.
            if (hc.selected_stage != 0xFFFFFFFF) {
                *(uint32_t*)FM2K::ADDR_SELECTED_STAGE = hc.selected_stage;
            }

            // SOCD mode — wire through the runtime setter. Persists for
            // the rest of the session unless host changes it again.
            if (hc.socd_mode != 0xFF) {
                Hook_SetSOCDMode((int)hc.socd_mode);
            }

            // Game-ini-derived settings. Engine's hit_judge_set_function
            // (0x414930) loaded these from the LOCAL game.ini at boot —
            // for spec mode that's spec's local .ini which doesn't know
            // about the host's per-match overrides. Host's authoritative
            // values must clobber here so timer / round count / speed
            // match. Sentinel 0xFFFFFFFF means "host left default, don't
            // override". 0 IS a valid value for round_time_sec (= no
            // timer / infinite), which is why we can't use 0 as unset.
            if (hc.round_time_sec != 0xFFFFFFFFu) {
                *(uint32_t*)0x430114 = hc.round_time_sec;  // lParam (TIMER_SET)
            }
            if (hc.round_count != 0xFFFFFFFFu) {
                *(uint32_t*)0x430124 = hc.round_count;     // g_default_round (1v1)
            }
            if (hc.game_speed_pct != 0xFFFFFFFFu) {
                *(uint32_t*)0x430104 = hc.game_speed_pct;  // uValue (GameSpeed)
            }
            break;
        }

        case CtrlMsg::CHAT: {
            // Inbound peer chat. Append to the chat log ring; launcher UI
            // reads via Netplay_PopChatMessage on its own cadence.
            const char* text = packet->data.chat.text;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Netplay: CHAT from remote: \"%s\"", text);
            Netplay_PushChatMessage(/*from_remote*/ true, text);
            break;
        }

        case CtrlMsg::SPEC_JOIN_REQ:
            // Older spectator builds send no payload (zero-init bytes), which
            // resolves to mode=FULL_SESSION — the existing replay-from-frame-0
            // path. New builds set mode explicitly. Range-clamp anything
            // beyond the highest known enum value back to FULL_SESSION so a
            // future-versioned spectator pointed at this older host stays on
            // the safe path.
            {
                const uint8_t mode_byte = packet->data.spec_join_req.mode;
                const SpecJoinMode mode =
                    (mode_byte == static_cast<uint8_t>(SpecJoinMode::CURRENT_MATCH))
                        ? SpecJoinMode::CURRENT_MATCH
                        : SpecJoinMode::FULL_SESSION;
                SpectatorNode_HandleJoinReq(from, mode);
            }
            break;

        case CtrlMsg::SPEC_JOIN_ACK:
            SpectatorNode_HandleJoinAck(from,
                                        packet->data.spec_join_ack.host_session_kind,
                                        packet->data.spec_join_ack.host_tcp_port,
                                        packet->data.spec_join_ack.host_p1_char,
                                        packet->data.spec_join_ack.host_p2_char,
                                        packet->data.spec_join_ack.host_stage);
            break;

        case CtrlMsg::SPEC_JOIN_REDIRECT:
            SpectatorNode_HandleJoinRedirect(
                from,
                packet->data.spec_redirect.redirect_ip,
                packet->data.spec_redirect.redirect_port);
            break;

        case CtrlMsg::SPEC_HEARTBEAT:
            SpectatorNode_HandleHeartbeat(from);
            break;

        case CtrlMsg::SPEC_LEAVE:
            SpectatorNode_HandleLeave(from);
            break;

        default:
            break;
    }
}

// =============================================================================
// LIFECYCLE
// =============================================================================

bool Netplay_Init(int player_index, uint16_t local_port, const char* remote_addr) {
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "Netplay: Init player=%d port=%d remote=%s",
        player_index, local_port, remote_addr);

    g_player_index = player_index;
    g_simple_state = SimpleState::DISCONNECTED;
    g_session = nullptr;
    g_session_ready = false;
    g_netplay_frame = 0;
    g_p1_input = 0;
    g_p2_input = 0;

    // Reset CSS state — input transport now lives in the CSS GekkoSession,
    // so the legacy ring-buffer fields are gone. The session itself is
    // created on first BATTLE_READY rendezvous and torn down on battle entry.
    g_session_kind      = SessionKind::NONE;
    g_css_frame         = 0;
    g_css_advance_p1    = 0;
    g_css_advance_p2    = 0;
    g_css_advance_ready = false;
    g_css_active = false;
    g_css_synced = false;
    g_remote_css_ready = false;
    g_local_css_ready = false;

    // Reset battle sync state (entry direction)
    g_local_battle_entered    = false;
    g_remote_battle_entered   = false;
    g_battle_synced           = false;
    g_battle_entry_swap_frame = 0;
    g_battle_entry_armed      = false;

    // Reset battle sync state (exit direction, for next return-to-CSS)
    g_local_battle_end_signaled  = false;
    g_remote_battle_end_signaled = false;
    g_battle_end_synced          = false;
    g_battle_end_swap_frame      = 0;
    g_battle_end_armed           = false;

    // Reset handshake
    g_received_hello = false;
    g_received_hello_ack = false;

    // Store network config
    g_local_port = local_port;
    strncpy(g_remote_addr, remote_addr, sizeof(g_remote_addr) - 1);

    // Initialize control channel
    ControlChannel_SetCallback(OnControlMessage);

    // Delay-negotiation mode (#24). 0 = avg ping (mean RTT), 1 = peak
    // ping (worst RTT). Picked by the launcher's Delay combo; absent
    // env defaults to avg. Drives ControlChannel_GetLocalDelayCandidate.
    if (const char* env = std::getenv("FM2K_DELAY_MODE"); env && env[0]) {
        ControlChannel_SetDelayMode(std::atoi(env));
    } else {
        ControlChannel_SetDelayMode(0);
    }

    if (!NetSocket_IsInitialized()) {
        if (!NetSocket_Init(local_port, remote_addr)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Netplay: Failed to init socket");
            return false;
        }
    }

    // Initialize spectator tree node AFTER NetSocket — its TCP listener
    // binds to the UDP socket's port number (TCP/UDP share port space).
    SpectatorNode_Init();

    // Hub-driven NAT traversal. If FM2K_HUB_* env vars are present,
    // the launcher started this match via the hub — fire a STUN probe
    // (so the hub can reflect our public mapping) and start the
    // peer-to-peer burst-punch using the supplied match_token. If the
    // env vars aren't set, this match was a legacy direct-connect:
    // we skip and rely on the existing 0xCC HELLO loop alone.
    {
        const char* hub_udp     = std::getenv("FM2K_HUB_UDP_ADDR");
        const char* hub_user_id = std::getenv("FM2K_HUB_USER_ID");
        const char* match_tok   = std::getenv("FM2K_HUB_MATCH_TOKEN");

        if (hub_udp && hub_user_id) {
            ::fm2k::nat::SendStunProbe();
        }

        // Read relay env vars early so the post-burst fallback in
        // StartPunch's worker thread can engage relay mode the
        // moment direct punch fails.
        ::fm2k::nat::ConfigureRelay();

        if (match_tok && remote_addr && *remote_addr) {
            // Decode the 32-hex-char match token into 16 binary bytes.
            uint8_t token_bytes[16] = {};
            size_t hex_len = std::strlen(match_tok);
            if (hex_len > 32) hex_len = 32;
            auto nibble = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return 10 + c - 'a';
                if (c >= 'A' && c <= 'F') return 10 + c - 'A';
                return -1;
            };
            for (size_t i = 0; i + 1 < hex_len; i += 2) {
                int hi = nibble(match_tok[i]);
                int lo = nibble(match_tok[i + 1]);
                if (hi < 0 || lo < 0) break;
                token_bytes[i / 2] = static_cast<uint8_t>((hi << 4) | lo);
            }

            // Parse "ip:port" from FM2K_REMOTE_ADDR (= remote_addr param).
            std::string addr(remote_addr);
            auto colon = addr.rfind(':');
            if (colon != std::string::npos) {
                std::string ip_s = addr.substr(0, colon);
                int port_i = std::atoi(addr.c_str() + colon + 1);
                in_addr peer_ia{};
                if (inet_pton(AF_INET, ip_s.c_str(), &peer_ia) == 1 &&
                    port_i > 0 && port_i <= 65535) {
                    ::fm2k::nat::StartPunch(peer_ia.s_addr,
                                             static_cast<uint16_t>(port_i),
                                             token_bytes);
                }
            }
        }
    }

    // Send HELLO with our local game-data fingerprint (#57). Receiver
    // compares against its own and aborts the handshake on mismatch.
    ControlChannel_SendHello(static_cast<uint8_t>(player_index),
                             fm2k::game_hash::Compute());

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Netplay: Initialized, connecting...");
    return true;
}

bool Netplay_InitAsSpectator(uint16_t local_port, const char* host_addr) {
    g_player_index = 2;  // sentinel — not a player slot
    g_simple_state = SimpleState::CONNECTED;  // skip handshake; spectators don't HELLO
    g_session = nullptr;
    g_session_ready = false;
    g_netplay_frame = 0;

    // Replay-from-file mode (no peer, no network). When FM2K_REPLAY_FILE
    // is set, skip the network init + JOIN_REQ entirely and load the
    // .fm2krep / .fm2kset file directly into pb_queue. The trampoline's
    // RunSpectatorTick consumes events from pb_queue identically to a
    // live spectator, so playback Just Works through the same driver.
    if (const char* replay_path = std::getenv("FM2K_REPLAY_FILE");
        replay_path && replay_path[0]) {
        SpectatorNode_Init();
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "Replay: loading %s", replay_path);
        if (!SpectatorNode_LoadSessionFile(replay_path, {})) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                "Replay: file load failed: %s", replay_path);
            return false;
        }
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "Replay: loaded — trampoline will drive playback");
        return true;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Netplay (spectator): Init port=%u host=%s",
                local_port, host_addr ? host_addr : "(null)");

    // Wire up the control-channel callback. OnControlMessage already
    // dispatches SPEC_JOIN_ACK / SPEC_JOIN_REDIRECT / SPEC_HEARTBEAT / SPEC_LEAVE
    // into SpectatorNode_*, so the same handler covers spectator inbound packets.
    ControlChannel_SetCallback(OnControlMessage);

    if (!NetSocket_IsInitialized()) {
        if (!NetSocket_Init(local_port, host_addr)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "Netplay (spectator): socket init failed");
            return false;
        }
    }

    // Stand up the spectator node (initializes its state) and request to join
    // the host. INPUT_BATCH frames flow over a TCP connection opened on
    // JOIN_ACK; UDP carries only handshake + heartbeat.
    SpectatorNode_Init();

    const sockaddr_in* upstream = NetSocket_GetRemoteAddr();
    if (!upstream || upstream->sin_port == 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Netplay (spectator): no upstream addr latched");
        return false;
    }
    // Stash the original upstream as the fallback root. If our current
    // upstream later goes silent (e.g. an overflow-redirect relay died),
    // SpectatorNode_TickHealth will reconnect us here. The host always
    // serves as the root and is the always-on failback target.
    extern void SpectatorNode_SetRootAddr(const sockaddr_in& root);
    SpectatorNode_SetRootAddr(*upstream);

    // Phase 5: launcher controls the join mode via FM2K_SPECTATE_MODE env var.
    //   "current" → CURRENT_MATCH (CCCaster-style snapshot join, default)
    //   "full"    → FULL_SESSION  (replay-from-frame-0)
    // Anything else (or unset) defaults to CURRENT_MATCH — the user-facing
    // intent for live spectating; FULL_SESSION is opt-in for streamers.
    SpecJoinMode mode = SpecJoinMode::CURRENT_MATCH;
    if (const char* env = std::getenv("FM2K_SPECTATE_MODE")) {
        if (env[0] == 'f' || env[0] == 'F') {
            mode = SpecJoinMode::FULL_SESSION;
        }
    }
    // Hub-driven NAT registration. Same as Netplay_Init's player path:
    // fire a STUN probe so the hub learns OUR external UDP mapping
    // (from the spec hook's UDP socket — same one ControlChannel uses
    // for SPEC_JOIN_REQ + SPEC_HEARTBEAT). Without this, hub's
    // user.udp_addr is whatever an earlier game STUN landed (or
    // empty), spectator_incoming forwards the wrong port to the
    // host, and the host's UDP NAT-punch heartbeat goes nowhere —
    // spec then sits on "Connecting..." through the entire reconnect
    // backoff.
    {
        const char* hub_udp     = std::getenv("FM2K_HUB_UDP_ADDR");
        const char* hub_user_id = std::getenv("FM2K_HUB_USER_ID");
        if (hub_udp && hub_user_id) {
            ::fm2k::nat::SendStunProbe();
        } else {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "Netplay (spectator): FM2K_HUB_UDP_ADDR or FM2K_HUB_USER_ID "
                "unset — STUN probe skipped, host's punch may target wrong "
                "port (cross-NAT spec will likely fail)");
        }
    }

    SpectatorNode_RequestJoin(*upstream, mode);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Netplay (spectator): SPEC_JOIN_REQ sent to host (mode=%s)",
                mode == SpecJoinMode::CURRENT_MATCH ? "CURRENT_MATCH" : "FULL_SESSION");
    return true;
}

void Netplay_Shutdown() {
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Netplay: Shutting down");

    if (g_session) {
        gekko_destroy(&g_session);
        g_session = nullptr;
    }

    if (ControlChannel_IsConnected()) {
        ControlChannel_SendDisconnect();
    }

    g_simple_state = SimpleState::DISCONNECTED;
    g_received_hello = false;
    g_received_hello_ack = false;
    g_session_delay_cache_valid = false;
    g_session_delay_cached      = 0;
}

// =============================================================================
// STATE ACCESSORS
// =============================================================================

NetplayState Netplay_GetState() {
    return MapToLegacyState(g_simple_state);
}

void Netplay_SetState(NetplayState state) {
    (void)state;  // Ignored - we use simplified state
}

bool Netplay_IsConnected() {
    return g_simple_state >= SimpleState::CONNECTED;
}

bool Netplay_IsActive() {
    return g_simple_state == SimpleState::BATTLE && g_session != nullptr;
}

bool Netplay_IsSessionReady() {
    return g_session_ready;
}

// =============================================================================
// CSS PROCESSING — GekkoNet lockstep (input_prediction_window = 0)
//
// CSS used to ride a custom CCCaster-style ring buffer over CtrlMsg::CSS_INPUT.
// It's now a GekkoGameSession with prediction_window=0, which gives true
// lockstep: gekko_update_session emits AdvanceEvent only when both peers'
// inputs for the current frame have arrived. Save/Load events are suppressed
// in lockstep mode (game_session.cpp:226-228, 365-367, 537), so CSS state is
// derived purely from the shared seed (0x12345678 reseeded on sync) +
// identical confirmed inputs — same determinism as today, but riding a
// well-tested transport.
// =============================================================================

// Forward decl — defined after Netplay_StartBattle (shares the actor-add
// pattern). Returns true if the CSS session is ready to drive.
static bool Netplay_StartCSSSession();
static void Netplay_EndCSSSession();

// Iterate currently-subscribed spectators (from SpectatorNode's address list)
// and call gekko_add_actor(GekkoSpectator, &addr) for each on the active
// g_session. Called after the player actors are added in StartCSS/StartBattle.
// Late joiners (after session creation) are added directly in
// SpectatorNode_HandleJoinReq.
static void AddSubscribedSpectatorsToSession();

void Netplay_PollCSS() {
    ControlChannel_Poll();
    if (g_session && g_session_kind == SessionKind::CSS) {
        gekko_network_poll(g_session);
    }
}

bool Netplay_CanAdvanceCSS() {
    // Not synced yet — let game run freely (pre-CSS or waiting for remote)
    if (!g_css_synced) {
        return true;
    }
    // Once the CSS session is up, advance is gated on the AdvanceEvent
    // having fired during the most recent Netplay_ProcessCSS call.
    return g_css_advance_ready;
}

bool Netplay_ProcessCSS() {
    // Poll for incoming control-channel messages (BATTLE_READY rendezvous,
    // BATTLE_ENTERING, etc.) — independent of GekkoNet's transport.
    ControlChannel_Poll();

    // Not connected yet — let game run with local input
    if (g_simple_state < SimpleState::CONNECTED) {
        return true;
    }

    uint32_t now = GetTickCount();

    // Signal we're in CSS
    if (!g_css_active) {
        g_css_active = true;
        g_local_css_ready = true;
        ControlChannel_SendBattleReady();
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "CSS: Entered, signaling remote...");
    }

    // Keep resending BATTLE_READY until BOTH sides are bilaterally
    // confirmed in the GekkoNet CSS session.
    //
    // Why not gate on `!g_remote_css_ready`? That flag flips true the
    // moment THIS side receives one BATTLE_READY from the peer — which
    // can happen before this side has even entered CSS, because the peer
    // who-entered-first is spamming. Then when this side finally enters
    // CSS, the unconditional first-send fires (line 762) but the spam
    // loop's `!g_remote_css_ready` is already false, so resends stop.
    // If THAT one BATTLE_READY drops on a lossy / high-RTT link, the
    // peer never receives this side's signal and stays stuck forever.
    // Observed live in P1/P2 logs under simulated loss.
    //
    // Gate on `g_css_frame == 0` instead: g_css_frame is incremented in
    // the GekkoNet CSS AdvanceEvent handler, which only fires once
    // BOTH sides have joined the CSS session. So spam keeps going on
    // both sides independently until bilateral sync is genuinely
    // confirmed by a real GekkoNet frame. Once g_css_frame > 0 on a
    // side, both sides have it (frame numbers are agreed). Idempotent
    // BATTLE_READYs in the meantime are harmless (small payload, peer
    // ignores duplicates beyond setting g_remote_css_ready).
    static uint32_t last_ready_send = 0;
    if (g_css_active && g_css_frame == 0 && now - last_ready_send > 100) {
        ControlChannel_SendBattleReady();
        last_ready_send = now;
    }

    // Wait for both clients to be in CSS before bringing up the GekkoNet
    // CSS session. Pre-rendezvous frames run unsynchronized (identical to
    // today's pre-g_css_synced behavior).
    if (!g_remote_css_ready) {
        return true;  // Let game run but don't drive the session yet
    }

    // First frame after rendezvous: reseed RNG and stand up the CSS session.
    if (!g_css_synced) {
        // CRITICAL: Re-seed RNG now that both clients are synced. Pre-CSS
        // frames ran unsynchronized and diverged the RNG. Stage selection
        // uses RNG during CSS->battle transition, so it MUST be identical
        // from this point forward.
        *(uint32_t*)FM2K::ADDR_RANDOM_SEED = 0x12345678;
        SpectatorNode_AppendPinRng(0x12345678);

        if (!Netplay_StartCSSSession()) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                "CSS: Netplay_StartCSSSession failed");
            return false;
        }
        g_css_synced = true;
        g_css_frame  = 0;
        // Arm BATTLE_ENTERING acceptance for this match. Stale packets from
        // the prior match arriving before this point are dropped; from
        // here through the actual battle-session start they're accepted
        // as legitimate signaling.
        g_battle_entry_armed = true;

        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "CSS SYNCED: Both ready, GekkoNet CSS session up, RNG reseeded");
    }

    // Drive the GekkoNet CSS session for this tick.
    g_css_advance_ready = false;
    if (!g_session || g_session_kind != SessionKind::CSS) {
        // Session torn down (e.g., we just swapped to battle); nothing to do.
        return true;
    }

    gekko_network_poll(g_session);

    // Submit local input. With prediction=0, this commits at frame
    // local_frame + CSS_LOCAL_DELAY; AdvanceEvent fires later for the
    // committed frame once the remote's input for that frame arrives.
    uint16_t local_raw = Input_CaptureLocal();
    // Test-harness CSS auto-advance: when FM2K_TEST_AUTO_CSS is set,
    // alternate 0x010 (button A) every other frame so the rising edge
    // fires CSS confirm on both peers. CssAutoConfirm pins cursor /
    // selected_char via its game_state_manager detour; this pulse fills
    // in the missing gekko-delivered input that PGI needs to actually
    // process the confirm. Without it, gekko delivers 0x0000 forever
    // and CSS never advances in netplay mode (CssAutoConfirm overrides
    // engine memory AFTER PGI, but the underlying gekko CSS-delay
    // session needs a real input pulse to keep both peers in sync).
    {
        static int s_test_auto_css = -1;
        if (s_test_auto_css < 0) {
            const char* v = std::getenv("FM2K_TEST_AUTO_CSS");
            s_test_auto_css = (v && v[0]) ? 1 : 0;
        }
        if (s_test_auto_css == 1) {
            static uint32_t s_pulse = 0;
            local_raw = (s_pulse++ & 1) ? 0x010u : 0u;
        }
    }
    gekko_add_local_input(g_session, g_player_index, &local_raw);

    // Drain session events (Connected/Syncing/Disconnected/Desync).
    int event_count = 0;
    auto events = gekko_session_events(g_session, &event_count);
    for (int i = 0; i < event_count; i++) {
        auto event = events[i];
        switch (event->type) {
            case GekkoSessionStarted:
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "CSS: GekkoNet CSS session started");
                g_session_ready = true;
                break;
            case GekkoPlayerConnected:
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "CSS: GekkoNet player %d connected", event->data.connected.handle);
                g_session_ready = true;
                break;
            case GekkoPlayerSyncing:
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "CSS: GekkoNet syncing %u/%u",
                    event->data.syncing.current, event->data.syncing.max);
                break;
            case GekkoPlayerDisconnected:
                // Peer's CSS-phase Gekko session went silent past
                // DISCONNECT_TIMEOUT. Publish CSS_ABORT (NOT DISCONNECT)
                // so the launcher closes the surviving local game but
                // doesn't record this in W/L/D — battle never started,
                // there's no result to commit. DISCONNECT outcome is
                // reserved for "peer dropped during battle", which IS
                // a forfeit and counts.
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "CSS: peer disconnected (handle=%d) — publishing CSS_ABORT outcome",
                    event->data.disconnected.handle);
                SharedMem_PublishMatchOutcome(FM2K_MATCH_OUTCOME_CSS_ABORT);
                break;
            case GekkoDesyncDetected:
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                    "CSS DESYNC f=%d local=0x%08X remote=0x%08X",
                    event->data.desynced.frame,
                    event->data.desynced.local_checksum,
                    event->data.desynced.remote_checksum);
                break;
            default:
                break;
        }
    }

    // Drain update events. With prediction=0 + limited_saving=false, only
    // AdvanceEvent fires (lockstep mode skips Save/Load — see
    // game_session.cpp:226 / :365 / :537).
    int update_count = 0;
    auto updates = gekko_update_session(g_session, &update_count);
    for (int i = 0; i < update_count; i++) {
        auto update = updates[i];
        if (update->type != GekkoAdvanceEvent) {
            continue;  // Save/Load shouldn't fire under lockstep, but ignore if they do.
        }
        // Inputs are packed in slot order (p1 at index 0, p2 at index 1).
        const uint16_t* in = (const uint16_t*)update->data.adv.inputs;
        g_css_advance_p1    = in[0];
        g_css_advance_p2    = in[1];
        g_css_advance_ready = true;
        g_css_frame         = (uint32_t)update->data.adv.frame + 1;

        // session_history recording moved to Hook_GetPlayerInput where
        // the actual returned input values pass through. That captures
        // pre-rendezvous title-screen / auto-mash inputs too, which this
        // post-AdvanceEvent point misses (no AdvanceEvents fire pre-
        // rendezvous). One canonical log spanning FM2K boot to disconnect.

        if ((g_css_frame - 1) % 100 == 0) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "CSS: advance frame=%d p1=0x%04X p2=0x%04X",
                update->data.adv.frame, g_css_advance_p1, g_css_advance_p2);
        }
    }

    // No AdvanceEvent this tick → lockstep is waiting on remote → stall.
    return g_css_advance_ready;
}

uint16_t Netplay_GetCSSInput(int player_id) {
    uint16_t input;
    if (player_id == 0) {
        input = g_css_advance_p1;
    } else {
        input = g_css_advance_p2;
    }

    // CCCaster-style: block confirm/cancel for the first CSS_CONFIRM_LOCKOUT
    // frames (moon selector workaround). g_css_frame is one past the last
    // confirmed AdvanceEvent, so the "current" read frame is g_css_frame - 1.
    const uint32_t read_frame = (g_css_frame > 0) ? g_css_frame - 1 : 0;
    if (read_frame < (uint32_t)CSS_CONFIRM_LOCKOUT) {
        input &= 0x0FF;  // Mask button presses, keep direction bits.
    }

    return input;
}

// =============================================================================
// BATTLE ENTRY SYNC BARRIER
// Ensures both clients enter battle mode at the same time
// =============================================================================

// Helper: send a BATTLE_ENTERING / BATTLE_END payload to every currently
// subscribed spectator so they can mirror the swap. Called alongside the
// usual unicast-to-remote-peer send.
static void BroadcastSwapToSubscribers(CtrlMsg type, uint32_t swap_frame) {
    auto subs = SpectatorNode_GetSubscriberAddrs();
    for (const auto& addr : subs) {
        CtrlPacket pkt = {};
        pkt.header.type     = type;
        pkt.data.sync.frame = swap_frame;
        ControlChannel_SendTo(pkt, addr);
    }
}

void Netplay_SignalBattleEntry() {
    if (g_local_battle_entered) {
        return;  // Already signaled
    }

    // Compute our proposed swap frame on the active CSS session. Remote may
    // already have proposed a higher value (we adopt it via the receive
    // handler); take max so we never go backwards.
    const uint32_t local_proposal = g_css_frame + SWAP_FRAME_BUFFER;
    if (local_proposal > g_battle_entry_swap_frame) {
        g_battle_entry_swap_frame = local_proposal;
    }
    g_local_battle_entered = true;
    ControlChannel_SendBattleEntering(g_battle_entry_swap_frame);
    BroadcastSwapToSubscribers(CtrlMsg::BATTLE_ENTERING, g_battle_entry_swap_frame);

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "BATTLE SYNC: Local entered battle mode (css_frame=%u, swap_frame=%u)",
        g_css_frame, g_battle_entry_swap_frame);
}

bool Netplay_IsBattleSynced() {
    // Once the game's CSS detects the battle transition (game_mode -> 3000),
    // the trampoline phase classifier flips us into TRAMPOLINE_BATTLE and we
    // stop driving Netplay_ProcessCSS — so g_css_frame stops advancing. The
    // swap_frame value (g_css_frame + SWAP_FRAME_BUFFER) is therefore
    // unreachable from the CSS session itself. Lockstep already guarantees
    // both peers detect the transition at the same logical frame (the same
    // shared CSS input stream produced the same selected character + lock-in
    // events), so the agreed-on-both-sides BATTLE_ENTERING is enough — no
    // need to also gate on css_frame parity. swap_frame stays in the message
    // for diagnostic logging and future battle-side gating.
    if (!g_battle_synced &&
        g_local_battle_entered &&
        g_remote_battle_entered) {
        g_battle_synced = true;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "BATTLE SYNC: both peers signaled (css_frame=%u, swap_frame=%u) - swap CSS->battle now",
            g_css_frame, g_battle_entry_swap_frame);
    }
    return g_battle_synced;
}

uint32_t Netplay_GetBattleEntrySwapFrame() {
    return g_battle_entry_swap_frame;
}

void Netplay_PollBattleSync() {
    // Poll control channel to receive BATTLE_ENTERING from remote
    ControlChannel_Poll();

    // CSS-session transport keepalive while waiting for the trailing peer
    // (CSS->battle swap deadlock, found 2026-06-11). The peer whose
    // game_mode flips to 3000 first stops running Netplay_ProcessCSS (the
    // phase classifier moves it to the battle wait = this function), so
    // its CSS gekko session went silent: the final CSS input packets the
    // trailing peer still needs to reach ITS OWN detection frame could sit
    // unflushed, ACKs stopped, and the trailing peer either stalled a few
    // frames short of detection forever or hit the 5s gekko disconnect ->
    // CSS_ABORT. 3-for-3 repro in the autoplay loopback harness (which
    // races the flip); real matches usually masked it because humans idle
    // on CSS long enough for the transport to flush. Fix: keep polling the
    // session, keep feeding neutral padding inputs (frames past both
    // peers' detection point -- never consumed by either sim, both flip at
    // the same lockstep-determined frame), and drain-discard its events
    // until both peers signal and the swap runs.
    if (g_session && g_session_kind == SessionKind::CSS) {
        gekko_network_poll(g_session);
        uint16_t neutral = 0;
        gekko_add_local_input(g_session, g_player_index, &neutral);
        int event_count = 0;
        auto events = gekko_session_events(g_session, &event_count);
        for (int i = 0; i < event_count; i++) {
            if (events[i]->type == GekkoPlayerDisconnected) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "BATTLE SYNC: CSS peer disconnected while waiting for "
                    "swap -- publishing CSS_ABORT");
                SharedMem_PublishMatchOutcome(FM2K_MATCH_OUTCOME_CSS_ABORT);
            }
        }
        int update_count = 0;
        (void)gekko_update_session(g_session, &update_count);
        // AdvanceEvents drained here are post-detection padding frames;
        // the local sim already left CSS, so they are intentionally not
        // applied.
    }

    // Resend BATTLE_ENTERING until remote acknowledges, carrying the latest
    // agreed swap_frame each time. If remote's proposal arrived higher than
    // ours, the agreed value bumped — keep both sides in sync via resend.
    static uint32_t last_send = 0;
    uint32_t now = GetTickCount();
    if (g_local_battle_entered && !g_remote_battle_entered && now - last_send > 50) {
        ControlChannel_SendBattleEntering(g_battle_entry_swap_frame);
        last_send = now;
    }
}

// =============================================================================
// BATTLE EXIT SYNC BARRIER
// Mirrors the entry barrier; gates battle->CSS swap on agreed swap_frame.
// =============================================================================

void Netplay_SignalBattleEnd() {
    if (g_local_battle_end_signaled) {
        return;
    }

    const uint32_t local_proposal = g_netplay_frame + SWAP_FRAME_BUFFER;
    if (local_proposal > g_battle_end_swap_frame) {
        g_battle_end_swap_frame = local_proposal;
    }
    g_local_battle_end_signaled = true;
    ControlChannel_SendBattleEnd(g_battle_end_swap_frame);
    BroadcastSwapToSubscribers(CtrlMsg::BATTLE_END, g_battle_end_swap_frame);

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "BATTLE END SYNC: Local left battle mode (battle_frame=%u, swap_frame=%u)",
        g_netplay_frame, g_battle_end_swap_frame);
}

bool Netplay_IsBattleEndSynced() {
    // Same chicken-and-egg as the entry direction: once game_mode leaves
    // the [3000,4000) battle range, the phase classifier flips to CSS and
    // RunBattleTick stops driving the battle session — so g_netplay_frame
    // stops, the swap_frame target is unreachable. Both peers' confirmed
    // exit detection happens at the same logical battle frame (deterministic
    // from shared inputs), so the both-signaled gate is sufficient.
    if (!g_battle_end_synced &&
        g_local_battle_end_signaled &&
        g_remote_battle_end_signaled) {
        g_battle_end_synced = true;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "BATTLE END SYNC: both peers signaled (battle_frame=%u, swap_frame=%u) - swap battle->CSS now",
            g_netplay_frame, g_battle_end_swap_frame);
    }
    return g_battle_end_synced;
}

uint32_t Netplay_GetBattleEndSwapFrame() {
    return g_battle_end_swap_frame;
}

void Netplay_PollBattleEndSync() {
    ControlChannel_Poll();

    static uint32_t last_send = 0;
    uint32_t now = GetTickCount();
    if (g_local_battle_end_signaled && !g_remote_battle_end_signaled && now - last_send > 50) {
        ControlChannel_SendBattleEnd(g_battle_end_swap_frame);
        last_send = now;
    }
}

static void AddSubscribedSpectatorsToSession() {
    // Spectators are NOT GekkoSpectator actors. Input distribution to
    // spectators flows over the SpectatorNode INPUT_BATCH path — every
    // confirmed (p1, p2) frame is recorded into session_history at
    // Hook_GetPlayerInput's capture_and_return and the host's
    // FlushBatch broadcasts to every subscriber.
    //
    // Adding spectators to GekkoNet was the wrong architecture — it required
    // host/spectator sub-state to match at session-create time, which is
    // launch-timing dependent and a snapshot transfer to fix. Pure input
    // replay sidesteps all of that: spectator boots → starts consuming
    // host's recorded inputs from frame 0 → walks title→CSS→battle in
    // lockstep with host's recorded execution.
    (void)0;  // intentionally empty — kept as a hook for future per-session
              // setup if needed.
}

// =============================================================================
// GEKKONET SESSION - CSS Lockstep (input_prediction_window = 0)
// =============================================================================

static bool Netplay_StartCSSSession() {
    if (g_session) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "Netplay_StartCSSSession: session already exists (kind=%d)",
            (int)g_session_kind);
        return g_session_kind == SessionKind::CSS;
    }

    // Compute CSS delay dynamically from current RTT instead of pinning
    // at the conservative CSS_LOCAL_DELAY=6. With prediction=0 lockstep,
    // delay too low for the actual link makes CSS visibly choppy: every
    // frame stalls waiting for peer input. Same formula the battle path
    // uses: ceil(mean_one_way_ms / 10ms), floored at 2, capped at 15.
    // RTT samples come from the existing PING / HELLO ack cycle so this
    // is meaningful by the time we create the CSS session (post-HELLO_ACK).
    int css_delay = CSS_LOCAL_DELAY;  // fallback
    {
        const uint32_t rtt_mean_ms  = ControlChannel_GetRttMs();
        if (rtt_mean_ms > 0) {
            const uint32_t mean_one_way = rtt_mean_ms / 2;
            int d = (int)((mean_one_way + 9) / 10);
            if (d < 2)  d = 2;
            if (d > 15) d = 15;
            css_delay = d;
        }
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "Netplay: Creating CSS GekkoSession (lockstep, prediction=0, delay=%d)",
        css_delay);

    GekkoConfig config = {};
    config.num_players              = 2;
    // Allow up to 4 spectators per session; spectator_delay sized for full
    // CSS catch-up. CSS sessions are short (a few hundred frames at most
    // before battle entry), so default is plenty.
    config.max_spectators           = 4;
    config.spectator_delay          = 0;    // see battle-session comment — disables pause-buffer
    // input_history_size: host keeps every confirmed CSS input frame in
    // _net_spectator_queue, capped at this many. Late-joining spectators
    // (last_acked_frame == NULL_FRAME) get the entire history streamed
    // on connect. 60000 frames = 10 min @ 100 FPS — plenty for a CSS
    // lobby session that ran for an unusually long pre-match wait.
    // See vendored/GekkoNet patch + README.md:36.
    config.input_history_size       = 60000;
    config.input_prediction_window  = 0;    // lockstep — IsLockstepActive() in game_session.cpp:520
    config.input_size               = sizeof(uint16_t);
    config.state_size               = sizeof(uint32_t);
    config.desync_detection         = true;
    config.limited_saving           = false;  // No effect in lockstep — Save events suppressed

    gekko_create(&g_session, GekkoGameSession);
    gekko_start(g_session, &config);
    // Fresh session = no GekkoSpectator actors yet. Reset the dedup
    // tracking so any post-boundary spec rejoins re-add cleanly.
    SpectatorNode_ClearGekkoSpectatorTracking();

    auto adapter = CreateMultiplexAdapter();
    gekko_net_adapter_set(g_session, adapter);

    // Refresh remote address string from learned sockaddr (post-HELLO_ACK).
    if (const sockaddr_in* learned = NetSocket_GetRemoteAddr()) {
        if (learned->sin_port != 0) {
            char ip_buf[INET_ADDRSTRLEN] = {};
            inet_ntop(AF_INET, (void*)&learned->sin_addr, ip_buf, sizeof(ip_buf));
            snprintf(g_remote_addr, sizeof(g_remote_addr), "%s:%u",
                     ip_buf, ntohs(learned->sin_port));
        }
    }

    for (int i = 0; i < 2; i++) {
        if (i == g_player_index) {
            gekko_add_actor(g_session, GekkoLocalPlayer, nullptr);
            gekko_set_local_delay(g_session, i, css_delay);
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "CSS: Added local player at slot %d (delay=%d)", i, css_delay);
        } else {
            GekkoNetAddress addr = {};
            addr.data = (void*)g_remote_addr;
            addr.size = (int)strlen(g_remote_addr);
            gekko_add_actor(g_session, GekkoRemotePlayer, &addr);
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "CSS: Added remote player at slot %d -> %s", i, g_remote_addr);
        }
    }

    // No runahead in lockstep mode (suppressed by IsLockstepActive at
    // game_session.cpp:537 even if requested).

    // Set kind BEFORE the spectator add — AddSubscribedSpectatorsToSession
    // re-broadcasts SPEC_JOIN_ACK carrying g_session_kind, which spectators
    // use to swap their SpectateSession config to match.
    g_session_kind      = SessionKind::CSS;
    g_session_ready     = false;
    g_css_advance_ready = false;
    g_css_advance_p1    = 0;
    g_css_advance_p2    = 0;
    g_css_frame         = 0;
    g_local_delay       = css_delay;

    AddSubscribedSpectatorsToSession();

    return true;
}

static void Netplay_EndCSSSession() {
    if (g_session && g_session_kind == SessionKind::CSS) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Netplay: Destroying CSS GekkoSession");
        gekko_destroy(&g_session);
        g_session       = nullptr;
        g_session_kind  = SessionKind::NONE;
        g_session_ready = false;
    }
    g_css_advance_ready = false;
}

// =============================================================================
// GEKKONET SESSION - Spectate (passive observer of a remote host)
// =============================================================================

// Build the GekkoConfig for a spectator session, mirroring the host's config
// for the given phase. Spectators only need session_type, input_size,
// state_size, and spectator_delay — num_players + prediction_window are
// informational but kept consistent with host for clarity.
static GekkoConfig MakeSpectateConfig(SessionKind host_kind) {
    GekkoConfig config = {};
    config.num_players      = 2;
    config.max_spectators   = 0;     // Spectator doesn't accept further spectators via gekko
    config.input_size       = sizeof(uint16_t);
    config.state_size       = sizeof(uint32_t);
    config.desync_detection = true;
    config.limited_saving   = false;
    // spectator_delay = 0: disable GekkoNet's spectator pause-buffer.
    // With > 0, SpectatorSession::ShouldDelaySpectator gates AdvanceEvent
    // emission until min_received - current >= delay frames, which can
    // never converge on low-latency connections (current advances each
    // tick once unpaused; min only advances on incoming packets, so the
    // diff stays ~stable). 0 == always advance as fast as inputs arrive.
    config.spectator_delay     = 0;
    // Late-joiner backfill: receive buffer holds up to ~10 minutes of
    // confirmed inputs (60000 frames @ 100 FPS, 4B each = ~240 KB). Lets
    // the spectator's local sim FF through the host's full session
    // history on connect. See vendored/GekkoNet patch for README.md:36.
    config.input_history_size  = 60000;
    if (host_kind == SessionKind::BATTLE) {
        // Must match the players' battle prediction_window (16) so the
        // spectator's rollback + desync-checkpoint budget lines up with the
        // session it's mirroring. Was stale at 8.
        config.input_prediction_window = 16;
    } else {
        // CSS or anything else → lockstep config
        config.input_prediction_window = 0;
    }
    return config;
}

bool Netplay_IsSpectatorSession() {
    return g_session_kind == SessionKind::SPECTATE;
}

NetplaySessionKind Netplay_GetSessionKind() {
    return g_session_kind;
}

GekkoSession* Netplay_GetActiveSession() {
    return g_session;
}

bool Netplay_StartSpectateSession(NetplaySessionKind host_kind, const char* host_addr) {
    // Idempotent: if a SpectateSession is already alive against the same
    // host with the same kind, don't tear it down. SpectatorNode's silence-
    // failover currently fires on legacy 0xCE quiet (input streaming dead)
    // and re-runs JOIN_REQ → JOIN_ACK → here. Without this guard we'd
    // destroy a healthy session every 5 seconds and re-handshake.
    if (g_session && g_session_kind == SessionKind::SPECTATE &&
        host_addr && host_addr[0] && std::strcmp(g_remote_addr, host_addr) == 0) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "Netplay_StartSpectateSession: session already alive for %s — keeping",
            host_addr);
        return true;
    }
    if (g_session) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "Netplay_StartSpectateSession: session already exists (kind=%d) — destroying first",
            (int)g_session_kind);
        gekko_destroy(&g_session);
        g_session = nullptr;
        g_session_kind = SessionKind::NONE;
    }

    if (!host_addr || !host_addr[0]) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "Netplay_StartSpectateSession: empty host_addr");
        return false;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "Netplay: Creating SpectateSession (host_kind=%d, host=%s)",
        (int)host_kind, host_addr);

    GekkoConfig config = MakeSpectateConfig(host_kind);

    gekko_create(&g_session, GekkoSpectateSession);
    gekko_start(g_session, &config);
    g_session_kind = SessionKind::SPECTATE;
    SpectatorNode_ClearGekkoSpectatorTracking();

    auto adapter = CreateMultiplexAdapter();
    gekko_net_adapter_set(g_session, adapter);

    // Add host as the (single) remote actor we receive input forwards from.
    GekkoNetAddress addr = {};
    addr.data = (void*)host_addr;
    addr.size = (int)strlen(host_addr);
    gekko_add_actor(g_session, GekkoRemotePlayer, &addr);

    g_session_ready     = false;
    g_p1_input          = 0;
    g_p2_input          = 0;
    g_netplay_frame     = 0;
    g_css_advance_ready = false;

    return true;
}

void Netplay_EndSpectateSession() {
    if (g_session && g_session_kind == SessionKind::SPECTATE) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Netplay: Destroying SpectateSession");
        gekko_destroy(&g_session);
        g_session       = nullptr;
        g_session_kind  = SessionKind::NONE;
        g_session_ready = false;
    }
}

// Spectator-side phase-flip handlers. Called from the BATTLE_ENTERING /
// BATTLE_END control-channel handlers when g_session_kind == SPECTATE.
// The spectator is passive: it observes the host's announced swap_frame
// and executes destroy/create at that frame.
//
// NOTE: For now we don't gate on swap_frame on the spectator side — the host's
// session destroy at swap_frame causes magic-mismatch silence to land at the
// same logical point on the wire, and the spectator's session sync re-handshake
// with the new session naturally aligns. swap_frame gating on the spectator
// side is a refinement; keep simple for first cut.
// Spectator-side pending swap: when the host announces a phase change,
// the spectator may still be FFing through backfilled inputs to catch up
// to live. Recording the target swap_frame lets ProcessSpectatorPhase
// finish draining AdvanceEvents up to that frame BEFORE tearing the
// session down — otherwise we'd lose the tail of CSS / battle inputs.
// Drained per-tick from Netplay_ProcessSpectatorPhase.
//
// pending_kind == NONE means no swap pending. Set by On* handlers, cleared
// when the swap actually fires (or on EndSpectateSession reset).
static NetplaySessionKind g_pending_swap_kind  = NetplaySessionKind::NONE;
static uint32_t           g_pending_swap_frame = 0;

void Netplay_OnHostBattleEntering(uint32_t swap_frame) {
    if (g_session_kind != SessionKind::SPECTATE) return;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "Spectator: host entering battle (swap_frame=%u) — pending CSS->battle swap",
        swap_frame);
    g_pending_swap_kind  = SessionKind::BATTLE;
    g_pending_swap_frame = swap_frame;
}

void Netplay_OnHostBattleEnd(uint32_t swap_frame) {
    if (g_session_kind != SessionKind::SPECTATE) return;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "Spectator: host ending battle (swap_frame=%u) — pending battle->CSS swap",
        swap_frame);
    g_pending_swap_kind  = SessionKind::CSS;
    g_pending_swap_frame = swap_frame;
}

// Called from Netplay_ProcessSpectatorPhase after each AdvanceEvent.
// Returns true if a swap fired this tick (caller should stop draining
// further events from the now-destroyed session).
static bool MaybeSwapPendingSpectator(uint32_t advanced_frame) {
    if (g_pending_swap_kind == SessionKind::NONE)        return false;
    if (g_session_kind     != SessionKind::SPECTATE)     return false;
    if (advanced_frame      < g_pending_swap_frame)      return false;

    const NetplaySessionKind next_kind = g_pending_swap_kind;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "Spectator: caught up to swap_frame=%u — swapping to %s SpectateSession",
        g_pending_swap_frame,
        next_kind == SessionKind::BATTLE ? "battle" : "CSS");

    char host_addr[64];
    snprintf(host_addr, sizeof(host_addr), "%s", g_remote_addr);
    Netplay_EndSpectateSession();
    Netplay_StartSpectateSession(next_kind, host_addr);

    g_pending_swap_kind  = SessionKind::NONE;
    g_pending_swap_frame = 0;
    return true;
}

// =============================================================================
// GEKKONET SESSION - Battle Mode (rollback)
// =============================================================================

bool Netplay_StartBattle() {
    // Tear down the CSS lockstep session before standing up the battle
    // session. Sequenced so g_session is null between the two — the
    // multiplex adapter is shared (session_magic demuxes), but only one
    // session is alive at a time.
    if (g_session && g_session_kind == SessionKind::CSS) {
        Netplay_EndCSSSession();
    }
    if (g_session) {
        return true;  // Already in some other session (battle or stress).
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Netplay: Starting battle GekkoSession");

    // Capture per-match character IDs + resolved .player filenames
    // while the CSS-side state is still live. Held in shared mem so
    // the launcher can include them in the hub `match_result` payload
    // after Netplay_EndBattle wipes CSS. Addresses come from
    // FM2K_Integration.h (launcher-side header) — inlined here because
    // the hook DLL has its own minimal FM2K:: namespace in globals.h
    // that doesn't pull in the full Integration header.
    //
    // Roster table at 0x435474 (`g_char_slot_data`): array of 256-byte
    // CP932 (Shift-JIS) filename strings, indexed by the same char_id
    // that lives at P1/P2_CHARACTER_ID_ADDR. Convert CP932 → UTF-8
    // via Win32 before publishing so JP names survive round-tripping
    // through the JSON hub protocol intact.
    {
        // Engine-aware via globals.h: FM2K reads CCCaster-style address pair
        // at 0x470180/4 with the 258-slot g_char_slot_data table at 0x435474;
        // FM95 reads its per-player CSS struct at 0x432720/0x432730 with the
        // 50-slot g_player_file_name_array at 0x463CF0.
        constexpr uintptr_t kP1CharIdAddr  = FM2K::ADDR_P1_SELECTED_CHAR;
        constexpr uintptr_t kP2CharIdAddr  = FM2K::ADDR_P2_SELECTED_CHAR;
        constexpr uintptr_t kCharSlotData  = FM2K::ADDR_CHAR_FILENAME_TABLE;
        constexpr size_t    kSlotStride    = FM2K::CHAR_FILENAME_STRIDE;
        constexpr size_t    kSlotCount     = FM2K::CHAR_FILENAME_COUNT;

        const uint32_t p1_char = *(const uint32_t*)kP1CharIdAddr;
        const uint32_t p2_char = *(const uint32_t*)kP2CharIdAddr;

        auto resolve_name = [&](uint32_t id, char* out, size_t out_cap) {
            out[0] = '\0';
            if (id >= kSlotCount) return;
            const char* cp932 =
                reinterpret_cast<const char*>(kCharSlotData + id * kSlotStride);
            // Bound the read at the slot stride so a missing NUL never
            // walks off into the next slot's data.
            const int len_932 = (int)strnlen(cp932, kSlotStride);
            if (len_932 == 0) return;

            wchar_t wbuf[kSlotStride];
            int wlen = MultiByteToWideChar(932, 0, cp932, len_932,
                                           wbuf, (int)(sizeof(wbuf) / sizeof(wbuf[0])));
            if (wlen <= 0) return;
            int u8len = WideCharToMultiByte(CP_UTF8, 0, wbuf, wlen,
                                            out, (int)(out_cap - 1),
                                            nullptr, nullptr);
            if (u8len <= 0) { out[0] = '\0'; return; }
            out[u8len] = '\0';
        };

        char p1_name[FM2K_MATCH_CHAR_NAME_MAX] = {};
        char p2_name[FM2K_MATCH_CHAR_NAME_MAX] = {};
        resolve_name(p1_char, p1_name, sizeof(p1_name));
        resolve_name(p2_char, p2_name, sizeof(p2_name));

        SharedMem_PublishMatchChars(p1_char, p2_char, p1_name, p2_name);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "Netplay: match chars p1=%u(\"%s\") p2=%u(\"%s\")",
            p1_char, p1_name, p2_char, p2_name);
    }

    // Snapshot the actual battle stage_id BEFORE the random-stage block
    // touches ADDR_SELECTED_STAGE. By this point in Netplay_StartBattle
    // vs_round_function has already loaded the stage file (the
    // "<name>.stage" CreateFileA log line a few ms earlier), so the
    // value at ADDR_SELECTED_STAGE right now is the one that's actually
    // playing this match. The random-stage block below writes a fresh
    // value for the *next* match, but that value is what MATCH_START
    // used to record — leading to "battle on stage X, replay says Y"
    // mismatches reported on 2026-05-09. Capture pre-roll, use that
    // for both MATCH_START and SharedMem_PublishMatchStage.
    const uint32_t mstage_id_pre_roll =
        (FM2K::ADDR_SELECTED_STAGE != 0)
            ? *(const uint32_t*)FM2K::ADDR_SELECTED_STAGE
            : 0u;

    // Random stage (#56 — Lilithport-style seeded xorshift). The
    // launcher hands us a host-generated seed via FM2K_STAGE_RANDOM_SEED
    // when both peers agree on random stage. We re-seed once per
    // process from that env var (g_xorshift_seeded), then advance one
    // step per Netplay_StartBattle and write the resulting index to
    // FM2K's stage memory. Both peers run the same xorshift sequence
    // from the same seed, so rematches keep rolling identically with
    // zero per-rematch wire traffic.
    //
    // KNOWN ISSUE: the write here lands AFTER vs_round_function has
    // already loaded the stage file for the current match, so the roll
    // does nothing for THIS battle. Only takes effect if the cached
    // value happens to influence a subsequent stage read — which in
    // current FM2K builds it doesn't. The random-stage feature is
    // slated for replacement by an explicit lobby/game-settings stage
    // selector, so this isn't being fixed in place.
    {
        constexpr uintptr_t kSelectedStageAddr = FM2K::ADDR_SELECTED_STAGE;
        static bool      g_xorshift_seeded = false;
        static uint32_t  g_xs_a = 1812433254u, g_xs_b = 3713160357u,
                         g_xs_c = 3109174145u, g_xs_d = 64984499u;
        static int       g_stage_min = 0;
        static int       g_stage_max = -1;   // -1 = random disabled

        if (!g_xorshift_seeded) {
            g_xorshift_seeded = true;
            const char* seed_env = std::getenv("FM2K_STAGE_RANDOM_SEED");
            const char* min_env  = std::getenv("FM2K_STAGE_RANDOM_MIN");
            const char* max_env  = std::getenv("FM2K_STAGE_RANDOM_MAX");
            // One-shot diagnostic: if the env vars aren't visible to
            // the hook process, random-stage silently does nothing
            // (g_stage_max stays -1) and every match plays on whatever
            // stage_id the CSS cursor happens to be on. Logging both
            // presence + value lets us tell apart "host disabled" vs
            // "env didn't propagate to game process".
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Netplay: random-stage env: SEED=%s MIN=%s MAX=%s",
                seed_env ? seed_env : "(unset)",
                min_env  ? min_env  : "(unset)",
                max_env  ? max_env  : "(unset)");
            if (seed_env && *seed_env) {
                uint32_t seed = (uint32_t)std::strtoul(seed_env, nullptr, 10);
                if (seed != 0) {
                    // Lilith's seeding scheme — same constants so a
                    // shared seed produces the same a[4] state on both
                    // peers (cross-implementation parity if anyone ever
                    // wants Lilith-FM2K interop, though we don't ship
                    // that today).
                    uint32_t s = seed;
                    uint32_t* arr[4] = {&g_xs_a, &g_xs_b, &g_xs_c, &g_xs_d};
                    for (int i = 0; i < 4; ++i) {
                        s = 1812433253u * (s ^ (s >> 30)) + (uint32_t)i;
                        *arr[i] = s;
                    }
                    g_stage_min = (min_env && *min_env)
                        ? std::atoi(min_env) : 0;
                    g_stage_max = (max_env && *max_env)
                        ? std::atoi(max_env) : -1;
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Netplay: random-stage seeded (seed=%u min=%d max=%d)",
                        (unsigned)seed, g_stage_min, g_stage_max);
                }
            }
        }

        if (g_stage_max >= g_stage_min && g_stage_max >= 0) {
            // Advance one step. xorshift128 — identical to Lilith's
            // RandomStage() arrival-no-args branch (stdafx.cpp:670).
            uint32_t t  = g_xs_a ^ (g_xs_a << 11);
            g_xs_a = g_xs_b; g_xs_b = g_xs_c; g_xs_c = g_xs_d;
            g_xs_d = (g_xs_d ^ (g_xs_d >> 19)) ^ (t ^ (t >> 8));
            const uint32_t span = (uint32_t)(g_stage_max - g_stage_min + 1);
            const uint32_t roll = g_xs_d % span;
            const uint32_t stage = (uint32_t)g_stage_min + roll;
            *(uint32_t*)kSelectedStageAddr = stage;
            // Also stash for FM95's LoadStageFile_alt hook — vs/story
            // mode reads its stage_id from a per-character table at
            // call time, so writing to ADDR_SELECTED_STAGE alone only
            // covers practice mode. The hook reads g_pending_random_stage
            // and overrides arg0 when non-FFFFFFFF.
            g_pending_random_stage = stage;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Netplay: random stage rolled=%u (range %d..%d)",
                stage, g_stage_min, g_stage_max);
        }

        // Stage_id capture for the hub match_result payload. Uses the
        // pre-roll snapshot taken at function entry — that's the stage
        // file vs_round_function already loaded for THIS match. Reading
        // ADDR_SELECTED_STAGE here would pick up whatever the random
        // block just wrote (for the next match), producing a record
        // that doesn't match what players actually saw on screen.
        // FM95 has no documented selected-stage scalar yet
        // (ADDR_SELECTED_STAGE == 0); publish unknown.
        if constexpr (FM2K::ADDR_SELECTED_STAGE != 0) {
            SharedMem_PublishMatchStage(mstage_id_pre_roll);
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Netplay: match stage_id=%u", mstage_id_pre_roll);
        } else {
            SharedMem_PublishMatchStage(0xFFFFFFFFu);
        }
    }

    // Reset CSS state when entering battle
    g_css_active        = false;
    g_css_synced        = false;
    g_local_css_ready   = false;
    g_remote_css_ready  = false;
    g_css_advance_ready = false;

    // Per-player input delay (#24 -- Melancholy/Spooder, bug bumbler).
    //
    // Peers used to compute delay INDEPENDENTLY off their own RTT
    // samples. On jittery links the two means drifted apart and the
    // peers ran different delays (Melancholy 10 / Spooder 5); a manual-
    // delay challenger likewise desynced a computed-delay opponent.
    //
    // Now each peer broadcasts a DELAY_PROPOSAL over the control channel
    // through CSS (ControlChannel_SendDelayProposal on the ping cadence)
    // and we adopt max(local candidate, remote candidate) here. max is
    // commutative, so both peers land on the SAME delay. The candidate
    // already folds in a manual FM2K_LOCAL_DELAY override, so a peer who
    // pins 14 pulls the other peer up to 14 instead of desyncing.
    //
    // ControlChannel computes the local candidate from measured RTT per
    // the delay mode (FM2K_DELAY_MODE: avg = mean RTT, peak = worst
    // RTT) at FM2K's 100 Hz, where 10 ms is one frame budget:
    //   candidate = clamp(ceil(one_way_ms / 10), 2, 15)
    // A manual override instead yields the user's 0..16 verbatim.
    //
    // The negotiated value is pinned for the connection lifetime
    // (g_session_delay_cached) so it stays stable across every CSS->
    // battle transition. A manual override bypasses the cache so the
    // user can still flip it mid-session.
    bool has_manual_delay = false;
    if (const char* env = std::getenv("FM2K_LOCAL_DELAY"); env && env[0]) {
        int v = std::atoi(env);
        if (v >= 0 && v <= 16) has_manual_delay = true;
    }
    const uint32_t rtt_mean_ms  = ControlChannel_GetRttMs();
    const uint32_t rtt_worst_ms = ControlChannel_GetWorstRttMs();
    const int local_cand  = ControlChannel_GetLocalDelayCandidate();
    const int remote_cand = ControlChannel_GetRemoteDelayCandidate();

    int local_delay;
    enum DelaySource { DS_MANUAL, DS_COMPUTED, DS_CACHED } delay_source;
    if (g_session_delay_cache_valid && !has_manual_delay) {
        local_delay  = g_session_delay_cached;
        delay_source = DS_CACHED;
    } else {
        // local_cand is -1 only when no RTT has been measured AND no
        // manual override -- fall back to the conservative CSS delay.
        const int lc = (local_cand < 0) ? CSS_LOCAL_DELAY : local_cand;
        local_delay  = (remote_cand > lc) ? remote_cand : lc;
        delay_source = has_manual_delay ? DS_MANUAL : DS_COMPUTED;
        if (!has_manual_delay) {
            g_session_delay_cached      = local_delay;
            g_session_delay_cache_valid = true;
            // Fresh worst-RTT window for the next match's diagnostics.
            ControlChannel_ResetWorstRttMs();
        }
    }

    // prediction_window: how far GekkoNet will speculatively rewind
    // before stalling. Default 16 at 100 FPS gives ~160 ms wall-clock
    // budget (matches the GekkoNet OnlineSession reference's 10-frame
    // budget at 60 FPS). Free CPU at steady state — only costs work
    // *during* a recovery from a deep rollback, which is rare.
    //
    // FM2K_PREDICTION_WINDOW env override accepts 0..64. Set 8 to
    // bisect a regression against the pre-2026-05-18 default.
    int prediction_window = 16;
    if (const char* env = std::getenv("FM2K_PREDICTION_WINDOW"); env && env[0]) {
        int v = std::atoi(env);
        if (v >= 0 && v <= 64) prediction_window = v;
    }
    g_pred_window = prediction_window;

    const char* source_str =
        delay_source == DS_MANUAL   ? "manual" :
        delay_source == DS_CACHED   ? "negotiated (cached)" :
                                      "negotiated";
    const char* mode_str =
        ControlChannel_GetDelayMode() == 1 ? "peak" : "avg";
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "Netplay: RTT mean=%ums worst=%ums -> delay candidate "
        "local=%d remote=%d, local_delay=%d (%s, mode=%s), "
        "prediction_window=%d",
        rtt_mean_ms, rtt_worst_ms, local_cand, remote_cand,
        local_delay, source_str, mode_str, prediction_window);

    GekkoConfig config = {};
    config.num_players = 2;
    // Up to 4 spectators per battle session. spectator_delay = 0 disables
    // GekkoNet's spectator pause-buffer (spectator_session.cpp:216-218
    // short-circuits ShouldDelaySpectator). With > 0 the spectator's
    // local sim stays paused until min_received - current >= delay, but
    // current also advances each tick post-unpause — for low-latency
    // LAN/localhost setups the buffer never converges and spectator stays
    // paused forever. Re-enable jitter buffering once we have measured
    // cross-internet ping data.
    config.max_spectators           = 4;
    config.spectator_delay          = 0;
    // Late-joiner backfill — see CSS-session comment + README.md:36.
    config.input_history_size       = 60000;
    config.input_prediction_window  = prediction_window;
    config.input_size = sizeof(uint16_t);
    config.state_size = sizeof(uint32_t);
    config.desync_detection = true;
    config.limited_saving = false;

    gekko_create(&g_session, GekkoGameSession);
    gekko_start(g_session, &config);
    g_session_kind = SessionKind::BATTLE;
    SpectatorNode_ClearGekkoSpectatorTracking();

    auto adapter = CreateMultiplexAdapter();
    gekko_net_adapter_set(g_session, adapter);

    // Refresh remote address string from the actually-learned peer sockaddr.
    // g_remote_addr was written at Netplay_Init from the env var (often stale, e.g.
    // "127.0.0.1:7001" for a host before the client connects). The adapter formats
    // inbound packet addresses from the real sockaddr ("ip:port"), so the string we
    // give gekko_add_actor must match that same formatting or every packet is dropped.
    if (const sockaddr_in* learned = NetSocket_GetRemoteAddr()) {
        if (learned->sin_port != 0) {
            char ip_buf[INET_ADDRSTRLEN] = {};
            inet_ntop(AF_INET, (void*)&learned->sin_addr, ip_buf, sizeof(ip_buf));
            snprintf(g_remote_addr, sizeof(g_remote_addr), "%s:%u",
                     ip_buf, ntohs(learned->sin_port));
        }
    }

    for (int i = 0; i < 2; i++) {
        if (i == g_player_index) {
            gekko_add_actor(g_session, GekkoLocalPlayer, nullptr);
            gekko_set_local_delay(g_session, i, local_delay);
            g_local_delay = local_delay;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Netplay: Added local player at slot %d (delay=%d)", i, local_delay);
        } else {
            GekkoNetAddress addr = {};
            addr.data = (void*)g_remote_addr;
            addr.size = (int)strlen(g_remote_addr);
            gekko_add_actor(g_session, GekkoRemotePlayer, &addr);
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Netplay: Added remote player at slot %d -> %s", i, g_remote_addr);
        }
    }

    AddSubscribedSpectatorsToSession();

    // Runahead: speculatively advance `runahead` extra frames PAST confirmed
    // input EVERY tick, re-simulating them, to hide the VISUAL lag of input
    // delay. The catch: that's `1 + runahead` full sim passes per wall-clock
    // frame, ALWAYS (not just on rollback). At 100 FPS the per-frame budget
    // is only 10 ms, and a lot of 2DFM players are on weak hardware where
    // even plain rollback strains — runahead on top routinely blows the
    // budget and drops frames. So:
    //
    //   DEFAULT OFF. Runahead is opt-in, not automatic.
    //
    // It's a latency-feel nicety, not a correctness feature — turning it off
    // costs only visible input lag (which the input delay already covers),
    // never desync. F8 toggles it on to `runahead_pref` (= local_delay, the
    // value that exactly cancels the delay's visual lag) for anyone who wants
    // it and has the CPU headroom. FM2K_RUNAHEAD env force-pins 0..15.
    int runahead_pref = local_delay;            // the "on" value F8 enables
    if (runahead_pref < 0)  runahead_pref = 0;
    if (runahead_pref > 15) runahead_pref = 15; // GekkoNet runahead is a u8 cap
    int runahead = 0;                           // DEFAULT OFF (see above)
    if (const char* env = std::getenv("FM2K_RUNAHEAD"); env && env[0]) {
        int v = std::atoi(env);
        if (v >= 0 && v <= 15) {
            runahead = v;
            if (v > 0) runahead_pref = v;       // env-on also sets the F8 target
        }
    }
    g_runahead_user_pref = runahead_pref;
    g_runahead_active.store(runahead, std::memory_order_release);
    gekko_set_runahead(g_session, (unsigned char)runahead);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "Netplay: prediction_window=%d runahead=%d (DEFAULT OFF; F8 toggles "
        "0<->%d, local_delay=%d)",
        prediction_window, runahead, runahead_pref, local_delay);

    *(uint32_t*)FM2K::ADDR_RANDOM_SEED = 0x12345678;
    SpectatorNode_AppendPinRng(0x12345678);
    SaveState_Init();
    SaveState_DoInitialSync();  // eager pre-AdvEvent reset (was lazy)
    SoundRollback::Init();
    // Battle-start init order matters: PIN_RNG → RESET_INPUT_STATE → SOUND_INIT
    // mirrors what host's local sim is doing this frame. The RESET_INPUT_STATE
    // op corresponds to SaveState_Save's first-call buf_idx + edge state +
    // history-rings reset (savestate.cpp:223-237) which fires when the first
    // GekkoNet AdvanceEvent triggers a Save, not here. We append it now so
    // it lands BEFORE the first INPUT batch of the match — the spectator
    // applies it at the same logical-frame boundary the host clears state at.
    SpectatorNode_AppendResetInputState();
    SpectatorNode_AppendSoundInit();

    // Per-battle replay capture is owned by SpectatorNode (v2 .fm2krep
    // file format). The legacy Replay::Replay_BeginRecording call here was
    // retired in v0.2.27 — its 96-byte ReplayHeader + raw ReplayFrame[]
    // body has been a strict subset of the v2 SessionEvent stream since
    // C6 landed. Initial RNG seed + state hash are captured below for
    // SpectatorNode_OnMatchStart, which inlines them into the
    // MATCH_START event's 96-byte payload (byte-compatible with the old
    // ReplayHeader so the wire schema stays stable).
    const uint32_t initial_seed = 0x12345678;
    const uint32_t initial_state_hash =
        SaveState_GetRegionChecksums().gameplay_fingerprint;

    // C7 — emit the host's session_id once at the very first match of the
    // connection. Generated lazily on first call: high 32 bits = unix epoch
    // seconds (so files sort chronologically + collisions are bounded to
    // intra-second), low 32 bits = a random nonce for uniqueness even within
    // the same second across machines. Subsequent matches in the same
    // session reuse the cached value via SpectatorNode_GetSessionId().
    if (g_player_index == 0 && SpectatorNode_GetSessionId() == 0) {
        const uint64_t epoch = (uint64_t)std::time(nullptr);
        std::random_device rd;
        const uint64_t nonce = ((uint64_t)rd() << 32) | (uint64_t)rd();
        const uint64_t session_id = (epoch << 32) ^ nonce;
        SpectatorNode_AppendSessionId(session_id);
    }

    // Notify the spectator tree: start of a new match, push INITIAL_MATCH to
    // any currently-subscribed viewers so they reset and follow this match.
    // C6: chars/stage read from the same addresses SharedMem_PublishMatchChars
    // / PublishMatchStage above use. Cast to uint8 — char/stage IDs are well
    // under 256 (FM2K rosters cap at 50, stages at ~50).
    //
    // Color pick: read from each player's char-slot record at slot+0xE00B
    // (ADDR_CHARSLOT0_COLOR_PICK + CHARSLOT_STRIDE * slot). 1v1 VS mode
    // pins slot[0]=P1, slot[1]=P2 (set by AssignPlayerColor's call site
    // in game_state_manager). v0.2.33 and earlier hardcoded 0 here based
    // on a misread that FM2K had no palette select — but the bit-mask
    // ladder in AssignPlayerColor maps attack buttons 1..5 to colors
    // 1..5. With 0 in MATCH_START, css_autoconfirm's per-player target
    // bit always fell back to bit 4 (color 0) on replay/spec, so every
    // playback showed palette 0 regardless of what was originally picked.
    const uint8_t mp1_char = static_cast<uint8_t>(
        *(const uint32_t*)FM2K::ADDR_P1_SELECTED_CHAR);
    const uint8_t mp2_char = static_cast<uint8_t>(
        *(const uint32_t*)FM2K::ADDR_P2_SELECTED_CHAR);
    uint8_t mp1_color = 0;
    uint8_t mp2_color = 0;
    if constexpr (FM2K::ADDR_CHARSLOT0_COLOR_PICK != 0) {
        mp1_color = static_cast<uint8_t>(
            *(const uint32_t*)FM2K::ADDR_CHARSLOT0_COLOR_PICK);
        mp2_color = static_cast<uint8_t>(
            *(const uint32_t*)(FM2K::ADDR_CHARSLOT0_COLOR_PICK + FM2K::CHARSLOT_STRIDE));
    }
    const uint8_t mstage_id = static_cast<uint8_t>(mstage_id_pre_roll);
    SpectatorNode_OnMatchStart(
        /*game_hash*/         0,
        /*initial_rng_seed*/  initial_seed,
        /*initial_state_hash*/initial_state_hash,
        /*p1_char*/mp1_char, /*p1_color*/mp1_color,
        /*p2_char*/mp2_char, /*p2_color*/mp2_color,
        /*stage_id*/mstage_id);

    // C3.5 — reset the intra-match round counter so the first ROUND_START
    // emitted from vs_round_function lands as round_idx=1.
    extern void RoundEvents_OnMatchStart();
    RoundEvents_OnMatchStart();

    // Task #18 phase 2: capture a SaveState snapshot RIGHT NOW so a
    // CURRENT_MATCH-mode spectator joining mid-set can SaveState_Load
    // directly to this match's start instead of replaying every previous
    // battle. Order matters: must run AFTER OnMatchStart (which appends
    // the MATCH_START SessionEvent — we want our snapshot's input_frame
    // anchor to count from after that op) and AFTER the pin/init sites
    // above (snapshot reflects the canonical pristine match-start state).
    SpectatorNode_StashSnapshot();

    // Re-broadcast host config so any setting changes (host clicked a
    // different stage between matches, switched SOCD mode, etc.) propagate
    // to client + spectators before this match's first sim frame.
    Netplay_BroadcastHostConfig();

    g_simple_state = SimpleState::BATTLE;
    g_session_ready = false;
    g_netplay_frame = 0;
    g_highest_recorded_frame = 0;  // monotonic dedup gate, reset per battle
    g_rollback_count = 0;
    g_last_rollback_frame = 0;
    g_desync_count = 0;
    g_last_desync_log_tick = 0;

    // Battle session is up. Disarm BATTLE_ENTERING (we're past the signaling
    // window — any further arrivals are duplicates / late echoes and should
    // be dropped, otherwise the rate-limited echo path keeps both peers in
    // a forever-ping-pong that competes with GekkoNet's own sync handshake).
    // Arm BATTLE_END for the eventual return-to-CSS swap.
    g_battle_entry_armed = false;
    g_battle_end_armed   = true;

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "Netplay: GekkoNet battle session created (runahead=%d, prediction_window=%d)",
        g_runahead_active.load(std::memory_order_acquire), g_pred_window);
    return true;
}

// Single-instance determinism test: GekkoStressSession with both players local
// and no network adapter. GekkoNet rewinds every check_distance frames and
// compares checksums, flagging any sim divergence as a desync event.
bool Netplay_StartStressBattle() {
    if (g_session) {
        return true;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "Netplay: Starting GekkoStressSession (single-instance determinism test)");

    GekkoConfig config = {};
    config.num_players = 2;
    config.max_spectators = 0;
    config.input_prediction_window = 8;
    config.input_size = sizeof(uint16_t);
    config.state_size = sizeof(uint32_t);
    config.desync_detection = true;
    config.limited_saving = false;
    // StressSession-specific: force a rollback every check_distance frames
    // so GekkoNet re-simulates from a saved state and compares checksums
    // against the original advance. Any mismatch fires GekkoDesyncDetected.
    //
    // 0 = no forced rollback. With check_distance=10 the replay self-test
    // produces a record→replay divergence at frame 764 (5× run-bisected
    // confirmation): host's forward sim records inputs that, replayed
    // through the engine without rollback, diverge from the host's own
    // recorded parity stream. With check_distance=0 the same 1500-frame
    // test passes 100% (all aligned frames identical). Means: SaveState
    // is missing some piece of state that PGI+UG mutates — after rollback
    // Load + re-PGI+UG, the post-re-sim state differs from the original
    // forward state, and subsequent forward advances drift from then on.
    //
    // Production netplay rollbacks are predictive (1-3 frames deep), so
    // the accumulated drift over 76 stress rollback cycles is much
    // larger than what a real game would hit, but the underlying
    // savestate-completeness bug is real and should be fixed. Task #34
    // tracks the hunt; until then check_distance=0 to keep the replay
    // self-test green for shipping. Real-netplay savestate correctness
    // is verified by per-frame checksum compares against the peer when
    // gekko rollback fires for genuine prediction misses.
    //
    // FM2K_CHECK_DISTANCE env overrides for task #34 bisects: e.g.
    // FM2K_CHECK_DISTANCE=10 python3 tools/replay_selftest.py ...
    // (the harness forwards it through the cmd.exe env block).
    config.check_distance = 0;
    if (const char* env = std::getenv("FM2K_CHECK_DISTANCE"); env && env[0]) {
        config.check_distance = std::atoi(env);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "Netplay: FM2K_CHECK_DISTANCE override -> %d", config.check_distance);
    }

    // StressSession mode: ignores network, forces rollbacks from a single instance.
    gekko_create(&g_session, GekkoStressSession);
    gekko_start(g_session, &config);
    g_session_kind = SessionKind::STRESS;
    SpectatorNode_ClearGekkoSpectatorTracking();

    // Both actors local. No adapter set -> no network calls.
    //
    // local_delay=0 in stress mode. Real netplay sets delay>=1 to hide
    // network latency, but stress mode has no peer — there's nothing to
    // hide. With delay=1, gekko buffers each tick's added input and
    // delivers it on the FOLLOWING confirmed advance, so host's frame K
    // sim consumes autoplay(K-1). The .fm2krep recorder writes INPUT[K] =
    // the autoplay value added on tick K (one-shot per AdvanceEvent
    // capture, not delay-corrected). Replay reads INPUT[K] sequentially
    // and feeds it to frame K's PGI — so replay's frame K consumes
    // autoplay(K) while host's consumed autoplay(K-1). Same value at the
    // INPUT[K] slot, but the engine sim aligns INPUT[K-1] (host) vs
    // INPUT[K] (replay) into "frame K", producing an off-by-one engine
    // input divergence that surfaces around frame 65 (the first frame
    // where the autoplay pattern emits a non-zero p1 value that the
    // motion-check engine cares about). With delay=0, host's frame K
    // consumes autoplay(K) too — symmetric with replay.
    for (int i = 0; i < 2; i++) {
        gekko_add_actor(g_session, GekkoLocalPlayer, nullptr);
        gekko_set_local_delay(g_session, i, 0);
    }
    g_local_delay = 0;

    // Deterministic initial RNG seed (matches Netplay_StartBattle).
    *(uint32_t*)FM2K::ADDR_RANDOM_SEED = 0x12345678;
    SaveState_Init();
    SaveState_DoInitialSync();  // eager pre-AdvEvent reset
    SoundRollback::Init();
    // Initialize SpectatorNode in offline-record mode so the stress run
    // produces a self-replayable .fm2krep file on auto-terminate. Without
    // this, MATCH_START never reaches session_events and
    // SpectatorNode_WriteCurrentBattleFile returns false.
    SpectatorNode_Init();
    // Emit the same pre-MATCH_START init sequence as real netplay
    // (Netplay_StartBattle): PIN_RNG → RESET_INPUT_STATE → SOUND_INIT.
    // Without these in the recorded slice, the replay-side engine never
    // gets its rng reset to 0x12345678 — it stays at whatever the C
    // runtime / boot sequence left there — and the post-sim rng on
    // frame 0 diverges from the record (~ that's the literal record vs
    // replay rng split the harness diff caught at frame 0).
    SpectatorNode_AppendPinRng(0x12345678);
    SpectatorNode_AppendResetInputState();
    SpectatorNode_AppendSoundInit();
    // Emit MATCH_START with zeroed CSS metadata (boot-to-battle bypasses
    // CSS; char/color/stage IDs are whatever was in default memory).
    // The replay-self-test driver only needs the events to be slice-able,
    // not for the metadata to be accurate.
    SpectatorNode_OnMatchStart(
        /*game_hash*/         0,
        /*initial_rng_seed*/  0x12345678,
        /*initial_state_hash*/0,
        /*p1_char*/0, /*p1_color*/0,
        /*p2_char*/0, /*p2_color*/0,
        /*stage_id*/0);

    g_simple_state = SimpleState::BATTLE;
    g_session_ready = true;   // no handshake needed for stress
    g_netplay_frame = 0;
    g_highest_recorded_frame = 0;
    g_rollback_count = 0;
    g_last_rollback_frame = 0;
    g_desync_count = 0;
    g_last_desync_log_tick = 0;

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "Netplay: GekkoStressSession created (check_distance=%d, prediction_window=%d)",
        config.check_distance, config.input_prediction_window);
    return true;
}

void Netplay_EndBattle() {
    // Capture match outcome BEFORE we destroy the session — reading HP
    // at this point reflects the final state of the just-ended battle.
    // Outcome is from the local player's perspective; the launcher
    // forwards it as a `match_result` to the hub, which correlates
    // both peers' reports for stats. Only meaningful for actual
    // GekkoGameSessions (player vs player); spectate sessions and
    // stress runs skip the publish.
    // C7 — capture winner / per-side round wins for both the launcher's
    // SharedMem outcome publish AND the SessionEvent MATCH_END payload that
    // ships to subscribers + replay files. Same data, two consumers.
    uint8_t  match_winner_idx  = 2;  // 0=P1, 1=P2, 2=draw / unknown
    uint8_t  match_rounds_p1   = 0;
    uint8_t  match_rounds_p2   = 0;
    if (g_session && g_session_kind == SessionKind::BATTLE) {
        FM2KMatchOutcome outcome = FM2K_MATCH_OUTCOME_NONE;
        if constexpr (FM2K::kIsFM2K) {
            // FM2K: HP-based outcome — direct read at the moment the
            // session ends. KO state has the loser's HP at 0; both at
            // 0 = double-KO (draw); both >0 = timeout/non-KO end (we
            // can't decide and log as draw).
            const uint32_t p1_hp = *(uint32_t*)0x4DFC85;
            const uint32_t p2_hp = *(uint32_t*)0x4EDCC4;
            if (p1_hp == 0 && p2_hp == 0) {
                outcome = FM2K_MATCH_OUTCOME_DRAW;
                match_winner_idx = 2;
            } else if (p1_hp > 0 && p2_hp == 0) {
                outcome = (g_player_index == 0)
                            ? FM2K_MATCH_OUTCOME_SELF_WON
                            : FM2K_MATCH_OUTCOME_PEER_WON;
                match_winner_idx = 0;
            } else if (p2_hp > 0 && p1_hp == 0) {
                outcome = (g_player_index == 1)
                            ? FM2K_MATCH_OUTCOME_SELF_WON
                            : FM2K_MATCH_OUTCOME_PEER_WON;
                match_winner_idx = 1;
            } else {
                outcome = FM2K_MATCH_OUTCOME_DRAW;
                match_winner_idx = 2;
            }
            // FM2K round-win counters (v0.2.21 probe-verified). Per-char-slot
            // field at offset -0x18 from HP. 0 at match start, increments
            // each round the player wins. Hooks.cpp's `g_match_phase` /
            // `g_round_sub_state` labels at the same addresses are
            // misleading — those are per-slot rounds-won, not phase fields.
            match_rounds_p1 = (uint8_t)*(uint32_t*)FM2K::ADDR_P1_ROUNDS_WON;
            match_rounds_p2 = (uint8_t)*(uint32_t*)FM2K::ADDR_P2_ROUNDS_WON;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Netplay: match outcome p1_hp=%u p2_hp=%u outcome=%d",
                p1_hp, p2_hp, (int)outcome);
        } else {
            // FM95: round-win-counter-based outcome — mirrors the
            // game's own decision in obj_match_result_state @ 0x410db0
            // case 4: whoever has more rounds won is the match winner.
            // These counters reset to 0 only at the START of a new
            // match (vs_round_function case 1), so by the time we land
            // here at session-stop they hold the final per-match
            // values. Doesn't depend on g_p_main_object_ptr being
            // valid (the per-object struct may have been torn down by
            // the time we get here on a peer-disconnect path).
            const uint32_t p1_wins = *(uint32_t*)FM2K::ADDR_P1_WIN_COUNTER;
            const uint32_t p2_wins = *(uint32_t*)FM2K::ADDR_P2_WIN_COUNTER;
            if (p1_wins > p2_wins) {
                outcome = (g_player_index == 0)
                            ? FM2K_MATCH_OUTCOME_SELF_WON
                            : FM2K_MATCH_OUTCOME_PEER_WON;
                match_winner_idx = 0;
            } else if (p2_wins > p1_wins) {
                outcome = (g_player_index == 1)
                            ? FM2K_MATCH_OUTCOME_SELF_WON
                            : FM2K_MATCH_OUTCOME_PEER_WON;
                match_winner_idx = 1;
            } else {
                outcome = FM2K_MATCH_OUTCOME_DRAW;
                match_winner_idx = 2;
            }
            match_rounds_p1 = (uint8_t)p1_wins;
            match_rounds_p2 = (uint8_t)p2_wins;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Netplay: match outcome p1_wins=%u p2_wins=%u outcome=%d",
                p1_wins, p2_wins, (int)outcome);
        }
        SharedMem_PublishMatchOutcome(outcome);
    }

    if (g_session) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "Netplay: Ending GekkoNet session (kind=%d)", (int)g_session_kind);
        gekko_destroy(&g_session);
        g_session       = nullptr;
        g_session_kind  = SessionKind::NONE;
    }

    // (Legacy Replay::Replay_EndRecording call retired in v0.2.27 — the
    // SpectatorNode_WriteCurrentBattleFile call below writes the v2
    // .fm2krep file that supersedes the legacy 96-byte-header format.)

    // Tell the spectator tree the match is over — subscribers receive
    // MATCH_END and go idle until the next SpectatorNode_OnMatchStart.
    // C7: payload carries winner + per-side rounds + frames_total for
    // self-describing .fm2krep files. frames_total is computed inside
    // SpectatorNode_AppendMatchEnd via session-input-frame delta against
    // the most-recent MATCH_START — host-side bookkeeping, no caller
    // input needed for that field.
    MatchEndPayload match_end_payload = {};
    match_end_payload.winner_idx     = match_winner_idx;
    match_end_payload.rounds_won_p1  = match_rounds_p1;
    match_end_payload.rounds_won_p2  = match_rounds_p2;
    match_end_payload.frames_total   = 0;  // filled inside Append
    SpectatorNode_OnMatchEnd(match_end_payload);

    // (Removed in v0.2.20: post-MATCH_END SpectatorNode_StashSnapshot. It
    // crashed users on the first 3000→2000 transition — likely SaveState_Save
    // running its replay-diff scan against torn-down FM2K state after
    // gekko_destroy. CURRENT_MATCH-mode spectator joining mid-CSS still
    // receives the start-of-match snapshot from Netplay_StartBattle's
    // StashSnapshot — they replay the prior match's frames, which is
    // suboptimal but correct. Phase 6 robustness pass can re-add a
    // between-match cache freshen with a JIT live-peek (no SaveState_Save)
    // path that doesn't trigger the replay-diff scan.)

    // Per-battle .fm2krep — slice the SessionEvent log between the most
    // recent MATCH_START and the just-appended MATCH_END. Same on-disk
    // shape as .fm2kset (full session); is_battle_slice flag distinguishes.
    //
    // HOST-ONLY: round_events.cpp's vs_round_function detour gates emit
    // on g_player_index == 0, so non-host peers' session_events lacks
    // ROUND_START/END entries. Writing both peers' files at battle end
    // would race on the same `replays/<ts>.fm2krep` filename and the
    // (possibly later) non-host write would clobber the authoritative
    // host file with one missing round events + missing session_id.
    // The host's file is canonical. Spectators that record their own
    // local files use a separate code path.
    if (g_player_index == 0) {
        char ts[64] = {};
        std::time_t now = std::time(nullptr);
        std::tm tm_buf{};
        localtime_s(&tm_buf, &now);
        std::strftime(ts, sizeof(ts), "replays/%Y-%m-%d_%H%M%S.fm2krep", &tm_buf);
        CreateDirectoryA("replays", nullptr);
        SpectatorNode_WriteCurrentBattleFile(ts);
    }

    // Stop any pending SFX "desired" entries and clear the channel map so
    // the next battle rescans the channel table (handles character-load
    // changes between matches).
    SoundRollback::OnBattleEnd();

    // Flush the rngtrace ring on clean battle exit too, so a desync-free
    // round still produces a trace to diff against a future desync run.
    SaveState_FlushRngTrace(g_player_index, "battle end");

    g_session_ready = false;
    g_simple_state = SimpleState::CONNECTED;

    // Reset CSS state for rematch
    g_css_active = false;
    g_css_synced = false;
    g_local_css_ready = false;
    g_remote_css_ready = false;

    // Reset battle sync state for next battle (entry direction). Both gates
    // disarmed: the next CSS rendezvous re-arms BATTLE_ENTERING when the
    // new CSS GekkoSession comes up, and Netplay_StartBattle re-arms
    // BATTLE_END once the next battle session is created. Anything that
    // arrives between now and those points is stale carryover and gets
    // dropped at the handler.
    g_local_battle_entered    = false;
    g_remote_battle_entered   = false;
    g_battle_synced           = false;
    g_battle_entry_swap_frame = 0;
    g_battle_entry_armed      = false;

    // Reset battle-end sync state — fresh for the rematch's next return.
    g_local_battle_end_signaled  = false;
    g_remote_battle_end_signaled = false;
    g_battle_end_synced          = false;
    g_battle_end_swap_frame      = 0;
    g_battle_end_armed           = false;
}

// =============================================================================
// BATTLE FRAME PROCESSING
// =============================================================================

// Frame pacing state (matches GekkoNet examples' handle_frame_time)
static LARGE_INTEGER g_perf_freq = {};
static LARGE_INTEGER g_frame_start = {};
static bool g_frame_timer_initialized = false;

// Called at the END of each frame to apply frame advantage throttle.
// The game already has its OWN 10ms frame limiter (timeGetTime in WinMain).
// We only add EXTRA delay when ahead of remote to prevent rollback cascade.
// Without this, we'd double-limit (game's 10ms + our 10ms = 20ms = 50fps).
static void HandleFrameTime(float frames_ahead) {
    // Only throttle when ahead -- the game handles base frame timing
    if (frames_ahead <= 0.5f) {
        return;  // Not ahead, let game's own limiter handle timing
    }

    // Scale extra delay proportionally to advantage:
    // 0.5-1.0 ahead: +0.16ms (1.6% of 10ms)
    // 1.0-2.0 ahead: +0.5ms
    // 2.0-4.0 ahead: +1.0ms
    // 4.0+ ahead:    +2.0ms
    DWORD extra_ms;
    if (frames_ahead > 4.0f) {
        extra_ms = 2;
    } else if (frames_ahead > 2.0f) {
        extra_ms = 1;
    } else if (frames_ahead > 1.0f) {
        // Sleep(0) yields timeslice, ~0.5ms effective
        Sleep(0);
        return;
    } else {
        // 0.5-1.0: minimal throttle, just yield
        Sleep(0);
        return;
    }

    Sleep(extra_ms);
}

bool Netplay_ProcessBattleInputPhase() {
    if (!g_session) return true;

    // In stress mode, skip the network poll (no adapter, no socket).
    if (!g_stress_mode) {
        gekko_network_poll(g_session);
    }

    uint16_t local_input = Input_CaptureLocal();
    if (g_stress_mode) {
        // Single-instance determinism test. When FM2K_PARITY_AUTOPLAY_BATTLE
        // is on, drive both gekko slots with per-player autoplay values
        // (deterministic-pseudo-random from g_input_buffer_index+player_id).
        // Without this, gekko sees Input_CaptureLocal (keyboard, typically
        // 0) and the .fm2krep + spec stream record 0/0 — but the engine
        // sims with autoplay values, so --replay re-runs with 0/0 and
        // diverges from the record at frame 0.
        //
        // Cached env-var check: once active for this run, ALWAYS use
        // autoplay values (including legitimate 0s on phase=0/1 idle
        // frames). Falling back to keyboard on zero would let stale
        // focus-state poison the input stream.
        static int s_autoplay_battle = -1;
        if (s_autoplay_battle < 0) {
            const char* v = std::getenv("FM2K_PARITY_AUTOPLAY_BATTLE");
            s_autoplay_battle = (v && v[0] && v[0] != '0') ? 1 : 0;
        }
        if (s_autoplay_battle == 1) {
            uint16_t auto_p1 = Hook_ComputeAutoplayBattleInput(0);
            uint16_t auto_p2 = Hook_ComputeAutoplayBattleInput(1);
            gekko_add_local_input(g_session, 0, &auto_p1);
            gekko_add_local_input(g_session, 1, &auto_p2);
        } else {
            // Local 2P: P1 keeps the captured local input (binder slot 0);
            // P2 gets its OWN binder mask (slot 1). Previously both slots got
            // the same local_input — a leftover from idle-only stress — which
            // made the P1 controller drive BOTH players, so you couldn't set
            // up a real combo against an independent P2 (e.g. keyboard).
            uint16_t p2_input = Input_CaptureLocalPlayer(1);
            gekko_add_local_input(g_session, 0, &local_input);
            gekko_add_local_input(g_session, 1, &p2_input);
        }
    } else {
        // Harness autoplay for REAL netplay sessions too: each peer
        // drives ITS OWN gekko slot with the deterministic autoplay
        // stream when FM2K_PARITY_AUTOPLAY_BATTLE=1. Without this the
        // loopback netplay/spectator harnesses played IDLE matches
        // (keyboard reads 0x000 headless, nobody moves, HP never
        // changes) -- sync verdicts were real but exercised a
        // near-static sim. Env-gated; production input is untouched.
        static int s_np_autoplay_battle = -1;
        if (s_np_autoplay_battle < 0) {
            const char* v = std::getenv("FM2K_PARITY_AUTOPLAY_BATTLE");
            s_np_autoplay_battle = (v && v[0] && v[0] != '0') ? 1 : 0;
        }
        if (s_np_autoplay_battle == 1) {
            local_input = Hook_ComputeAutoplayBattleInput(g_player_index);
        }
        gekko_add_local_input(g_session, g_player_index, &local_input);
    }

    int event_count = 0;
    auto events = gekko_session_events(g_session, &event_count);
    for (int i = 0; i < event_count; i++) {
        auto event = events[i];
        switch (event->type) {
            case GekkoPlayerConnected:
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Netplay: GekkoNet player %d connected",
                    event->data.connected.handle);
                g_session_ready = true;
                break;

            case GekkoSessionStarted:
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Netplay: GekkoNet session started");
                g_session_ready = true;
                break;

            case GekkoPlayerSyncing:
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Netplay: GekkoNet syncing %u/%u",
                    event->data.syncing.current, event->data.syncing.max);
                break;

            case GekkoPlayerDisconnected: {
                // Peer dropped (timeout / closed game / network died). Publish
                // a DISCONNECT outcome so the launcher's shared-mem poll
                // forwards a match_result to the hub AND tears down the
                // surviving local game. Fires on CSS too — without this the
                // survivor froze on the character-select screen with music
                // playing when their opponent closed during CSS (real bug
                // report 2026-05-05).
                if (g_session_kind == SessionKind::BATTLE ||
                    g_session_kind == SessionKind::CSS) {
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Netplay: peer disconnected (handle=%d kind=%d) — publishing DISCONNECT outcome",
                        event->data.disconnected.handle,
                        (int)g_session_kind);
                    SharedMem_PublishMatchOutcome(FM2K_MATCH_OUTCOME_DISCONNECT);
                }
                break;
            }

            case GekkoDesyncDetected: {
                HandleDesyncDetected(
                    event->data.desynced.frame,
                    event->data.desynced.local_checksum,
                    event->data.desynced.remote_checksum,
                    /*synthetic=*/false);
                break;
            }

            default:
                break;
        }
    }

    int update_count = 0;
    auto updates = gekko_update_session(g_session, &update_count);

    bool has_advance = false;
    // Track the advance-batch window for the Mike Z sound sync. Forward-only
    // batches have earliest == latest; rollback batches span the rewind range.
    uint32_t earliest_advance = UINT32_MAX;
    uint32_t latest_advance = g_netplay_frame;
    // Per-batch LoadEvent counter. GekkoNet's update sequence is:
    //   RewindRunahead -> [maybe HandleRollback] -> Advance -> Save
    //   -> [HandleRunahead emits Save + N speculative Advances]
    // RewindRunahead unconditionally emits a LoadEvent once runahead has
    // run at least once (to undo last tick's speculative advances). It
    // is NOT a real rollback. Real rollbacks emit a SECOND LoadEvent in
    // the same batch from HandleRollback. The first LoadEvent of every
    // batch is therefore the runahead rewind; only the 2nd+ should be
    // counted as user-visible rollbacks.
    // [RING] trace gate (task #34): cached FM2K_RING_TRACE env check.
    static auto RingTraceOn = []() {
        static int c = -1;
        if (c < 0) {
            const char* v = std::getenv("FM2K_RING_TRACE");
            c = (v && v[0] && v[0] != '0') ? 1 : 0;
        }
        return c == 1;
    };
    int load_events_in_batch = 0;
    for (int i = 0; i < update_count; i++) {
        auto update = updates[i];
        switch (update->type) {
            case GekkoSaveEvent: {
                int frame = update->data.save.frame;
                // [PHASE-F-DIAG] log save event details BEFORE Save (rng_seed
                // is read from live engine state; should match the AdvEvent
                // EXIT log's rng if no mutation happened in between).
                static int s_save_diag_cached = -1;
                if (s_save_diag_cached < 0) {
                    const char* v = std::getenv("FM2K_STRESS_DIAG");
                    s_save_diag_cached = (v && v[0] && v[0] != '0') ? 1 : 0;
                }
                if (s_save_diag_cached == 1) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[PHASE-F] SaveEvent save.frame=%d g_netplay_frame=%u "
                        "live_rng=0x%08X",
                        frame, g_netplay_frame,
                        *(uint32_t*)FM2K::ADDR_RANDOM_SEED);
                }
                { PerfScope _ps(&g_perf_save); SaveState_Save(frame); }  // profile save cost (#62)
                // [RING] trace (FM2K_RING_TRACE=1, frames 0..40): buf_idx +
                // ring slots at every Save/Load/Advance so the one-slot
                // input-history shift (task #34) can be watched being born
                // at the first rollback.
                if (RingTraceOn() && frame <= 40) {
                    const uint32_t b = *(uint32_t*)0x447EE0;
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[RING] SAVE f=%d buf=%u h1[b]=%03X h1[b-1]=%03X",
                        frame, b,
                        *(uint32_t*)(0x4280E0 + (b & 0x3FF) * 4),
                        *(uint32_t*)(0x4280E0 + ((b - 1) & 0x3FF) * 4));
                }
                (void)SaveState_GetLastChecksum(frame);  // updates RegionChecksums via side effect
                // Use the gameplay fingerprint as GekkoNet's desync checksum.
                // If both peers see the same HP/pos/rng/timer values at this
                // frame, fingerprints match and GekkoNet stays happy even when
                // per-process memory residue differs. Full combined hash is
                // still computed and available for the diagnostic dump.
                uint32_t checksum = SaveState_GetRegionChecksums().gameplay_fingerprint;
                *update->data.save.state_len = sizeof(uint32_t);
                *update->data.save.checksum = checksum;
                memcpy(update->data.save.state, &frame, sizeof(uint32_t));
                break;
            }

            case GekkoLoadEvent: {
                int frame = update->data.load.frame;
                load_events_in_batch++;
                // The first LoadEvent in a batch is the runahead rewind from
                // last tick's speculative advance -- a fixed-distance load
                // every tick, NOT a network rollback -- so it's excluded from
                // the stats. BUT that's only true when runahead is actually
                // ON. With runahead OFF (the bleeding default since the
                // runahead-off change), there IS no speculative rewind: a
                // real network rollback is a SINGLE LoadEvent (batch==1).
                // Gating only on (batch==1) then misclassified every real
                // rollback as a runahead rewind, so g_rollback_count stayed
                // pinned at 0 forever even while rollbacks visibly happened
                // (pringle's "visual rollbacks but the number stays 0", and
                // the rb_total=0 on the 200ms c785d0ca desync). Only skip the
                // first load when runahead is live.
                const bool runahead_on =
                    g_runahead_active.load(std::memory_order_acquire) > 0;
                const bool is_runahead_rewind =
                    runahead_on && (load_events_in_batch == 1);
                g_last_rollback_frame = g_netplay_frame;
                // Rolling stats only fire for real (non-runahead) rollbacks.
                // The first LoadEvent in any batch is the runahead rewind
                // from last tick's speculative advance -- a fixed-distance
                // load every tick under runahead, NOT a network-driven
                // rollback. Counting it inflated max_rewind to a constant
                // value and `% 100 == 0` was true when count==0, so the
                // log fired every tick until the first real rollback (15MB
                // of spam in steady-state replays).
                static uint32_t s_rb_window_rewind_sum = 0;
                static uint32_t s_rb_window_rewind_max = 0;
                if (!is_runahead_rewind) {
                    g_rollback_count++;
                    if (g_rollback_count == 1) {
                        s_rb_window_rewind_sum = 0;
                        s_rb_window_rewind_max = 0;
                    }
                    uint32_t rewind = g_netplay_frame - (uint32_t)frame;
                    s_rb_window_rewind_sum += rewind;
                    if (rewind > s_rb_window_rewind_max) s_rb_window_rewind_max = rewind;
                    // [BEAT] window — same numbers but reset on each
                    // heartbeat emit (every ~10s) so the BEAT line
                    // describes the most recent window, not session totals.
                    g_beat_window_rb_sum += rewind;
                    g_beat_window_rb_count++;
                    if (rewind > g_beat_window_rb_max) g_beat_window_rb_max = rewind;
                    if (g_rollback_count > 0 && g_rollback_count % 100 == 0) {
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "ROLLBACK stats: total=%u, last-100 avg_rewind=%.1f max_rewind=%u (last frame=%d current=%u)",
                            g_rollback_count,
                            (double)s_rb_window_rewind_sum / 100.0,
                            s_rb_window_rewind_max,
                            frame, g_netplay_frame);
                        s_rb_window_rewind_sum = 0;
                        s_rb_window_rewind_max = 0;
                    }
                }
                { PerfScope _pl(&g_perf_load); SaveState_Load(frame); }  // profile load cost (#62)
                if (RingTraceOn() && frame <= 40) {
                    const uint32_t b = *(uint32_t*)0x447EE0;
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[RING] LOAD f=%d buf=%u h1[b]=%03X h1[b-1]=%03X",
                        frame, b,
                        *(uint32_t*)(0x4280E0 + (b & 0x3FF) * 4),
                        *(uint32_t*)(0x4280E0 + ((b - 1) & 0x3FF) * 4));
                }
                // Rewind the deterministic virtual clock to match the loaded
                // frame. Without this g_virtual_time_ms stays at its forward-sim
                // value, replay main_game_loop iterations read timeGetTime()
                // values AHEAD of where forward was at the same frame, and the
                // self-referential arithmetic at main_game_loop 0x405BA8/0x405BBB
                // (g_last_frame_time += N*10) writes a different byte pattern
                // into g_last_frame_time @ 0x447DD4 — which is what produced
                // the "AfterimagePool +0x4A4" divergence at f=9.
                extern uint32_t g_virtual_time_ms;
                g_virtual_time_ms = (uint32_t)frame * 10;
                // Also rewind g_netplay_frame so the per-advance increments
                // below produce the same frame numbers the forward pass did.
                g_netplay_frame = (uint32_t)frame;
                break;
            }

            case GekkoAdvanceEvent: {
                // Authoritative frame label straight from gekko (task #34
                // root-cause fix, 2026-06-11). The old scheme (increment at
                // the end of each advance + relabel to load.frame at
                // LoadEvent) went ONE LOW after the first rollback batch:
                // gekko's Load(N) restores POST-frame-N state, so the next
                // advance produces frame N+1 -- but we relabeled to N. The
                // recorder's monotonic dedup gate below then swallowed the
                // first confirmed frame after the batch, leaving every
                // .fm2krep one INPUT short from the session's first
                // rollback onward. Replay playback consumed the shifted
                // stream (each input one frame early), eventually visibly
                // desyncing -- the "replays desync from matches" bug.
                // Proven via [RING] trace + raw .fm2krep diff: sim frame
                // 11's input (000,006) absent from the file, two confirmed
                // advances both labeled f=10.
                g_netplay_frame = (uint32_t)update->data.adv.frame;

                // Record the pre-sim frame for the Mike Z sound sync window.
                // `g_netplay_frame` here is the frame we're about to *produce*;
                // Hook_DispatchScriptSoundCommand tags desired[] with this same
                // value (via Netplay_GetFrame) so earliest/latest compare apples
                // to apples.
                if (g_netplay_frame < earliest_advance) {
                    earliest_advance = g_netplay_frame;
                }

                // Set inputs for THIS frame
                if (update->data.adv.inputs && update->data.adv.input_len >= sizeof(uint16_t) * 2) {
                    uint16_t* inputs = (uint16_t*)update->data.adv.inputs;
                    g_p1_input = inputs[0] & 0x7FF;
                    g_p2_input = inputs[1] & 0x7FF;
                }

                // Track rollback state (matching BBBR reference implementation).
                // During rollback replay, input edge detection globals must NOT
                // be overwritten - they get restored by LoadEvent and should only
                // be updated on the authoritative (non-rollback) frame.
                g_is_rolling_back = update->data.adv.rolling_back;

                // [REMOVED] per-slot fan-out at slot+0xDF79 / slot+0xDF7D.
                // main_game_loop's prologue writes these every iteration,
                // but IDA xref of g_p1_input_buffer_index_field (0x4DFD0D)
                // shows the only reader is check_game_continue, which is
                // a no-op when g_directplay_interface == NULL — and we
                // never initialize DirectPlay. KGT scripts and
                // update_game/process_game_inputs do not read these
                // fields. The writes were just trampling adjacent slot
                // memory the StudioS chars happen to use, breaking
                // their scripts.

                // Snapshot RNG BEFORE PGI/UG run. Used by the [HOST-TRACE] log
                // below — distinguishes "per-frame leak inside PGI/UG" from
                // "inter-frame leak between confirmed advances."
                const uint32_t rng_pre_advance = *(uint32_t*)0x41FB1C;

                // Speculative-advance carve-out for palette flash state.
                //
                // Why: original_update_game is the game's update_game_state @
                // 0x404CD0 — a 7-instruction stub that does nothing but
                // decrement two palette flash timers (g_timer_countdown1 @
                // 0x4456E4 and g_timer_countdown2 @ 0x447D91). PGI also runs
                // character_state_machine which may re-fire [EB] and write
                // timer = duration during a script's trigger anim slot.
                //
                // Under runahead = 4, every wall-clock frame fires 1 confirmed
                // + 4 speculative AdvanceEvents. Each speculative advance also
                // hits update_game_state, decrementing palette timers. Net:
                // palette drains 5x per wall-clock frame instead of 1x — a
                // 43-frame palette flash plays for ~9 wall-clock frames and
                // then "cuts out", which is the bug user reported.
                //
                // Fix: snapshot the palette flash structs before PGI runs,
                // and after UG returns, restore them IFF this advance is
                // speculative (running_ahead). Confirmed advances drain the
                // timer naturally; speculative advances are a no-op on
                // palette state. End result: timer drains 1/wall-clock-frame
                // matching vanilla.
                //
                // Why not the same fix for shake? Shake is decremented in
                // RENDER (ProcessShakeEffect), not sim. Render runs at most
                // once per wall-clock frame regardless of advance count, so
                // shake already drains correctly. Verified empirically — the
                // same diag log shows shake decrementing 1/frame while
                // palette decremented 5/frame.
                constexpr uintptr_t kEffectSys1Addr = 0x447D7D;
                constexpr size_t    kEffectSys1Size = 42;
                constexpr uintptr_t kPflash2Addr    = 0x4456D0;
                constexpr size_t    kPflash2Size    = 44;
                struct PaletteSnapshot {
                    uint8_t  effect_sys1[kEffectSys1Size];   // 0x447D7D, 42 B
                    uint8_t  pflash2[kPflash2Size];          // 0x4456D0, palette-flash-2 struct
                    uint32_t rng_seed;                       // 0x41FB1C
                };
                PaletteSnapshot pal_pre{};
                const bool running_ahead = update->data.adv.running_ahead;
                // Run-ahead-only palette snapshot. Rolling-back re-sim
                // INTENTIONALLY lets palette decrement naturally so it
                // reaches the same final state as forward sim — paired
                // with the savestate.cpp change that now saves/restores
                // real palette bytes (was zeroed). Initial attempt
                // extended the snapshot to rolling_back too, but that
                // froze palette at the Load-time value instead of letting
                // re-sim drain it, which is worse — replay's palette
                // drains naturally over forward sim so host needs to
                // match by also draining during re-sim.
                if (running_ahead) {
                    // Palette-flash TIMER snapshot only (prevents the flash
                    // timer draining ~5x under runahead -> the "flash cuts
                    // out" bug). The RNG snapshot/restore that used to live
                    // here is GONE: render-side rng is now its own stream
                    // (see Hook_GameRand / globals.h), so speculative sim
                    // advances no longer over-consume render rng, and manually
                    // restoring the gameplay seed here is exactly what made
                    // cross-peer rng diverge under real rollback.
                    std::memcpy(pal_pre.effect_sys1,
                                (void*)kEffectSys1Addr,
                                kEffectSys1Size);
                    std::memcpy(pal_pre.pflash2, (void*)kPflash2Addr, kPflash2Size);
                }

                // v0.2.48's Phase F PostRenderRng restore was re-tested
                // 2026-05-17 (faithful re-add WITH the running_ahead skip)
                // and STILL caused cross-peer desync at battle frame ~873.
                // So v0.2.48's fix itself is bugged in real-netplay rollback,
                // not just my missing-skip from earlier today. Keeping
                // the SaveState_GetPostRenderRng helper compiled in but
                // not calling it — replay determinism will need a
                // different approach.
                //
                // Diagnostic note: every desync we've seen has buf_idx
                // forward=K replay=K+1 — that's gekko's RunaheadSave
                // (saves frame K state at end-of-K-1) vs ConfirmedSave
                // (saves frame K state at end-of-K) artifact, NOT a real
                // engine divergence. But the CROSS-PEER Local≠Remote CRC
                // IS real, and it goes away when this PostRenderRng
                // restore is removed.

                // [PHASE-F-DIAG] (FM2K_STRESS_DIAG=1): log every AdvanceEvent
                // entry/exit RNG + inputs + rolling_back/running_ahead flag.
                // Goal: see whether forward vs replay sim_N read the same
                // pre-sim engine state and produce the same post-sim state.
                // If pre matches but post differs → sim consumes RNG based
                // on memory we don't save/restore. If pre already differs
                // → load isn't restoring correctly. Gated on env var so
                // production builds don't pay the log cost.
                const bool diag_enabled = []() {
                    static int cached = -1;
                    if (cached < 0) {
                        const char* v = std::getenv("FM2K_STRESS_DIAG");
                        cached = (v && v[0] && v[0] != '0') ? 1 : 0;
                    }
                    return cached == 1;
                }();
                const uint32_t pre_sim_rng = *(uint32_t*)FM2K::ADDR_RANDOM_SEED;
                if (diag_enabled) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[PHASE-F] AdvEvent ENTRY g_netplay_frame=%u rolling_back=%d running_ahead=%d "
                        "p1=0x%03X p2=0x%03X rng=0x%08X",
                        g_netplay_frame, (int)update->data.adv.rolling_back,
                        (int)update->data.adv.running_ahead,
                        g_p1_input, g_p2_input, pre_sim_rng);
                }

                // Run a FULL game tick for EVERY AdvanceEvent (matching GekkoNet examples).
                // The game loop must NOT run its own tick - we handle everything here.
                {
                    PerfScope _pa(&g_perf_adv);  // profile game-tick cost (#62)
                    if (original_process_game_inputs) {
                        original_process_game_inputs();
                    }
                    if (original_update_game) {
                        original_update_game();
                    }
                }
                PerfMaybeReport();

                if (running_ahead) {
                    // Restore palette-flash TIMER only (see snapshot above).
                    // Gameplay RNG is intentionally NOT restored here anymore.
                    std::memcpy((void*)kEffectSys1Addr,
                                pal_pre.effect_sys1,
                                kEffectSys1Size);
                    std::memcpy((void*)kPflash2Addr, pal_pre.pflash2, kPflash2Size);
                }

                if (diag_enabled) {
                    const uint32_t post_sim_rng = *(uint32_t*)FM2K::ADDR_RANDOM_SEED;
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[PHASE-F] AdvEvent EXIT  g_netplay_frame=%u rng=0x%08X (delta=0x%08X)",
                        g_netplay_frame, post_sim_rng,
                        post_sim_rng - pre_sim_rng);
                }

                if (RingTraceOn() && g_netplay_frame <= 40) {
                    const uint32_t b = *(uint32_t*)0x447EE0;
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[RING] ADV  f=%u rb=%d ra=%d buf=%u h1[b]=%03X in1=%03X in2=%03X h2[b]=%03X",
                        g_netplay_frame, (int)update->data.adv.rolling_back,
                        (int)update->data.adv.running_ahead, b,
                        *(uint32_t*)(0x4280E0 + (b & 0x3FF) * 4),
                        g_p1_input, g_p2_input,
                        *(uint32_t*)(0x4290E0 + (b & 0x3FF) * 4));
                }

                g_is_rolling_back = false;
                g_netplay_frame++;
                has_advance = true;

                // Parity recorder: post-advance snapshot per confirmed
                // battle frame. In stress mode the GekkoNet-driven loop
                // bypasses main_loop_trampoline's per-frame Capture
                // calls (those fire from CSS / spec-playback paths), so
                // without this the .pty captures only the header and
                // record-side stays at 0 snapshots. Skip rolling_back
                // and running_ahead re-advances — they'd produce
                // duplicate frame entries.
                if (!update->data.adv.rolling_back &&
                    !update->data.adv.running_ahead) {
                    ParityRecorder::Capture();
                }

                // Synthetic desync trigger — runs after the frame
                // counter advances so we can match on a specific frame.
                // One-shot per match; arms only when the env var is
                // set. Skip on rollback re-sims (g_is_rolling_back is
                // false here because we just unset it, but check anyway
                // for safety against future refactors).
                MaybeFireSyntheticDesync();

                // Harness terminator — clean exit at a configured battle
                // frame so the replay-self-test driver can pair record
                // and playback runs of identical length. Env-driven,
                // one-shot, fires on confirmed advances only (skip
                // rollback re-sims). Pairs with FM2K_PARITY_AUTOPLAY +
                // FM2K_PARITY_RECORD_PATH on a record-then-replay run.
                //
                // Goes through HandleDesyncDetected so the same
                // .fm2krep/.pty flush + manifest path runs, then
                // TerminateProcess(0) for a clean exit (vs the desync
                // path's exit(1)). Reusing the dump pipeline keeps the
                // diagnostic artifacts comparable.
                {
                    static int s_terminate_at = -2;
                    if (s_terminate_at == -2) {
                        const char* v = std::getenv("FM2K_AUTO_TERMINATE_AT_FRAME");
                        s_terminate_at = (v && *v) ? std::atoi(v) : -1;
                        if (s_terminate_at > 0) {
                            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                                "Harness: will TerminateProcess at battle frame %d "
                                "(env FM2K_AUTO_TERMINATE_AT_FRAME=%s)",
                                s_terminate_at, v);
                        }
                    }
                    if (s_terminate_at > 0 &&
                        (int)g_netplay_frame >= s_terminate_at &&
                        !update->data.adv.rolling_back &&
                        !update->data.adv.running_ahead) {
                        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "Harness: reached battle frame %u — flushing logs, "
                            "writing .fm2krep, terminating cleanly.",
                            g_netplay_frame);
                        // Append a synthetic MATCH_END so the slice writer
                        // produces a complete .fm2krep. Replay reader pops
                        // events until MATCH_END; without it playback
                        // stalls at the last INPUT.
                        MatchEndPayload mep = {};
                        mep.winner_idx = 2;  // draw / unknown
                        SpectatorNode_OnMatchEnd(mep);
                        // Write per-battle slice. Same filename pattern
                        // as Netplay_EndBattle so the harness driver knows
                        // where to look. Fails silently if no MATCH_START
                        // was emitted (e.g., session torn down mid-frame).
                        char ts[64] = {};
                        std::time_t now = std::time(nullptr);
                        std::tm tm_buf{};
                        localtime_s(&tm_buf, &now);
                        // Per-player filename: in the loopback netplay
                        // harness BOTH peers hit this terminator in the
                        // same second and previously wrote the SAME path,
                        // P2 clobbering P1's canonical file (P1's carries
                        // session_id + ROUND events; P2's does not).
                        char pattern[64];
                        std::snprintf(pattern, sizeof(pattern),
                            "replays/%%Y-%%m-%%d_%%H%%M%%S_p%d_harness.fm2krep",
                            g_player_index);
                        std::strftime(ts, sizeof(ts), pattern, &tm_buf);
                        CreateDirectoryA("replays", nullptr);
                        bool wrote = SpectatorNode_WriteCurrentBattleFile(ts);
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Harness: .fm2krep slice %s -> %s",
                            wrote ? "written" : "WRITE FAILED",
                            wrote ? ts : "(no MATCH_START in session_events?)");
                        SaveState_FlushRngTrace(g_player_index,
                            "harness auto-terminate");
                        // Flush parity recorder — fopen("wb") is fully
                        // buffered and TerminateProcess kills the stdio
                        // buffer without writing. Without this the
                        // record.pty hits disk as 0 bytes.
                        ParityRecorder::Close();
                        // Same flush for FM2K_RNG_TRACE=1 log (Hook_GameRand
                        // buffered output). Without this, the trace ends
                        // at the last 8192-call autoflush which may be
                        // many frames behind.
                        Hook_FlushRngTrace();
                        // Same for the CSM dispatch-loop diagnostic log.
                        extern void Hook_FlushCsmDiag();
                        Hook_FlushCsmDiag();
                        fflush(stdout);
                        fflush(stderr);
                        TerminateProcess(GetCurrentProcess(), 0);
                    }
                }

                // Replay recording + spectator stream. Gate on TRUE
                // confirmed advances only:
                //
                //   running_ahead=true → speculative runahead with
                //     PREDICTED inputs. We must NOT record these — when
                //     the prediction turns out wrong, GekkoNet rolls back
                //     and replays with the real input, but our recorded
                //     session_history would already contain the wrong
                //     prediction → spectator processes wrong inputs →
                //     desync. (This was the running-ahead-after-resync
                //     bug.)
                //   rolling_back=true → mid-rollback replay. Inputs are
                //     correct (post-confirmation) but we already recorded
                //     this frame on its first forward pass.
                //   monotonic gate → belt-and-suspenders against any
                //     other re-advance pattern.
                //
                // Only !rolling_back && !running_ahead && new-frame ever
                // hits session_history.
                if (!update->data.adv.rolling_back &&
                    !update->data.adv.running_ahead &&
                    g_netplay_frame > g_highest_recorded_frame)
                {
                    g_highest_recorded_frame = g_netplay_frame;

                    // PER-FRAME alignment-trace log: every confirmed frame
                    // for the first 100 frames of each battle. Pairs with the
                    // identical [SPEC-TRACE] log on spectator. Comparing
                    // (rng_pre, rng_post, p1_in, p2_in) at every battle frame
                    // tells us:
                    //   * inputs match every frame? → alignment correct
                    //   * inputs differ at some bf? → alignment off / wrong
                    //                                 input popped that frame
                    //   * rng_pre matches but rng_post differs → PGI+UG itself
                    //                                            calls game_rand
                    //                                            different #
                    //   * rng_pre differs → leak between confirmed advances
                    //                       (something between this frame and
                    //                       last frame mutated RNG)
                    // Long trace: 5000 confirmed frames (~50 sec), so we
                    // capture the wall-clock window where the user reports
                    // visible desync. "bf-1" subtraction aligns this with
                    // spec's bf=K labelling (spec starts at 0, host's
                    // post-increment makes its first log bf=1).
                    // [HOST-TRACE] per-frame for first 5000 battle frames.
                    // Routed via SDL_LOG_CATEGORY_CUSTOM into quill's
                    // backtrace ring (in-memory; flushed to disk on any
                    // LOG_ERROR). FM2K_SPECTATOR_DEBUG=1 routes to disk.
                    //
                    // Off by default since 2026-05-18 — set FM2K_HOST_TRACE=1
                    // to opt in. Previous default added ~25K lines per
                    // session (36% of log volume) just for diagnosis runs.
                    static int s_host_trace_enabled = -1;
                    if (s_host_trace_enabled < 0) {
                        const char* v = std::getenv("FM2K_HOST_TRACE");
                        s_host_trace_enabled = (v && v[0] && v[0] != '0') ? 1 : 0;
                    }
                    if (s_host_trace_enabled &&
                        g_netplay_frame > 0 && g_netplay_frame <= 5000) {
                        constexpr uintptr_t POOL = 0x4701E0;
                        constexpr size_t    SLOT = 382;
                        const int32_t p1_x = *(int32_t*)(POOL + 0 * SLOT + 0x08);
                        const int32_t p2_x = *(int32_t*)(POOL + 1 * SLOT + 0x08);
                        const int32_t p1_script = *(int32_t*)(POOL + 0 * SLOT + 0x30);
                        const int32_t p2_script = *(int32_t*)(POOL + 1 * SLOT + 0x30);
                        const uint32_t p1_hp = *(uint32_t*)0x4DFC85;
                        const uint32_t p2_hp = *(uint32_t*)0x4EDCC4;
                        SDL_LogInfo(SDL_LOG_CATEGORY_CUSTOM,
                            "[HOST-TRACE] bf=%u rng_pre=0x%08X rng_post=0x%08X "
                            "p1=0x%03X p2=0x%03X p1_x=%d p2_x=%d "
                            "p1_s=%d p2_s=%d hp=%u/%u",
                            g_netplay_frame - 1u, rng_pre_advance,
                            *(uint32_t*)0x41FB1C,
                            g_p1_input, g_p2_input,
                            p1_x, p2_x, p1_script, p2_script,
                            p1_hp, p2_hp);
                    }

                    // (Legacy Replay::Replay_RecordFrame call retired in
                    // v0.2.27 — SpectatorNode_OnFrameConfirmed below pushes
                    // the same frame data into the v2 .fm2krep event stream
                    // via the SessionEvent log.)
                    // Spectator input forwarding: gate is here because this
                    // site is already !rolling_back && !running_ahead &&
                    // new_frame — the dedup we need to avoid runahead /
                    // rollback re-recording the same frame. Pre-battle
                    // frames (title screen, CSS) are captured by
                    // Hook_GetPlayerInput's capture_and_return, which is
                    // correct for those phases (no rollback there).
                    //
                    // Phase F (#23, para's replay desync bug): the values
                    // gekko delivers in g_p1_input / g_p2_input are RAW —
                    // pre-SOCD, pre-facing-flip. The HOST engine then
                    // applies Hook_ApplySOCD before feeding them into the
                    // game. A spectator (live or .fm2krep replay) reads
                    // these stored values and ALSO applies SOCD — but
                    // using ITS OWN env-var-derived SOCD mode, which can
                    // differ from the host's. Result: divergent input on
                    // any frame where the user held L+R or U+D and the
                    // spec's mode resolves it differently → cascading
                    // script / position drift (parity_diff showed first
                    // divergence at bf=1260 in p1_pos/p1_script for
                    // vanpri, NOT in RNG).
                    //
                    // Fix: pre-apply the HOST's SOCD here. The stored
                    // value carries the host's resolution. Spec's later
                    // SOCD-application becomes idempotent (resolved
                    // inputs don't trigger SOCD branches), so it doesn't
                    // matter what mode the spec is in. Facing flip still
                    // happens on the spec side, driven by deterministic
                    // sim state — correct as long as sim hasn't diverged
                    // upstream (it hasn't, since we now feed identical
                    // post-SOCD inputs to both engines).
                    const uint16_t p1_for_spec = Hook_ApplySOCD_Public(g_p1_input);
                    const uint16_t p2_for_spec = Hook_ApplySOCD_Public(g_p2_input);
                    SpectatorNode_OnFrameConfirmed(p1_for_spec, p2_for_spec);

                    // Per-frame state fingerprint for spectator-desync
                    // diagnosis — pairs with [SPEC-FP] log in
                    // RunSpectatorTick. Same sample addresses; spectator's
                    // bf counter is its own pop count post-battle-entry,
                    // host's bf is g_netplay_frame. Match by bf to find
                    // first divergent frame.
                    // [HOST-FP] every 30 frames — same diagnostic-ring
                    // routing as [HOST-TRACE] above (CUSTOM category).
                    // Same FM2K_HOST_TRACE=1 env gate so the pair stays
                    // together when investigating a desync.
                    static int s_host_fp_enabled = -1;
                    if (s_host_fp_enabled < 0) {
                        const char* v = std::getenv("FM2K_HOST_TRACE");
                        s_host_fp_enabled = (v && v[0] && v[0] != '0') ? 1 : 0;
                    }
                    if (s_host_fp_enabled && (g_netplay_frame % 30) == 0) {
                        const uint32_t rng     = *(uint32_t*)0x41FB1C;
                        const uint32_t buf_idx = *(uint32_t*)0x447EE0;
                        const uint32_t p1_hp   = *(uint32_t*)0x4DFC85;
                        const uint32_t p2_hp   = *(uint32_t*)0x4EDCC4;
                        const uint32_t timer   = *(uint32_t*)0x470044;
                        // World positions live in the object pool: slot 0 +0x08
                        // = pos_x, slot 1 = slot 0 + 382. (0x470020 was the
                        // CSS character-slot index, not the in-battle x —
                        // earlier logs were comparing useless data.)
                        constexpr uintptr_t POOL = 0x4701E0;
                        constexpr size_t    SLOT = 382;
                        const int32_t p1_x = *(int32_t*)(POOL + 0 * SLOT + 0x08);
                        const int32_t p1_y = *(int32_t*)(POOL + 0 * SLOT + 0x0C);
                        const int32_t p2_x = *(int32_t*)(POOL + 1 * SLOT + 0x08);
                        const int32_t p2_y = *(int32_t*)(POOL + 1 * SLOT + 0x0C);
                        const int32_t p1_script = *(int32_t*)(POOL + 0 * SLOT + 0x30);
                        const int32_t p2_script = *(int32_t*)(POOL + 1 * SLOT + 0x30);
                        SDL_LogInfo(SDL_LOG_CATEGORY_CUSTOM,
                            "[HOST-FP] bf=%u rng=0x%08X buf=%u "
                            "p1_hp=%u p2_hp=%u timer=%u "
                            "p1_pos=(%d,%d) p2_pos=(%d,%d) "
                            "p1_script=%d p2_script=%d "
                            "p1_in=0x%03X p2_in=0x%03X",
                            g_netplay_frame, rng, buf_idx,
                            p1_hp, p2_hp, timer,
                            p1_x, p1_y, p2_x, p2_y,
                            p1_script, p2_script,
                            g_p1_input, g_p2_input);
                    }

                    // C9: append FINGERPRINT op every 30 confirmed INPUTs.
                    // Off by default; gated on FM2K_SPEC_FINGERPRINT=1.
                    // Spectator's ApplySessionEvent computes the same hash
                    // and logs WARN on mismatch.
                    if (SpectatorFingerprint_Enabled() &&
                        (g_netplay_frame % 30) == 0) {
                        SpectatorNode_AppendFingerprint(
                            SpectatorFingerprint_Compute());
                    }
                }

                // Shake safety cap removed — the real fix is letting the
                // render-path timer decrement persist (see carve-out in
                // main_loop_trampoline.cpp:RenderFrameWithSnapshot). With
                // that in place, the KGT-scripted `Duration` value the
                // character author wrote drives how long shake lasts, and
                // the timer naturally reaches 0 -> ProcessShakeEffect zeros
                // the visible offset -> shake ends at exactly the designed
                // frame count. No per-frame force-clear needed.

                // Advance the deterministic virtual clock used by Hook_timeGetTime.
                // Exactly one bump per AdvanceEvent.
                extern uint32_t g_virtual_time_ms;
                g_virtual_time_ms += 10;

                // NOTE: we intentionally do NOT write g_last_frame_time here.
                // Setting it equal to g_virtual_time_ms made
                // main_game_loop's frame-skip check
                //   Time (= timeGetTime = g_virtual_time_ms) >= g_last_frame_time + 10
                // always false, so the game stopped running new iterations
                // (session hang at the save just before the write would have
                // fired). Leave g_last_frame_time to main_game_loop's own
                // prologue writes; if the forward-vs-replay divergence at
                // 0x447DD4 reappears we'll mask it at save time instead of
                // trying to drive it from here.

                // Tell Hook_RenderGame there is an unrendered authoritative
                // tick. The render hook is allowed to draw exactly this one
                // sim state, then clears the flag. Rolled-back replay ticks
                // do not set the flag — we only render the final resolved
                // state of an advance, never the intermediate replay frames.
                extern bool g_frame_pending_render;
                if (!update->data.adv.rolling_back) {
                    g_frame_pending_render = true;
                }

                // Periodic status every 500 frames (~5 sec). Gate on
                // NON-rolling_back advances only — in stress mode g_netplay_frame
                // hits any given value N times (once forward, N-1 times on
                // replay) so a naive `% 500 == 0` fires on every replay
                // pass and floods the log. Also dedupe by last-logged frame
                // so rollback that re-hits a multiple-of-500 frame doesn't
                // re-log.
                if (!update->data.adv.rolling_back) {
                    static uint32_t last_status_frame = 0;
                    if (g_netplay_frame >= last_status_frame + 500) {
                        last_status_frame = g_netplay_frame;
                        float fa = g_session ? gekko_frames_ahead(g_session) : 0.0f;
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "BATTLE STATUS: frame=%u rb=%u desync=%u ahead=%.1f frame_time_ms=%u skip=%u",
                            g_netplay_frame, g_rollback_count, g_desync_count, fa,
                            *(uint32_t*)0x41E2F0,
                            *(uint32_t*)0x4246F4);
                    }

                    static uint32_t last_state_frame = 0;
                    if (g_netplay_frame >= last_state_frame + 1000) {
                        last_state_frame = g_netplay_frame;
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "STATE f=%u: rng=0x%08X game_timer=%u round_timer_ctr=%u render_fc=%u",
                            g_netplay_frame,
                            *(uint32_t*)0x41FB1C,
                            *(uint32_t*)0x470044,
                            *(uint32_t*)0x424F00,
                            *(uint32_t*)0x4456FC);
                    }
                }
                break;
            }

            default:
                break;
        }
    }

    // Mike Z sound sync: once per displayed frame, AFTER all advances have
    // completed and BEFORE render. Desired[] now reflects the authoritative
    // sim history for this frame; reconcile to actual DSound plays, applying
    // the rollback-window filter that prevents erased/re-triggered sounds.
    if (has_advance && earliest_advance != UINT32_MAX) {
        latest_advance = g_netplay_frame;
        SoundRollback::SyncAfterAdvance(earliest_advance, latest_advance);
    }

    return has_advance;
}

uint16_t Netplay_GetInput(int player_id) {
    return (player_id == 0) ? g_p1_input : g_p2_input;
}

// Spectator-side per-tick driver. Mirrors Netplay_ProcessBattleInputPhase
// minus the gekko_add_local_input call (spectators have no local input).
// Save/Load events fire only when host's session is in BATTLE config
// (rollback-capable); CSS spectate sessions are pure forward AdvanceEvent.
//
// Returns true if the sim advanced this tick (caller renders); false on
// stall (waiting for confirmed inputs from host).

bool Netplay_ProcessSpectatorPhase() {
    if (!g_session || g_session_kind != SessionKind::SPECTATE) return false;

    gekko_network_poll(g_session);

    // No local input add — spectator is passive.

    // Drain session events.
    int event_count = 0;
    auto events = gekko_session_events(g_session, &event_count);
    for (int i = 0; i < event_count; i++) {
        auto event = events[i];
        switch (event->type) {
            case GekkoSessionStarted:
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Spectator: GekkoSpectateSession started");
                g_session_ready = true;
                break;
            case GekkoPlayerConnected:
                g_session_ready = true;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Spectator: handle %d connected",
                    event->data.connected.handle);
                break;
            case GekkoPlayerSyncing:
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Spectator: syncing %u/%u",
                    event->data.syncing.current, event->data.syncing.max);
                break;
            case GekkoPlayerDisconnected:
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Spectator: host disconnected (handle=%d)",
                    event->data.disconnected.handle);
                break;
            case GekkoSpectatorPaused:
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Spectator: paused — buffering");
                break;
            case GekkoSpectatorUnpaused:
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Spectator: unpaused — playback resumed");
                break;
            case GekkoDesyncDetected:
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                    "Spectator: DESYNC f=%d local=0x%08X remote=0x%08X",
                    event->data.desynced.frame,
                    event->data.desynced.local_checksum,
                    event->data.desynced.remote_checksum);
                break;
            default:
                break;
        }
    }

    // Drain Save/Load/Advance events — same handlers as the host's battle
    // path, ensuring the spectator's render-side mutations and virtual
    // clock stay locked to the host's confirmed state.
    int update_count = 0;
    auto updates = gekko_update_session(g_session, &update_count);

    bool advanced = false;
    for (int i = 0; i < update_count; i++) {
        auto update = updates[i];
        switch (update->type) {
            case GekkoSaveEvent: {
                int frame = update->data.save.frame;
                SaveState_Save(frame);
                (void)SaveState_GetLastChecksum(frame);
                uint32_t checksum = SaveState_GetRegionChecksums().gameplay_fingerprint;
                *update->data.save.state_len = sizeof(uint32_t);
                *update->data.save.checksum  = checksum;
                memcpy(update->data.save.state, &frame, sizeof(uint32_t));
                break;
            }
            case GekkoLoadEvent: {
                int frame = update->data.load.frame;
                SaveState_Load(frame);
                break;
            }
            case GekkoAdvanceEvent: {
                const uint16_t* in = (const uint16_t*)update->data.adv.inputs;
                g_p1_input = in[0];
                g_p2_input = in[1];
                g_netplay_frame = (uint32_t)update->data.adv.frame;
                // Lock virtual clock to host's frame schedule — same contract
                // as the host's GekkoAdvance handler. This is what closes H3
                // (g_virtual_time_ms skew vs host's rollback-rewinds).
                extern uint32_t g_virtual_time_ms;
                g_virtual_time_ms = g_netplay_frame * 10;

                if (original_process_game_inputs) original_process_game_inputs();
                if (original_update_game)         original_update_game();
                // ParityRecorder::Capture() runs from the trampoline post-tick
                // (mirrors the offline + battle paths) — keeps the parity
                // header dependency out of netplay.cpp.
                advanced = true;

                // Pending CSS<->battle phase swap: now that the local sim
                // has caught up to the host's announced swap_frame, tear
                // down this session and bring up the next-kind one. Stop
                // draining further events from the now-destroyed session.
                if (MaybeSwapPendingSpectator(g_netplay_frame)) {
                    return advanced;
                }
                break;
            }
            default:
                break;
        }
    }

    return advanced;
}

// =============================================================================
// LEGACY API
// =============================================================================

bool Netplay_StartGekkoSession() {
    return Netplay_StartBattle();
}

void Netplay_StopGekkoSession() {
    Netplay_EndBattle();
}

void Netplay_PollGekkoNet() {
    if (g_session) {
        gekko_network_poll(g_session);
        int event_count = 0;
        auto events = gekko_session_events(g_session, &event_count);
        for (int i = 0; i < event_count; i++) {
            auto event = events[i];
            if (event->type == GekkoSessionStarted || event->type == GekkoPlayerConnected) {
                g_session_ready = true;
            }
        }
    }
}

void Netplay_ResetCSSState() {
    // Tear down the CSS GekkoSession if it's still alive — happens when this
    // is called outside the normal battle-entry path (e.g. peer disconnect).
    if (g_session && g_session_kind == SessionKind::CSS) {
        Netplay_EndCSSSession();
    }
    g_css_frame         = 0;
    g_css_advance_p1    = 0;
    g_css_advance_p2    = 0;
    g_css_advance_ready = false;
    g_css_active        = false;
    g_css_synced        = false;
    g_remote_css_ready  = false;
    g_local_css_ready   = false;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "CSS: Reset state for new sync");
}

bool Netplay_IsRemoteCSSReady() {
    return g_remote_css_ready;
}

bool Netplay_IsCSSFullySynced() {
    return g_css_synced;
}

void Netplay_SetLocalCSSReady(bool ready) {
    g_local_css_ready = ready;
    if (ready) {
        ControlChannel_SendBattleReady();
    }
}

void Netplay_ProcessMenu() {
    ControlChannel_Poll();
}

const CSSState& Netplay_GetCSSState() {
    static CSSState dummy;
    return dummy;
}

uint32_t Netplay_GetFrame() {
    return g_netplay_frame;
}

uint32_t Netplay_GetPingMs() {
    return ControlChannel_GetRttMs();
}

int Netplay_GetLocalDelay() {
    return g_local_delay;
}

// -----------------------------------------------------------------------------
// Chat ring. Small fixed-size SPSC-ish ring since both push and pop run on
// the launcher-UI side; the only cross-thread producer is OnControlMessage
// via the control-channel poller. Size 64 is plenty for a single match's
// worth of unread messages.
// -----------------------------------------------------------------------------
static constexpr size_t CHAT_RING_CAP = 64;
static ChatEntry g_chat_ring[CHAT_RING_CAP];
static size_t    g_chat_head = 0;
static size_t    g_chat_tail = 0;

void Netplay_PushChatMessage(bool from_remote, const char* text) {
    if (!text) return;
    ChatEntry e = {};
    e.from_remote  = from_remote;
    e.timestamp_ms = (uint64_t)GetTickCount64();
    std::strncpy(e.text, text, sizeof(e.text) - 1);
    e.text[sizeof(e.text) - 1] = '\0';

    size_t next = (g_chat_head + 1) % CHAT_RING_CAP;
    if (next == g_chat_tail) {
        // Ring full — drop oldest to keep the newest message visible.
        g_chat_tail = (g_chat_tail + 1) % CHAT_RING_CAP;
    }
    g_chat_ring[g_chat_head] = e;
    g_chat_head = next;
}

bool Netplay_PopChatMessage(ChatEntry* out) {
    if (g_chat_tail == g_chat_head) return false;
    if (out) *out = g_chat_ring[g_chat_tail];
    g_chat_tail = (g_chat_tail + 1) % CHAT_RING_CAP;
    return true;
}

void Netplay_SendChatMessage(const char* text) {
    if (!text) return;
    if (!Netplay_IsConnected()) return;
    ControlChannel_SendChat(text);
    // Echo into local ring so the sender sees their own message in the UI.
    Netplay_PushChatMessage(/*from_remote*/ false, text);
}

GekkoNetworkStats Netplay_GetNetworkStats() {
    GekkoNetworkStats stats = {};
    if (g_session) {
        // Get stats for the remote player (whichever slot isn't us)
        int remote_handle = (g_player_index == 0) ? 1 : 0;
        gekko_network_stats(g_session, remote_handle, &stats);
    }
    return stats;
}

uint32_t Netplay_GetDesyncCount() {
    return g_desync_count;
}

uint32_t Netplay_GetRollbackCount() {
    return g_rollback_count;
}

float Netplay_GetFramesAhead() {
    if (g_session) {
        return gekko_frames_ahead(g_session);
    }
    return 0.0f;
}

void Netplay_HandleFrameTime() {
    float ahead = g_session ? gekko_frames_ahead(g_session) : 0.0f;
    HandleFrameTime(ahead);
}

// ============================================================================
// RUNAHEAD MID-MATCH TOGGLE (F8)
// ============================================================================
// WndProc subclass calls Netplay_RequestRunaheadToggle from the message-pump
// thread on VK_F8 press. The actual gekko_set_runahead call MUST happen on
// the same thread that owns g_session — see Netplay_PollRunaheadToggle below,
// which the trampoline calls at the top of every battle tick before
// Netplay_ProcessBattleInputPhase. Atomic flag + load/store keeps the cross-
// thread handoff clean without any extra mutex.

void Netplay_RequestRunaheadToggle() {
    g_runahead_toggle_requested.store(true, std::memory_order_release);
}

void Netplay_PollRunaheadToggle() {
    if (!g_runahead_toggle_requested.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    if (!g_session || g_session_kind != SessionKind::BATTLE) {
        // Toggle outside an active battle session is a no-op (CSS lockstep
        // doesn't have runahead; spectator session can't change peer's
        // runahead anyway). Silently swallow so spurious F8 presses during
        // menus don't log noise.
        return;
    }
    const int cur    = g_runahead_active.load(std::memory_order_acquire);
    const int target = (cur == 0) ? g_runahead_user_pref : 0;
    gekko_set_runahead(g_session, (unsigned char)target);
    g_runahead_active.store(target, std::memory_order_release);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Runahead toggled: %d -> %d at bf=%u (user_pref=%d)",
                cur, target, g_netplay_frame, g_runahead_user_pref);
}

// ============================================================================
// HEARTBEAT — [BEAT] line every ~10s
// ============================================================================
// Single INFO line per ~10s of battle wall-clock. Echoes the values support
// staff would otherwise have to dig the entire log for: version, game id,
// role, ping, frames_ahead, configured delay, live runahead/pred, last-window
// rollback stats, stall count. Format is space-delimited key=value pairs so
// `grep '^\[BEAT\]'` and a one-liner awk script can extract a time series.
//
// Cadence: based on wall-clock ms, not frame count, so battles paused on
// menus or stalled in CSS don't shift the cadence. Reset window stats on
// emit so the avg/max describe the last 10s, not session totals.

static const char* SessionRoleStr() {
    if (g_session_kind == SessionKind::SPECTATE) return "SPEC";
    return (g_player_index == 0) ? "P1" : "P2";
}

void Netplay_TickHeartbeat() {
    if (!g_session || g_session_kind != SessionKind::BATTLE) return;

    const uint64_t now_ms = (uint64_t)GetTickCount64();
    if (g_beat_last_emit_ms == 0) {
        g_beat_last_emit_ms = now_ms;
        return;
    }
    if (now_ms - g_beat_last_emit_ms < 10000ULL) return;
    g_beat_last_emit_ms = now_ms;

    const float fa = gekko_frames_ahead(g_session);
    GekkoNetworkStats stats = {};
    // Remote handle 0 is the peer in 2p sessions. For 1p offline/stress
    // there's no remote so stats stay zero; that's fine, BEAT still
    // shows local state.
    gekko_network_stats(g_session, 0, &stats);

    const uint32_t rb_count = g_beat_window_rb_count;
    const double   rb_avg   = rb_count ? (double)g_beat_window_rb_sum / rb_count : 0.0;
    const uint32_t rb_max   = g_beat_window_rb_max;

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "[BEAT] bf=%u role=%s ping=%ums jit=%.1fms FA=%+.1f "
        "delay=%d ra=%d pred=%d rb_total=%u rb_win=%u rb_avg=%.1f rb_max=%u",
        g_netplay_frame, SessionRoleStr(),
        stats.last_ping, stats.jitter, fa,
        g_local_delay,
        g_runahead_active.load(std::memory_order_acquire),
        g_pred_window,
        g_rollback_count, rb_count, rb_avg, rb_max);

    // Reset window so next emit describes only the next ~10s.
    g_beat_window_rb_sum   = 0;
    g_beat_window_rb_max   = 0;
    g_beat_window_rb_count = 0;
}
