// CCCaster-Style Netplay Implementation
// - Control channel for CSS input sync using INPUT DELAY (not lockstep)
// - GekkoNet for battle mode rollback
// - Uses game's internal timer for frame counting
#include "netplay.h"
#include "netplay_internal.h"  // shared file-scope state, externed for the split netplay_*.cpp TUs
#include "../hooks/hooks.h"   // Hook_ApplySOCD_Public for SOCD-pre-apply on spec capture
#include "../hooks/css_autoconfirm.h"  // CssAutoConfirm_OnReplayMatchStart (TEST_CSS_CHAR pin)
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

// SimpleState enum now lives in netplay_internal.h (shared with the split TUs).
SimpleState g_simple_state = SimpleState::DISCONNECTED;

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

// SessionKind alias now lives in netplay_internal.h (shared with the split TUs).
SessionKind g_session_kind = SessionKind::NONE;

// CSS lockstep parameters (ported from the legacy CCCaster-style ring-buffer
// implementation). With GekkoNet's prediction=0 mode, local_delay is the
// per-player input commitment delay — peer A's input committed at frame F
// becomes the input applied at frame F+local_delay, identical semantics to
// the previous "store at frame+delay, read at frame" model.

// CSS state — input transport now lives inside the CSS GekkoSession
bool g_css_active        = false;  // Currently in CSS mode (game-side detection)
bool g_css_synced        = false;  // Both peers BATTLE_READY, CSS GekkoSession ready
bool g_remote_css_ready  = false;  // Remote has entered CSS
bool g_local_css_ready   = false;  // We've entered CSS
uint32_t g_css_frame     = 0;      // Last confirmed CSS AdvanceEvent frame (+1)

// Per-poll AdvanceEvent input cache. Netplay_ProcessCSS drives gekko_update_session,
// which fires AdvanceEvent only when both peers have inputs for the current frame
// (lockstep guarantee). The cached inputs are read by Hook_GetPlayerInput via
// Netplay_GetCSSInput.
uint16_t g_css_advance_p1     = 0;
uint16_t g_css_advance_p2     = 0;
bool     g_css_advance_ready  = false;

// GekkoNet session pointer + readiness flag (shared by CSS and battle —
// only one is alive at a time, distinguished by g_session_kind).
GekkoSession* g_session = nullptr;
bool g_session_ready = false;
uint16_t g_p1_input = 0;
uint16_t g_p2_input = 0;
uint32_t g_netplay_frame = 0;

// Rollback tracking
uint32_t g_rollback_count = 0;
uint32_t g_last_rollback_frame = 0;
uint32_t g_desync_count = 0;
uint32_t g_last_desync_log_tick = 0;
int g_local_delay = 1;  // Computed from RTT at battle start

// Tuning knobs — set at battle session start, mirrored here so
// Netplay_TickHeartbeat can echo the live values into [BEAT] lines.
// `g_runahead_user_pref` is the configured "on" value (env / future
// UI default); `g_runahead_active` is the value actually applied to
// the running session right now, which can differ when the user
// hits F8 mid-match to toggle between 0 and user_pref.
int                g_pred_window               = 16;
// Runahead is DEFAULT OFF (too CPU-heavy at 100fps/10ms budget). These get
// overwritten per battle in Netplay_StartBattle (pref = local_delay, active
// = 0 unless FM2K_RUNAHEAD forces it); the static defaults just keep it off
// before/between matches.
int                g_runahead_user_pref        = 0;
std::atomic<int>   g_runahead_active{0};
std::atomic<bool>  g_runahead_toggle_requested{false};

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
uint32_t g_beat_window_rb_sum   = 0;
uint32_t g_beat_window_rb_max   = 0;
uint32_t g_beat_window_rb_count = 0;   // real rollbacks observed
uint64_t g_beat_last_emit_ms    = 0;

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
    // Phantom-checksum guard (2026-06-11 16:31, battle-2 f=1536): gekko's
    // SendSessionHealthCheck reads _storage.GetState(confirmed) and only
    // ASSERTS the slot actually holds that frame -- compiled out in
    // release, so a wrapped/unwritten slot transmits checksum 0 and the
    // peer kills itself on a phantom mismatch (P2 saw remote=0x00000000
    // while P1's own comparator stayed silent, i.e. the real checksums
    // matched). Our save callback always writes a nonzero fingerprint,
    // so a zero on either side is never a genuine state hash; a real
    // divergence keeps firing with nonzero pairs on subsequent frames.
    if (!synthetic && (local_chk == 0 || remote_chk == 0)) {
        static uint32_t s_phantom_count = 0;
        if (s_phantom_count++ < 8 || (s_phantom_count & 0x3F) == 0) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "DESYNC ignored (phantom #%u): f=%d local=0x%08X "
                "remote=0x%08X -- zero checksum is a gekko health-slot "
                "artifact, not a state hash",
                s_phantom_count, frame, local_chk, remote_chk);
        }
        return;
    }
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
int g_force_desync_at_frame = -1;
bool g_force_desync_inited = false;

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
int  g_session_delay_cached       = 0;
bool g_session_delay_cache_valid  = false;

// Highest frame number we've ever recorded into the replay/spectator stream.
// Reset on each Netplay_StartBattle (g_netplay_frame also resets to 0).
// Gates the GekkoAdvance recording path against runahead duplicates — each
// frame is advanced multiple times under runahead, but only the first
// monotonic forward crossing is "the" confirmed advance.
uint32_t g_highest_recorded_frame = 0;

// Confirmed-input recording ring (netplay battle sessions only). EVERY
// non-runahead advance -- speculative first pass AND rolling_back
// correction -- writes (frame, post-SOCD inputs) here, corrections
// overwriting predictions. The flush after the event loop emits entries
// to SpectatorNode_OnFrameConfirmed only once gekko_confirmed_frame()
// says ALL players' real inputs arrived for that frame. Without this,
// a predicting peer recorded speculative inputs into .fm2krep and the
// live spectator stream (cross-peer fork hunt, 2026-06-11).
// PendingConfirmInput struct now lives in netplay_internal.h.
PendingConfirmInput g_pending_confirm[PENDING_CONFIRM_RING];
uint32_t g_next_confirm_flush = 0;
void ResetConfirmRing() {
    for (auto& pc : g_pending_confirm) pc.frame = 0xFFFFFFFFu;
    g_next_confirm_flush = 0;
}

// Battle entry sync barrier - ensures both clients enter battle at same time.
// Also carries swap_frame negotiation for the CSS-session->battle-session swap:
// both peers propose g_css_frame + SWAP_FRAME_BUFFER on detection, exchange via
// BATTLE_ENTERING.data.sync.frame, and the agreed value is max(local, remote).
// The actual gekko_destroy(css)/gekko_create(battle) deferred until the active
// CSS session reaches g_battle_entry_swap_frame.
bool     g_local_battle_entered    = false;
bool     g_remote_battle_entered   = false;
bool     g_battle_synced           = false;
uint32_t g_battle_entry_swap_frame = 0;     // Latest agreed swap frame.

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
bool     g_battle_entry_armed      = false;

// Mirror gate for BATTLE_END to prevent the same stale-packet-poisoning
// pattern in the battle->CSS direction.
//
// Battle exit sync barrier (battle-session -> CSS-session swap, for rematch
// or return-to-menu). Mirrors the entry barrier but reads g_netplay_frame
// instead of g_css_frame and is driven by BATTLE_END instead of BATTLE_ENTERING.
bool     g_local_battle_end_signaled  = false;
bool     g_remote_battle_end_signaled = false;
bool     g_battle_end_synced          = false;
uint32_t g_battle_end_swap_frame      = 0;
bool     g_battle_end_armed           = false;

// Barrier epoch + completion records (deafness fix, 2026-06-11). The old
// protocol had a fatal asymmetry under loss: peer B completes the barrier
// the instant it has A's signal, swaps sessions, and disarms ~30ms later.
// If B's OWN signal packets were all lost in flight, A keeps resending
// into a peer that now drops everything at the armed gate without
// answering — A wedges in the barrier indefinitely (observed: 593
// BATTLE_ENTERING resends over 36s). With B's signal sent effectively
// once or twice in the short window, that's roughly a coin-flip per
// barrier at 20% loss. Fix: a peer that COMPLETED a barrier keeps
// ANSWERING retries for that barrier instance (identified by epoch)
// with a completed-flag signal, so the lagging peer can always finish.
// The epoch tag replaces "armed window" as the stale-vs-current
// disambiguator for these answers: both peers arm entry/end barriers in
// the same order (CSS-session-up / battle-session-up are bilateral
// rendezvous), so a uint8 counter stays in step; 0 is reserved for
// legacy peers and treated as a wildcard.
uint8_t  g_barrier_epoch        = 0;  // last assigned instance id
uint8_t  g_entry_epoch          = 0;  // armed entry barrier instance
uint8_t  g_end_epoch            = 0;  // armed end barrier instance
uint8_t  g_entry_done_epoch     = 0;  // last COMPLETED entry instance
uint8_t  g_end_done_epoch       = 0;  // last COMPLETED end instance
uint32_t g_entry_done_ms        = 0;  // completion wall-clock (legacy TTL)
uint32_t g_end_done_ms          = 0;
// Proposal pair captured for the divergence diagnostic at completion.
uint32_t g_entry_local_proposal  = 0;
uint32_t g_entry_remote_proposal = 0;

uint8_t NextBarrierEpoch() {
    ++g_barrier_epoch;
    if (g_barrier_epoch == 0) g_barrier_epoch = 1;  // 0 = legacy wildcard
    return g_barrier_epoch;
}

// Frames of slack added to the proposed swap_frame so both peers have time
// to drain in-flight inputs and converge their proposals before reaching it.
// 8 @ 100 FPS = 80ms — comfortably above typical RTT. Tunable.
constexpr uint32_t SWAP_FRAME_BUFFER = 8;

// Handshake state
bool g_received_hello = false;
bool g_received_hello_ack = false;

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
uint32_t g_pending_random_stage = 0xFFFFFFFFu;
extern "C" uint32_t Netplay_PeekNextRolledStage() {
    return g_pending_random_stage;
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

    // Fresh connection — restart the barrier epoch sequence on both sides.
    // (Netplay_EndBattle deliberately does NOT touch these: the completion
    // records must survive into the next CSS so a lagging peer's barrier
    // retries still get answered.)
    g_barrier_epoch    = 0;
    g_entry_epoch      = 0;
    g_end_epoch        = 0;
    g_entry_done_epoch = 0;
    g_end_done_epoch   = 0;
    g_entry_done_ms    = 0;
    g_end_done_ms      = 0;

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

// Iterate currently-subscribed spectators (from SpectatorNode's address list)
// and call gekko_add_actor(GekkoSpectator, &addr) for each on the active
// g_session. Called after the player actors are added in StartCSS/StartBattle.
// Late joiners (after session creation) are added directly in
// SpectatorNode_HandleJoinReq.

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
        // Phase F seam mirror: mark the seam stream so viewers know where
        // the results-screen inputs end and the CSS dance begins.
        SpectatorNode_AppendCssEntered();
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

        // Canonical CSS open (belt-and-braces for the swap-window input
        // guard in Hook_GetPlayerInput): no confirm state and no rematch
        // countdown may survive into the lockstep stream. The engine's
        // own CSS init zeroes these, so in a healthy run this writes 0
        // over 0 -- it only corrects state if some input leaked into the
        // unsynchronized window between CSS init and the first advance.
        *(uint32_t*)FM2K::ADDR_P1_ACTION_STATE = 0;
        *(uint32_t*)FM2K::ADDR_P2_ACTION_STATE = 0;
        if constexpr (FM2K::ADDR_ROUND_TIMER_COUNTER != 0) {
            *(uint32_t*)FM2K::ADDR_ROUND_TIMER_COUNTER = 0;
        }
        // Restart the harness-autoplay browse window for this CSS phase
        // (authoritative per-session reset; the in-function gap heuristic
        // only covers offline runs).
        Hook_AutoplayCssResetDwell();

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
        // as legitimate signaling. The epoch tags this barrier instance —
        // both peers arm here (bilateral CSS rendezvous) so counters match.
        g_battle_entry_armed = true;
        g_entry_epoch = NextBarrierEpoch();
        g_entry_local_proposal  = 0;
        g_entry_remote_proposal = 0;

        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "CSS SYNCED: Both ready, GekkoNet CSS session up, RNG reseeded");

        // Test-harness char pin (FM2K_TEST_CSS_CHAR=<grid_idx>[,<color>]):
        // arm CssAutoConfirm on BOTH live peers so the netplay CSS
        // deterministically selects a SPECIFIC character mirror instead
        // of confirming char 0 at the grid origin. Needed to reproduce
        // content-specific bugs on the real game (e.g. Bewear=3 in
        // pkmncc, babel's counterhit crash) -- char 0/0 in WonderfulWorld
        // never exercised the same moves/effects. Both peers run the same
        // pin with the same target, so the lockstep stays in step.
        {
            static int s_css_char = -2;
            static int s_css_color = 0;
            if (s_css_char == -2) {
                const char* v = std::getenv("FM2K_TEST_CSS_CHAR");
                if (v && v[0]) {
                    s_css_char = std::atoi(v);
                    const char* comma = std::strchr(v, ',');
                    s_css_color = comma ? std::atoi(comma + 1) : 0;
                } else {
                    s_css_char = -1;  // disabled
                }
            }
            if (s_css_char >= 0) {
                CssAutoConfirm_OnReplayMatchStart(
                    (uint8_t)s_css_char, (uint8_t)s_css_color,
                    (uint8_t)s_css_char, (uint8_t)s_css_color,
                    /*stage_id=*/0);
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Netplay: TEST_CSS_CHAR pin armed -- both players -> "
                    "char %d color %d (mirror)", s_css_char, s_css_color);
            }
        }
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
    // Harness autoplay for netplay CSS (split-brain fix, 2026-06-11):
    // feed OUR slot with the deterministic wander/dwell/confirm stream
    // so the CSS dance travels through the lockstep session and both
    // sims consume the identical (p1, p2) pair. Supersedes the
    // FM2K_TEST_AUTO_CSS pulse above when both envs are set. Mirrors
    // the FM2K_PARITY_AUTOPLAY_BATTLE feed in ProcessBattleInputPhase.
    // Production (env unset) keeps Input_CaptureLocal untouched.
    {
        static int s_np_autoplay_css = -1;
        if (s_np_autoplay_css < 0) {
            const char* v = std::getenv("FM2K_PARITY_AUTOPLAY");
            s_np_autoplay_css = (v && v[0] && v[0] != '0') ? 1 : 0;
        }
        if (s_np_autoplay_css == 1) {
            local_raw = Hook_ComputeAutoplayCssInput((int)g_player_index);
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

// Boot-to-battle (FM2K_BOOT_TO_BATTLE) test/dev path: the two peers skip the
// CSS rendezvous entirely and boot straight into a battle stage. Normally the
// battle-entry barrier is armed inside the CSS-synced path (g_battle_entry_armed
// = true after Netplay_StartCSSSession). With CSS skipped that never runs, so
// each peer's BATTLE_ENTERING packet is rejected by the other as "out-of-window"
// (armed=0) and both wedge forever at ">>> ENTERING BATTLE MODE - waiting for
// sync". Arm the barrier here so boot-to-battle netplay can sync. epoch stays 0
// (both peers send epoch 0, which the receive handler always accepts), and
// g_css_frame is 0 so both propose the same swap_frame. Production never calls
// this (it always goes through CSS); it's only invoked from the BTB signal site.
void Netplay_ArmBattleEntryBarrier() {
    if (g_battle_entry_armed) return;  // already armed (normal CSS path)
    g_battle_entry_armed    = true;
    g_entry_local_proposal  = 0;
    g_entry_remote_proposal = 0;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "Netplay: armed battle-entry barrier for boot-to-battle (no CSS rendezvous)");
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
    g_entry_local_proposal = local_proposal;
    ControlChannel_SendBattleEntering(g_battle_entry_swap_frame, g_entry_epoch, 0);
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
        // Record completion so the handler can keep ANSWERING the peer's
        // retries after we disarm (deafness fix — see the state block).
        g_entry_done_epoch = g_entry_epoch;
        g_entry_done_ms    = GetTickCount();
        // Completed-flag announce: saves the lagging peer one retry
        // round-trip, and tells an already-completed peer to go silent.
        ControlChannel_SendBattleEntering(g_battle_entry_swap_frame,
                                          g_entry_epoch, 0x1);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "BATTLE SYNC: both peers signaled (css_frame=%u, swap_frame=%u, epoch=%u) - swap CSS->battle now",
            g_css_frame, g_battle_entry_swap_frame, g_entry_epoch);
        // CSS divergence canary: in lockstep both sims flip to battle at
        // the same logical frame, so the two proposals should differ by
        // at most transit skew. A large gap means the CSS sims diverged
        // (different chars/colors are likely locked on each side) and
        // the upcoming battle is doomed to desync — make that loudly
        // visible at the moment it's decided, not minutes later.
        if (g_entry_local_proposal != 0 && g_entry_remote_proposal != 0) {
            const uint32_t a = g_entry_local_proposal;
            const uint32_t b = g_entry_remote_proposal;
            const uint32_t gap = (a > b) ? (a - b) : (b - a);
            if (gap > 300) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                    "BATTLE SYNC: CSS DIVERGENCE SUSPECTED — flip proposals "
                    "%u frames apart (local=%u remote=%u). Both sims should "
                    "leave CSS at the same lockstep frame.",
                    gap, a, b);
            }
        }
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
    static uint32_t wait_started = 0;
    static uint32_t last_wait_warn = 0;
    uint32_t now = GetTickCount();
    if (g_local_battle_entered && !g_remote_battle_entered) {
        if (now - last_send > 50) {
            ControlChannel_SendBattleEntering(g_battle_entry_swap_frame,
                                              g_entry_epoch, 0);
            last_send = now;
        }
        if (wait_started == 0) wait_started = now;
        if (now - wait_started > 5000 && now - last_wait_warn > 5000) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "BATTLE SYNC: waiting on remote for %us (swap=%u epoch=%u) — "
                "peer hasn't flipped to battle yet (its CSS sim may be "
                "behind or diverged)",
                (now - wait_started) / 1000, g_battle_entry_swap_frame,
                g_entry_epoch);
            last_wait_warn = now;
        }
    } else {
        wait_started = 0;
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
    ControlChannel_SendBattleEnd(g_battle_end_swap_frame, g_end_epoch, 0);
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
        // Completion record + announce — mirrors the entry barrier (the
        // handler answers post-disarm retries from a lagging peer).
        g_end_done_epoch = g_end_epoch;
        g_end_done_ms    = GetTickCount();
        ControlChannel_SendBattleEnd(g_battle_end_swap_frame, g_end_epoch, 0x1);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "BATTLE END SYNC: both peers signaled (battle_frame=%u, swap_frame=%u, epoch=%u) - swap battle->CSS now",
            g_netplay_frame, g_battle_end_swap_frame, g_end_epoch);
    }
    return g_battle_end_synced;
}

uint32_t Netplay_GetBattleEndSwapFrame() {
    return g_battle_end_swap_frame;
}

void Netplay_PollBattleEndSync() {
    ControlChannel_Poll();

    static uint32_t last_send = 0;
    static uint32_t wait_started = 0;
    static uint32_t last_wait_warn = 0;
    uint32_t now = GetTickCount();
    if (g_local_battle_end_signaled && !g_remote_battle_end_signaled) {
        if (now - last_send > 50) {
            ControlChannel_SendBattleEnd(g_battle_end_swap_frame,
                                         g_end_epoch, 0);
            last_send = now;
        }
        if (wait_started == 0) wait_started = now;
        if (now - wait_started > 5000 && now - last_wait_warn > 5000) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "BATTLE END SYNC: waiting on remote for %us (swap=%u epoch=%u)",
                (now - wait_started) / 1000, g_battle_end_swap_frame,
                g_end_epoch);
            last_wait_warn = now;
        }
    } else {
        wait_started = 0;
    }
}

void AddSubscribedSpectatorsToSession() {
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

void Netplay_EndCSSSession() {
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
NetplaySessionKind g_pending_swap_kind  = NetplaySessionKind::NONE;
uint32_t           g_pending_swap_frame = 0;

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

// =============================================================================
// BATTLE FRAME PROCESSING
// =============================================================================

// Frame pacing state (matches GekkoNet examples' handle_frame_time)
LARGE_INTEGER g_perf_freq = {};
LARGE_INTEGER g_frame_start = {};
bool g_frame_timer_initialized = false;

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

        // Battle-entry signal insurance (match-2 deadlock, 2026-06-11).
        // When the peer's BATTLE_ENTERING arrives during OUR armed window
        // BEFORE our own entry fires, IsBattleSynced latches the instant
        // we enter -- we swap to battle without ever running the
        // PollBattleSync wait loop, which is where both the
        // BATTLE_ENTERING resender and the CSS transport keepalive live.
        // Our single entry signal can then be the ONLY one the peer ever
        // gets; if it's lost (or lands outside their armed window), they
        // starve in CSS spamming proposals we drop as stale, while we sit
        // in an unsyncable battle session at bf=0. There is no entry ACK,
        // but gekko SessionStarted IS one: it can only fire once the peer
        // also swapped and synced. Resend until then, plus poll the
        // control channel so the peer's swap-side traffic keeps flowing.
        if (!g_session_ready && g_local_battle_entered) {
            static uint32_t s_last_entry_resend_ms = 0;
            const uint32_t now_ms = GetTickCount();
            if (now_ms - s_last_entry_resend_ms > 100) {
                // We're past the barrier (battle session exists), so this
                // is a completed-flag announce: peer latches remote=true
                // and won't echo back.
                ControlChannel_SendBattleEntering(g_battle_entry_swap_frame,
                                                  g_entry_done_epoch, 0x1);
                s_last_entry_resend_ms = now_ms;
            }
            ControlChannel_Poll();
        }
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
        // Only feed inputs into a STARTED session. The peer that enters
        // battle first ticks this loop while gekko is still syncing;
        // AddLocalInput stamps those adds with the pre-start frame
        // counter and the input buffer's sequential gate drops/misplaces
        // them -- net effect (cross-peer fork hunt, 2026-06-11): the two
        // peers permanently disagreed about THIS player's input timeline
        // by ~11 frames (= the leader's pre-sync tick count). States
        // matched through the round intro (inputs ignored there), then
        // forked at the first actionable frame (k=71, p1.script_idx 982
        // vs 986) -- the live "transient desync" class users hit.
        // GekkoNet's examples only add inputs in lockstep with a started
        // session.
        if (g_session_ready) {
            // ONE local input per call. This function is exactly one gekko
            // step (add -> update -> advance); the heavy-stage sim/render
            // decouple is orchestrated ONE LEVEL UP in the trampoline's
            // RunBattleTick, which calls this N times per rendered frame to
            // hold the sim at 100fps while display frames drop. Doing the
            // catch-up here (the old approach -- N adds before one
            // update_session) broke gekko's per-frame accounting and desynced
            // (RoHe/Aubeclisse f139); N full add->update->advance cycles do
            // not, because the sim clock is virtual (g_virtual_time_ms =
            // frame*10) so per-peer wall-clock only changes prediction depth,
            // which rollback absorbs. See RunBattleTick for the accumulator.
            gekko_add_local_input(g_session, g_player_index, &local_input);
        }
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
    // [RING] trace gate (task #34 + cross-peer fork hunt): cached
    // FM2K_RING_TRACE env check. Value 1 = legacy 40-frame window;
    // value N>1 = trace confirmed frames 0..N.
    static auto RingTraceMax = []() {
        static int c = -2;
        if (c == -2) {
            const char* v = std::getenv("FM2K_RING_TRACE");
            c = (v && v[0] && v[0] != '0') ? std::atoi(v) : 0;
            if (c == 1) c = 40;
        }
        return c;
    };
    static auto RingTraceOn = []() { return RingTraceMax() > 0; };
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
                if (RingTraceOn() && frame <= RingTraceMax()) {
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
                if (RingTraceOn() && frame <= RingTraceMax()) {
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
                    ++g_sim_step_count;   // sim-fps: one logic tick (netplay battle advance, incl. re-sims)
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

                if (RingTraceOn() && g_netplay_frame <= (uint32_t)RingTraceMax()) {
                    const uint32_t b = *(uint32_t*)0x447EE0;
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[RING] ADV  f=%u rb=%d ra=%d buf=%u h1[b]=%03X in1=%03X in2=%03X h2[b]=%03X",
                        g_netplay_frame, (int)update->data.adv.rolling_back,
                        (int)update->data.adv.running_ahead, b,
                        *(uint32_t*)(0x4280E0 + (b & 0x3FF) * 4),
                        g_p1_input, g_p2_input,
                        *(uint32_t*)(0x4290E0 + (b & 0x3FF) * 4));
                }

                // Confirmed-input ring: every non-runahead advance writes
                // its frame's inputs; rolling_back corrections OVERWRITE
                // the speculative first pass, so by the time the flush
                // below reaches a frame its values are final.
                if (!g_stress_mode && !update->data.adv.running_ahead) {
                    const uint32_t f = (uint32_t)update->data.adv.frame;
                    auto& slot = g_pending_confirm[f % PENDING_CONFIRM_RING];
                    slot.frame = f;
                    slot.p1 = Hook_ApplySOCD_Public(g_p1_input);
                    slot.p2 = Hook_ApplySOCD_Public(g_p2_input);
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
                    // Monotonic per-frame dedup. Under the heavy-stage
                    // sim/render decouple (RunBattleTick runs this N times per
                    // render) with asymmetric rollback, gekko can re-emit a
                    // confirmed advance for a frame already captured, padding
                    // the .pty and misaligning the cross-peer index diff
                    // (wanwan stall run: 850 vs 813 snapshots). A confirmed
                    // frame's state is FINAL, so record each frame number once.
                    // != (not >) so a fresh battle's frame 0 after a stale high
                    // value still records.
                    static uint32_t s_last_parity_frame = UINT32_MAX;
                    if (g_netplay_frame != s_last_parity_frame) {
                        ParityRecorder::Capture();
                        s_last_parity_frame = g_netplay_frame;
                    }
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
                    // FM2K_AUTO_TERMINATE_TOTAL: session-cumulative variant.
                    // g_netplay_frame resets per battle, so AT_FRAME cannot
                    // target anything past match 1 -- multi-match harness
                    // runs (spectator across MATCH_END -> CSS -> match 2)
                    // terminate on TOTAL confirmed battle frames instead.
                    static int s_terminate_total = -2;
                    static uint32_t s_total_confirmed = 0;
                    if (s_terminate_total == -2) {
                        const char* v = std::getenv("FM2K_AUTO_TERMINATE_TOTAL");
                        s_terminate_total = (v && *v) ? std::atoi(v) : -1;
                        if (s_terminate_total > 0) {
                            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                                "Harness: will TerminateProcess at %d TOTAL "
                                "confirmed battle frames (across matches)",
                                s_terminate_total);
                        }
                    }
                    if (!update->data.adv.rolling_back &&
                        !update->data.adv.running_ahead) {
                        ++s_total_confirmed;
                    }
                    const bool fire_at    = s_terminate_at > 0 &&
                        (int)g_netplay_frame >= s_terminate_at;
                    const bool fire_total = s_terminate_total > 0 &&
                        (int)s_total_confirmed >= s_terminate_total;
                    if ((fire_at || fire_total) &&
                        !update->data.adv.rolling_back &&
                        !update->data.adv.running_ahead) {
                        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "Harness: reached battle frame %u (total %u) — "
                            "flushing logs, writing .fm2krep, terminating cleanly.",
                            g_netplay_frame, s_total_confirmed);
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
                    // STRESS: record immediately -- no remotes, no
                    // predictions, every advance is confirmed truth.
                    //
                    // NETPLAY: do NOT record here. Under input prediction
                    // (window 16) this rb=0 advance may be SPECULATIVE --
                    // gekko advances with predicted remote inputs and the
                    // correction arrives later as a rolling_back re-advance
                    // (which this gate skips, and the monotonic gate blocks
                    // re-recording). Result: .fm2krep + live spectator
                    // stream recorded mispredicted inputs on any predicting
                    // peer -- on real internet BOTH peers predict, so real
                    // matches shipped corrupted replays/spec streams. The
                    // netplay path records via the pending ring + confirmed
                    // -horizon flush after the event loop instead.
                    if (g_stress_mode) {
                        SpectatorNode_OnFrameConfirmed(p1_for_spec, p2_for_spec);
                    }

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
        // Flush CONFIRMED inputs to the recorder/spectator stream. A frame
        // is safe once gekko has REAL inputs from all players for it --
        // predictions can no longer change it. Entries flush in strict
        // frame order; the pi.frame guard stops at frames not yet
        // advanced locally (confirmed horizon can lead our sim).
        if (!g_stress_mode && g_session && g_session_kind == SessionKind::BATTLE) {
            const int confirmed = gekko_confirmed_frame(g_session);
            while ((int)g_next_confirm_flush <= confirmed) {
                const PendingConfirmInput& pi =
                    g_pending_confirm[g_next_confirm_flush % PENDING_CONFIRM_RING];
                if (pi.frame != g_next_confirm_flush) break;
                SpectatorNode_OnFrameConfirmed(pi.p1, pi.p2);
                g_next_confirm_flush++;
            }
        }

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

    // [RELAY-RTT-DIAG] Under relay, surface whether gekko's RTT-match address
    // (the live g_remote_sockaddr stamp) still equals the string we registered
    // with gekko_add_actor (g_remote_addr). If they diverge, gekko's
    // NetworkHealth addr.Equals(actor) fails -> last_ping pins at 0 -> this peer
    // runs ahead -> one-sided rollback -> desync amplification (relay desync
    // cluster, 2026-06-13). Diagnostic only -- no behavior change. Formatting
    // mirrors g_remote_addr's (inet_ntop + "%s:%u") so the compare is byte-exact.
    if (::fm2k::nat::IsRelayMode()) {
        char live[INET_ADDRSTRLEN + 8] = "?";
        if (const sockaddr_in* rs = NetSocket_GetRemoteAddr()) {
            char ip_buf[INET_ADDRSTRLEN] = {};
            inet_ntop(AF_INET, (void*)&rs->sin_addr, ip_buf, sizeof(ip_buf));
            snprintf(live, sizeof(live), "%s:%u", ip_buf, ntohs(rs->sin_port));
        }
        const bool match = (strcmp(live, g_remote_addr) == 0);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "[BEAT-RELAY] gekko_ping=%ums actor='%s' live_stamp='%s' addr_match=%s%s",
            stats.last_ping, g_remote_addr, live, match ? "yes" : "NO",
            (!match || stats.last_ping == 0)
                ? "  <-- RTT-match BROKEN (ping pins at 0 -> one-sided rollback)" : "");
    }

    // Reset window so next emit describes only the next ~10s.
    g_beat_window_rb_sum   = 0;
    g_beat_window_rb_max   = 0;
    g_beat_window_rb_count = 0;
}
