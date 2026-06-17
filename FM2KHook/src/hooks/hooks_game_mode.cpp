// hooks_game_mode.cpp -- game-mode detection (IsCSSMode/IsBattleMode) + CheckGameModeTransition. Split from hooks.cpp (pure move).
#include "hooks.h"
#include "round_events.h"     // C3.5 — vs_round_function detour install
#include "css_autoconfirm.h"  // CSS lock-and-confirm for offline replay playback
#include "css_fastsound.h"    // FM2K_FPK_CSS_FASTSOUND: lazy DSound buffers (CSS dip fix)
#include "per_game_patches.h" // damage multiplier MinHook + team-size override
#include "render_simd.h"      // FM2K_BLIT_SIMD: blit + case -10 blur reimplementation
#include "globals.h"

#include <cstdlib>
#include <cstring>
#include <vector>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <list>
#include <thread>
#include <condition_variable>
#include <atomic>
#include "netplay.h"
#include "control_channel.h"
#include "../netplay/game_hash.h"
#include "imgui_overlay.h"
#include "shared_mem.h"
#include "savestate.h"  // CHAR_SLOT_BASE, CHAR_SLOT_SIZE (corrected by Wave C audit)
#include "../core/main_loop_trampoline.h"  // TrampolineMainLoop — owns the outer loop
#include "../audio/sound_rollback.h"        // Mike Z desired/actual sound layer
#include "../netplay/spectator_node.h"      // spectator playback queue accessors
#include "../ui/input_binder.h"             // FM2KInputBinder::Sample_Win32 + Bindings
#include "../ui/screenshot.h"               // FM2KCapture::SaveScreenshot for the auto-banner pipeline
#include "../ui/fc_hud.h"                   // IsChatInputActive — gate local input during typing
#include "../vfs/fpk_reader.h"              // FM2K_FPK_VFS: inflate a slim .fpk -> original asset bytes
#include <MinHook.h>
#include <SDL3/SDL_log.h>
#include <windows.h>
#include <mmsystem.h>
#include <cstdio>
#include <cfloat>   // _controlfp_s, _PC_53, _MCW_PC, _RC_NEAR, _MCW_RC, _MCW_EM
#include <cstdint>
#include <string>

// Pin the x87 FPU control word to a fixed precision + rounding mode on the
// game thread. IDA audit found the binary never calls _controlfp / fldcw and
// DirectDraw's SetCooperativeLevel is invoked without DDSCL_FPUPRESERVE, so
// the default precision is whatever DirectDraw/driver/OS happens to leave.
// That varies across machines and is almost certainly why peer simulations
// diverge on movement (velocity, collision, normalization all use floats).
// Call this before every gameplay tick to override any mid-frame changes.
// MXCSR bit layout (SSE control/status register):
//   bit 15 FZ (flush-to-zero)
//   bits 13-14 RC (round control): 00 nearest, 01 down, 10 up, 11 truncate
//   bits 7-12 exception masks (we set all = masked)
//   bit 6 DAZ (denormals-are-zero)
//   bits 0-5 exception flags (sticky, we clear)
// We want: round-to-nearest-even, all exceptions masked, no FZ/DAZ, flags clear.
// Value 0x1F80 is the x86 default but we pin it explicitly to ensure both
#include "hooks_internal.h"

// ============================================================================

static uint32_t g_last_game_mode = 0;

// Engine-aware phase detection.
// FM2K encodes phase via g_game_mode magic numbers (2000=CSS, 3000+=Battle).
// FM95 keeps g_game_mode near 0/1/10 — phase lives inside per-object slots
// in the 256-entry pool (type==19 sub_state ∈ [0x28,0xC9] = CSS, type==16
// sub_state ∈ [10,31] = Battle). Walk the pool once per call; could be
// frame-cached if it shows up hot in profiles.
//
// The `mode` argument is preserved so existing call sites keep compiling
// without change. On FM2K it's still load-bearing; on FM95 it's ignored
// and we read the object pool directly.
namespace {
    enum class FM95Phase { Boot, Title, CSS, PostCSS, Battle, MatchEnd, Other };

    inline FM95Phase Fm95ClassifyPhase() {
        const uint8_t* pool = (const uint8_t*)FM2K::ADDR_OBJECT_POOL;
        for (size_t i = 0; i < FM2K::OBJECT_POOL_COUNT; ++i) {
            const uint8_t* slot = pool + i * FM2K::OBJECT_POOL_STRIDE;
            uint32_t type = *reinterpret_cast<const uint32_t*>(slot);
            if (type < 2) continue;            // 0=empty, 1=disabled
            uint32_t sub  = *reinterpret_cast<const uint32_t*>(slot + 108);
            if (type == 19) {                  // title_screen_state_machine
                if (sub >= 0x28 && sub <= 0xC9) return FM95Phase::CSS;
                return FM95Phase::Title;
            }
            if (type == 16) {                  // vs_round_function
                if (sub >= 10 && sub <= 31)    return FM95Phase::Battle;
                return FM95Phase::MatchEnd;
            }
            if (type == 21) return FM95Phase::PostCSS;
            if (type == 30 || type == 15) return FM95Phase::Boot;
        }
        return FM95Phase::Other;
    }
}

// Exported (non-static) so main_loop_trampoline.cpp's ClassifyPhase can use
// the same engine-aware logic. Forward-declared in hooks.h.
bool IsCSSMode(uint32_t mode) {
    if constexpr (FM2K::kIsFM2K) {
        return mode == 2000;
    } else {
        (void)mode;
        return Fm95ClassifyPhase() == FM95Phase::CSS;
    }
}

bool IsBattleMode(uint32_t mode) {
    if constexpr (FM2K::kIsFM2K) {
        return mode >= 3000 && mode < 4000;
    } else {
        (void)mode;
        return Fm95ClassifyPhase() == FM95Phase::Battle;
    }
}

// Battle sync state - ensures both clients start GekkoNet together.
// Exposed non-static so the trampoline (main_loop_trampoline.cpp) can see it;
// the trampoline replaces main_game_loop wholesale and needs to drive the
// battle-entry handshake.
bool g_battle_entry_signaled_pub = false;

// Called every frame to check for game mode transitions
// Public shim so the trampoline (main_loop_trampoline.cpp) can invoke the
// same transition detector the hooks use.
extern "C" void Hook_CheckGameModeTransition_Public();
void CheckGameModeTransition();
extern "C" void Hook_CheckGameModeTransition_Public() { CheckGameModeTransition(); }


void CheckGameModeTransition() {
    uint32_t current_mode = *(uint32_t*)FM2K::ADDR_GAME_MODE;

    if (current_mode != g_last_game_mode) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "Hooks: game_mode changed: %u -> %u", g_last_game_mode, current_mode);

        // Publish session_kind to SharedMem so the launcher can forward
        // it to the hub. Used by the spectator-join /F decision: when
        // someone requests to spectate us, the hub returns our current
        // session_kind in spectate_grant so their launcher knows
        // whether to set FM2K_BOOT_TO_BATTLE=1 (we're in battle) or
        // not (we're in CSS — spec needs natural CSS init for the
        // CSS-state snapshot to apply cleanly at mode==2000).
        uint8_t kind = 0;  // menu / unknown
        if (current_mode == 2000u) kind = 1;            // CSS
        else if (current_mode >= 3000u && current_mode < 4000u)
            kind = 2;                                    // battle
        SharedMem_PublishSessionKind(kind);

        // Auto-capture banner pipeline. Drives one screenshot at each
        // mode-boundary the launcher's capture-runner cares about,
        // then writes a "DONE" sentinel file the launcher polls
        // before terminating the game. No-op when FM2K_AUTO_CAPTURE
        // wasn't set (FM2KCapture::IsActive() short-circuits).
        if (FM2KCapture::IsActive()) {
            static bool s_captured_title = false;
            static bool s_captured_css   = false;
            static bool s_captured_battle = false;
            if (!s_captured_title && current_mode == 1000) {
                FM2KCapture::SaveScreenshot("title.png");
                s_captured_title = true;
            }
            if (!s_captured_css && current_mode == 2000) {
                FM2KCapture::SaveScreenshot("css_initial.png");
                s_captured_css = true;
            }
            if (!s_captured_battle && current_mode >= 3000
                && current_mode < 4000) {
                FM2KCapture::SaveScreenshot("battle.png");
                s_captured_battle = true;
                // All three core captures done — touch a sentinel so
                // the launcher's capture-runner sees "ready to kill".
                // Empty zero-byte file; the launcher polls for its
                // existence on a 250 ms cadence.
                FILE* f = std::fopen(
                    (std::string(std::getenv("FM2K_CAPTURE_DIR") ?
                                 std::getenv("FM2K_CAPTURE_DIR") : ".")
                     + "/.capture_done").c_str(), "wb");
                if (f) std::fclose(f);
            }
        }

        // Whenever the game crosses any CSS↔battle boundary, kick off a
        // 300-frame (3 second @ 100 Hz) state dump so we can diff
        // working games (WonderfulWorld) vs broken ones (SFZ, StudioS
        // Fighters) at battle entry. Reset the per-window counter.
        // BATTLE-DIAG window: gated on FM2K_BATTLE_DIAG=1. Off by default
        // because each open-window dumps 300 frames of [BD ##] state into
        // the log per CSS↔battle transition — useful for diffing broken
        // FM2K variants at battle entry but pure noise during normal play.
        // Cached once at first call.
        static int s_battle_diag_enabled = -1;
        if (s_battle_diag_enabled < 0) {
            const char* v = std::getenv("FM2K_BATTLE_DIAG");
            s_battle_diag_enabled = (v && v[0] == '1' && v[1] == '\0') ? 1 : 0;
        }
        bool boundary = s_battle_diag_enabled &&
            ((IsCSSMode(g_last_game_mode)    && IsBattleMode(current_mode)) ||
             (IsBattleMode(g_last_game_mode) && IsCSSMode(current_mode)));
        if (boundary) {
            g_battle_diag_frames_remaining = 300;
            g_battle_diag_frame_idx = 0;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                ">>> BATTLE-DIAG window OPEN (300 frames) <<<");
        }

        // Spectator: state init no longer mirrors via local game_mode flips.
        // Host emits PIN_RNG / RESET_INPUT_STATE / SOUND_INIT ops as part of
        // the SessionEvent stream (see Netplay_StartBattle, Netplay_ProcessCSS,
        // CheckFullyConnected); the spectator applies them in
        // SpectatorNode_PopFrameInputs's head-drain at the moment its local
        // sim is about to consume the corresponding INPUT — same logical
        // frame the host's pin happened. Eliminates the off-by-N race
        // between host-side write and spectator-side game_mode flip.
        //
        // Just bail before any host-only player-state-machine work runs,
        // and let the BATTLE-DIAG window + capture pipeline above still
        // observe the boundary for diagnostics.
        if (g_spectator_mode) {
            g_last_game_mode = current_mode;
            return;
        }

        bool was_battle = IsBattleMode(g_last_game_mode);
        bool is_battle = IsBattleMode(current_mode);

        if (!was_battle && is_battle) {
            // ENTERING BATTLE MODE - Signal entry, but DON'T start GekkoNet yet
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                ">>> ENTERING BATTLE MODE - Signaling remote, waiting for sync");

            // Drain the trailing CSS-frame capture before the battle session
            // gates capture_and_return out. The pair sitting in g_capture_p[]
            // is the LAST CSS frame — the one whose confirm flipped game_mode.
            // Without this flush, spectator never sees that frame, never
            // flips its own game_mode, and desyncs at battle entry.
            extern void Hook_FlushPendingCapture();
            Hook_FlushPendingCapture();

            if (!g_offline_mode && Netplay_IsConnected()) {
                // Boot-to-battle (test/dev) skips the CSS rendezvous that
                // normally arms the battle-entry barrier. Arm it here so the
                // two direct-to-battle peers accept each other's signal
                // instead of deadlocking at "waiting for sync". No-op (and
                // never reached as an arm) on the production CSS path.
                static const bool s_btb = []{
                    const char* e = std::getenv("FM2K_BOOT_TO_BATTLE");
                    return e && e[0] == '1' && e[1] == '\0';
                }();
                if (s_btb) {
                    extern void Netplay_ArmBattleEntryBarrier();
                    Netplay_ArmBattleEntryBarrier();
                }
                Netplay_SignalBattleEntry();
                g_battle_entry_signaled = true;
                // NOTE: GekkoNet will be started in Hook_UpdateGameState
                // after both clients have entered battle mode
            }
        } else if (was_battle && !is_battle) {
            // LEAVING BATTLE MODE - Signal exit; trampoline tears down the
            // GekkoNet session at the agreed swap_frame so both peers
            // (and any spectators) destroy in lockstep. Synchronous
            // teardown here would leave a few frames of mismatched session
            // state on the wire and risk spectator desync at the boundary.
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "<<< LEAVING BATTLE MODE - Signaling swap_frame exit");
            if (!g_offline_mode && Netplay_IsActive()) {
                Netplay_SignalBattleEnd();
            } else if (Netplay_IsActive()) {
                // Offline / stress paths still tear down synchronously —
                // there's no remote peer to negotiate with.
                Netplay_EndBattle();
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "GekkoNet session stopped (offline path)");
            }
            g_battle_entry_signaled = false;
        }

        g_last_game_mode = current_mode;
    }
}

