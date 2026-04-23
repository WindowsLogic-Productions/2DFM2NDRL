// Simplified hooks - detect battle mode transitions, delegate to netplay
// Sync barrier: block game until both clients connected (CCCaster-style)
#include "hooks.h"
#include "globals.h"
#include "netplay.h"
#include "control_channel.h"
#include "imgui_overlay.h"
#include "shared_mem.h"
#include "savestate.h"  // CHAR_SLOT_BASE, CHAR_SLOT_SIZE (corrected by Wave C audit)
#include "../core/main_loop_trampoline.h"  // TrampolineMainLoop — owns the outer loop
#include "../audio/sound_rollback.h"        // Mike Z desired/actual sound layer
#include <MinHook.h>
#include <SDL3/SDL_log.h>
#include <windows.h>
#include <mmsystem.h>
#include <cstdio>
#include <cfloat>   // _controlfp_s, _PC_53, _MCW_PC, _RC_NEAR, _MCW_RC, _MCW_EM
#include <cstdint>

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
// peers use the same value regardless of prior state.
static inline void SetMXCSR(unsigned int v) {
    __asm__ volatile("ldmxcsr %0" : : "m"(v));
}

static inline void PinFPUControlWord() {
    unsigned int cur = 0;
    // x87: 53-bit precision, round-to-nearest-even, all exceptions masked.
    _controlfp_s(&cur, _PC_53 | _RC_NEAR | _MCW_EM,
                       _MCW_PC | _MCW_RC | _MCW_EM);
    // SSE: also pin MXCSR. FM2K's hit-detection and physics likely emit SSE
    // float ops under -mfpmath or vectorizer, and MXCSR rounding mode is
    // independent of the x87 control word. Both peers must agree.
    SetMXCSR(0x1F80u);
}

// Deterministic timeGetTime: during an active GekkoNet battle session the
// return value is derived from the authoritative advance count, NOT wall
// clock. main_game_loop writes timeGetTime() into g_last_frame_time @
// 0x447DD4 every iteration, which lives inside our saved "afterimage_pool"
// region. If forward-sim wrote wall-clock T1 and replay-sim wrote T2 at
// the same frame, the saved afterimage_pool diverges by that timestamp
// byte — this is the exact "REPLAY DIFF AfterimagePool +0x4A4" signature
// we caught at f=9 in the stress test.
//
// Virtual clock is advanced by 10 ms EACH TIME an AdvanceEvent completes
// (see netplay.cpp). Within a single main_game_loop iteration the game
// polls timeGetTime() multiple times — we return the same value on every
// call until the next advance. Forward-sim and replay-sim both consume
// the same advance sequence, so both produce identical virtual timestamps
// at the same logical frame.
//
// Outside an active session we pass through — menus/CSS rely on real wall
// clock for music/animation pacing, and determinism doesn't matter there.
extern bool Netplay_IsActive();
using timeGetTime_t = DWORD(WINAPI*)();
static timeGetTime_t original_timeGetTime = nullptr;
uint32_t g_virtual_time_ms = 0;  // bumped by 10 per AdvanceEvent in netplay.cpp

static DWORD WINAPI Hook_timeGetTime() {
    if (Netplay_IsActive()) {
        return g_virtual_time_ms;
    }
    return original_timeGetTime ? original_timeGetTime() : 0;
}

// ============================================================================
// GAME MODE DETECTION
// ============================================================================

static uint32_t g_last_game_mode = 0;

static bool IsCSSMode(uint32_t mode) {
    return mode == 2000;
}

static bool IsBattleMode(uint32_t mode) {
    return mode >= 3000 && mode < 4000;
}

// Battle sync state - ensures both clients start GekkoNet together.
// Exposed non-static so the trampoline (main_loop_trampoline.cpp) can see it;
// the trampoline replaces main_game_loop wholesale and needs to drive the
// battle-entry handshake.
bool g_battle_entry_signaled_pub = false;
#define g_battle_entry_signaled g_battle_entry_signaled_pub

// Called every frame to check for game mode transitions
// Public shim so the trampoline (main_loop_trampoline.cpp) can invoke the
// same transition detector the hooks use.
extern "C" void Hook_CheckGameModeTransition_Public();
static void CheckGameModeTransition();
extern "C" void Hook_CheckGameModeTransition_Public() { CheckGameModeTransition(); }

static void CheckGameModeTransition() {
    uint32_t current_mode = *(uint32_t*)FM2K::ADDR_GAME_MODE;

    if (current_mode != g_last_game_mode) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "Hooks: game_mode changed: %u -> %u", g_last_game_mode, current_mode);

        bool was_battle = IsBattleMode(g_last_game_mode);
        bool is_battle = IsBattleMode(current_mode);

        if (!was_battle && is_battle) {
            // ENTERING BATTLE MODE - Signal entry, but DON'T start GekkoNet yet
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                ">>> ENTERING BATTLE MODE - Signaling remote, waiting for sync");
            if (!g_offline_mode && Netplay_IsConnected()) {
                Netplay_SignalBattleEntry();
                g_battle_entry_signaled = true;
                // NOTE: GekkoNet will be started in Hook_UpdateGameState
                // after both clients have entered battle mode
            }
        } else if (was_battle && !is_battle) {
            // LEAVING BATTLE MODE - Stop GekkoNet
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "<<< LEAVING BATTLE MODE - Stopping GekkoNet session");
            if (Netplay_IsActive()) {
                Netplay_EndBattle();
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "GekkoNet session stopped, back to control channel");
            }
            g_battle_entry_signaled = false;
        }

        g_last_game_mode = current_mode;
    }
}

// ============================================================================
// HOOK IMPLEMENTATIONS
// ============================================================================

// Hook: GetPlayerInput
// CSS: return synced input from control channel
// Battle: return synchronized input from GekkoNet with facing adjustment
int __cdecl Hook_GetPlayerInput(int player_id, int input_type) {
    uint32_t game_mode = *(uint32_t*)FM2K::ADDR_GAME_MODE;

    // Battle mode with GekkoNet active - return synced input with facing fix
    if (Netplay_IsActive()) {
        uint16_t input = Netplay_GetInput(player_id);

        // Apply facing direction swap (same logic as original get_player_input).
        // During battle (3000-3999), if character is active and not in special
        // state, left/right are swapped based on facing direction.
        //
        // CRITICAL: these are OFFSETS inside the character slot, NOT absolute
        // addresses. Hard-coding absolute addresses broke when we corrected
        // CHAR_SLOT_BASE from 0x4D1D80 to 0x4D1D90 — the hook was reading
        // from 16 bytes into the wrong memory, decisions were garbage, and
        // the two peers could pick different facing-swap values from
        // non-deterministic residue. This is almost certainly the "HP
        // differs by 2 after a hit" signature we've been chasing.
        //
        // Offsets are relative to the CORRECTED base CHAR_SLOT_BASE=0x4D1D90.
        // First attempt computed these against the old 0x4D1D80 base, which was
        // 16 bytes too low for the new base — that made facing-swap read the
        // wrong bytes and the symptom was "left/right flip when you switch
        // sides". Absolute addresses of the fields are unchanged:
        //   0x4DFCD1 - 0x4D1D90 = 0xDF41   (char_active)
        //   0x4D9A36 - 0x4D1D90 = 0x7CA6   (char_state_flags)
        constexpr size_t CHAR_ACTIVE_FLAG_OFFSET = 0xDF41;
        constexpr size_t CHAR_STATE_FLAGS_OFFSET = 0x7CA6;

        bool facing_reversed = true;  // Default: no swap (normal directions)
        if (game_mode >= 3000 && game_mode < 4000) {
            uintptr_t slot_base = CHAR_SLOT_BASE + (size_t)input_type * CHAR_SLOT_SIZE;
            uint8_t char_active = *(uint8_t*)(slot_base + CHAR_ACTIVE_FLAG_OFFSET);
            if (char_active != 0) {
                uint8_t char_flags = *(uint8_t*)(slot_base + CHAR_STATE_FLAGS_OFFSET);
                if ((char_flags & 8) == 0) {
                    facing_reversed = false;  // Character active, facing applies
                }
            }
        }

        if (!facing_reversed) {
            // Swap left (bit 0 = 0x001) and right (bit 1 = 0x002)
            uint16_t left_bit = (input & 0x001);
            uint16_t right_bit = (input & 0x002);
            input = (input & ~0x003) | (left_bit << 1) | (right_bit >> 1);
        }

        // Log only the first 4 calls (initial handshake verification). After
        // that stay silent — Hook_GetPlayerInput fires 2x per sim tick, and
        // during stress-mode rollback replay that's thousands of calls per
        // second. Per-100 throttling was still showing up on screen.
        static uint32_t battle_log_count = 0;
        if (battle_log_count < 4) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[BATTLE INPUT #%u] player=%d type=%d -> 0x%03X (facing=%s)",
                battle_log_count, player_id, input_type, input,
                facing_reversed ? "normal" : "swapped");
        }
        battle_log_count++;

        return input;
    }

    // CSS mode with connection - return CSS input from control channel
    if (IsCSSMode(game_mode) && Netplay_IsConnected()) {
        uint16_t input = Netplay_GetCSSInput(player_id);
        return input;
    }

    // Offline or menu: use original function
    return original_get_player_input ? original_get_player_input(player_id, input_type) : 0;
}

// Hook: UpdateGameState
// Main control point - check transitions, process netplay
int __cdecl Hook_UpdateGameState() {
    uint32_t game_mode = *(uint32_t*)FM2K::ADDR_GAME_MODE;

    // Offline mode - just pass through
    if (g_offline_mode) {
        g_frame_counter++;
        return original_update_game ? original_update_game() : 0;
    }

    // ========================================================================
    // STRESS-TEST MODE (FM2K_STRESS_MODE=1) - single-instance determinism check
    // GekkoStressSession artificially rolls back every check_distance frames.
    // No network, no sync barriers. Menu/CSS run pass-through; battle mode
    // starts a GekkoStressSession and drives sim via the Save/Load/Advance
    // event loop (same path as online, minus the network).
    // Any desync fired here = local determinism bug. Pure repro.
    // ========================================================================
    if (g_stress_mode) {
        if (IsBattleMode(game_mode)) {
            if (!Netplay_IsActive()) {
                if (!Netplay_StartStressBattle()) {
                    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                        "Stress: Failed to start GekkoStressSession, falling through");
                    g_frame_counter++;
                    return original_update_game ? original_update_game() : 0;
                }
            }
            if (!Netplay_ProcessBattleInputPhase()) {
                return 0;
            }
            g_frame_counter++;
            return 0;
        }
        // Menu / CSS / results: run game normally
        g_frame_counter++;
        return original_update_game ? original_update_game() : 0;
    }

    // ========================================================================
    // SYNC BARRIER - Block game until both clients are connected
    // CCCaster-style: return 0 to freeze game at menu until connection
    // ========================================================================
    if (!Netplay_IsConnected()) {
        // Keep trying to connect
        static uint32_t last_poll = 0;
        static uint32_t block_count = 0;
        uint32_t now = GetTickCount();

        // Poll control channel to process HELLO/HELLO_ACK
        ControlChannel_Poll();

        // Send HELLO periodically until connected
        if (now - last_poll > 500) {
            ControlChannel_SendHello(static_cast<uint8_t>(g_player_index), 0);
            last_poll = now;

            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "SYNC BARRIER: Blocking game (P%d, mode=%u, blocked %u times)",
                g_player_index + 1, game_mode, block_count);
        }

        block_count++;

        // BLOCK GAME - return 0 to prevent any game state updates
        // This keeps both clients at the same starting point
        return 0;
    }

    // Log when we first pass the barrier
    static bool barrier_passed = false;
    if (!barrier_passed) {
        barrier_passed = true;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "SYNC BARRIER PASSED: P%d connected, game_mode=%u, frame=%u",
            g_player_index + 1, game_mode, g_frame_counter);
    }

    // Check for game mode transitions (CSS <-> Battle)
    CheckGameModeTransition();

    // ========================================================================
    // CSS MODE - Delay-based with stall when remote is behind
    // Game loop calls: ProcessGameInputs -> UpdateGameState -> InputHistory
    // We must block ALL of them during stalls to prevent edge detection desync
    // ========================================================================
    if (IsCSSMode(game_mode)) {
        // ProcessCSS handles everything: poll, stall, capture, send batch.
        // Returns false if stalling (waiting for remote input + resending ours).
        if (!Netplay_ProcessCSS()) {
            return 0;  // Stall - don't update game state
        }

        g_frame_counter++;
        return original_update_game ? original_update_game() : 0;
    }

    // ========================================================================
    // BATTLE MODE - Sync barrier then GekkoNet rollback
    // ========================================================================
    if (IsBattleMode(game_mode)) {
        // ----------------------------------------------------------------
        // BATTLE SYNC BARRIER - Block until both clients enter battle mode
        // This ensures both start GekkoNet at the same frame
        // ----------------------------------------------------------------
        if (g_battle_entry_signaled && !Netplay_IsActive()) {
            // Poll for BATTLE_ENTERING from remote
            Netplay_PollBattleSync();

            if (!Netplay_IsBattleSynced()) {
                // Still waiting for remote - block game
                static uint32_t last_log = 0;
                uint32_t now = GetTickCount();
                if (now - last_log > 500) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "BATTLE SYNC BARRIER: Waiting for remote to enter battle mode...");
                    last_log = now;
                }
                return 0;  // Block game until synced
            }

            // Both clients synced - NOW start GekkoNet
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "BATTLE SYNC BARRIER PASSED: Starting GekkoNet session");
            if (Netplay_StartBattle()) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "GekkoNet session started for battle");
            } else {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                    "Failed to start GekkoNet session!");
            }
        }

        // ----------------------------------------------------------------
        // GekkoNet active - process rollback
        // ----------------------------------------------------------------
        if (Netplay_IsActive()) {
            // Process GekkoNet frame - runs full game ticks inside each AdvanceEvent
            // (process_game_inputs + update_game), matching GekkoNet examples.
            // We do NOT call original_update_game here - it already ran.
            if (!Netplay_ProcessBattleInputPhase()) {
                // No advance event yet - keep polling
                return 0;
            }

            // GekkoNet already ran the tick(s). Just update our frame counter.
            g_frame_counter++;
            return 0;  // Skip game loop's own update - already done
        }
    }

    // ========================================================================
    // OTHER MODES (menu, results, etc.)
    // ========================================================================
    Netplay_ProcessMenu();

    g_frame_counter++;
    return original_update_game ? original_update_game() : 0;
}

// Find our game window
static HWND g_cached_window = NULL;

static BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam) {
    DWORD pid;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId()) {
        char class_name[64];
        if (GetClassNameA(hwnd, class_name, sizeof(class_name))) {
            if (strcmp(class_name, "KGT2KGAME") == 0) {
                *(HWND*)lParam = hwnd;
                return FALSE;
            }
        }
    }
    return TRUE;
}

static HWND GetOurGameWindow() {
    if (g_cached_window && IsWindow(g_cached_window)) {
        return g_cached_window;
    }
    g_cached_window = NULL;
    EnumWindows(EnumWindowsProc, (LPARAM)&g_cached_window);
    return g_cached_window;
}

// FPS tracking
static DWORD g_last_fps_time = 0;
static int g_fps_frame_count = 0;
static int g_current_fps = 0;

// Hook: RenderGame
// Set in the GekkoNet AdvanceEvent handler (netplay.cpp). Each advance
// produces exactly one new simulation tick; this flag says "that tick is
// unrendered". Cleared inside Hook_RenderGame after original_render_game()
// has drawn it. Any extra Hook_RenderGame invocations between advances skip
// the real render entirely, so render count cannot outpace sim count on
// either peer — both peers render exactly as many frames as GekkoNet has
// advanced. Without this gate, render mutates object-pool animation counters
// on wall-clock cadence, producing asymmetric state that feeds back into
// the next sim tick's RNG draws.
bool g_frame_pending_render = false;

// Public — called by the trampoline's render step so FPS + title bar stats
// keep updating even though Hook_RenderGame is bypassed in battle mode.
extern "C" void Hook_RenderDiagnostics_Tick();
extern "C" void Hook_RenderDiagnostics_Tick() {
    CheckOverlayHotkey();

    // Track FPS
    g_fps_frame_count++;
    DWORD now = GetTickCount();
    if (now - g_last_fps_time >= 1000) {
        g_current_fps = g_fps_frame_count;
        g_fps_frame_count = 0;
        g_last_fps_time = now;
    }

    // Update window title with BBBR-style stats (throttled to 500ms)
    static DWORD last_title_update = 0;
    DWORD title_now = GetTickCount();
    if (title_now - last_title_update >= 500) {
        last_title_update = title_now;
        HWND game_window = GetOurGameWindow();
        if (game_window) {
            char title[256];
            const char* role = (g_player_index == 0) ? "HOST" : "CLIENT";
            bool active = Netplay_IsActive();
            bool connected = Netplay_IsConnected();

            if (active) {
                GekkoNetworkStats stats = Netplay_GetNetworkStats();
                float ahead = Netplay_GetFramesAhead();
                int delay = Netplay_GetLocalDelay();
                uint32_t desyncs = Netplay_GetDesyncCount();
                uint32_t rollbacks = Netplay_GetRollbackCount();
                const char* tag = g_stress_mode ? "STRESS" : "Battle";

                if (desyncs > 0) {
                    snprintf(title, sizeof(title),
                        "FM2K [%s] %s | FPS:%d | P:%ums A:%.1fms | D:%d FA:%.1f | RB:%u | DESYNC x%u",
                        role, tag, g_current_fps,
                        stats.last_ping, stats.avg_ping,
                        delay, ahead, rollbacks, desyncs);
                } else {
                    snprintf(title, sizeof(title),
                        "FM2K [%s] %s | FPS:%d | P:%ums A:%.1fms | D:%d FA:%.1f | RB:%u",
                        role, tag, g_current_fps,
                        stats.last_ping, stats.avg_ping,
                        delay, ahead, rollbacks);
                }
            } else if (connected) {
                uint32_t ping = Netplay_GetPingMs();
                snprintf(title, sizeof(title),
                    "FM2K [%s] CSS | FPS:%d | RTT:%ums",
                    role, g_current_fps, ping);
            } else if (!g_offline_mode) {
                snprintf(title, sizeof(title),
                    "FM2K [%s] Connecting... | FPS:%d",
                    role, g_current_fps);
            } else {
                snprintf(title, sizeof(title),
                    "FM2K [Offline] %d FPS", g_current_fps);
            }

            SetWindowTextA(game_window, title);
        }
    }
}

// Mike Z sound rollback: intercept the SFX branch of FM2K's script sound
// dispatcher. During battle, instead of playing immediately we record the
// requested play into `desired[channel]`. Once per displayed frame (after
// the advance batch completes) SoundRollback::SyncAfterAdvance reconciles
// desired ↔ actual and issues real DSound stops/plays with the rollback-
// window filter that prevents erased/re-triggered sounds from clipping.
//
// Script item layout (42 bytes, from DispatchScriptSoundCommand decomp):
//   +36  void*  SoundBufferArray ptr       (SFX case)
//   +40  uint8  cmd byte (low nibble: 0=stop 1=SFX 2=MIDI 3=CD; bit 0x10=volume flag)
//   +41  uint8  CD track number             (CD case)
//
// MIDI and CD paths (music) pass through unchanged — music-restart on
// rollback is a v2 concern.
typedef int(__cdecl* DispatchScriptSoundFunc)(int);
static DispatchScriptSoundFunc original_dispatch_script_sound = nullptr;

int __cdecl Hook_DispatchScriptSoundCommand(int script_item) {
    if (!Netplay_IsActive() || script_item == 0) {
        return original_dispatch_script_sound(script_item);
    }

    uint8_t cmd = *reinterpret_cast<uint8_t*>(script_item + 40);
    if ((cmd & 0xF) != 1) {
        // Not SFX — MIDI (case 2), CD audio (case 3), or full stop (case 0).
        // These paths use MCI (mciSendCommandA), which is heavy/stateful and
        // doesn't survive the rapid-fire repeats that rollback replays cause.
        // In stress mode every displayed frame replays ~10 sim frames, so if
        // a music trigger is anywhere in that window it fires ~10 times per
        // displayed frame (1 forward + 9 replay). Even after we suppress the
        // replay branch, the FORWARD pass still re-fires every time the save
        // ring scrolls past that frame — music cuts in and out.
        //
        // Apply a "dedup by payload" filter: a (cmd, buf_ptr_or_track)
        // dispatch identical to the previous non-replay dispatch is treated
        // as a no-op. Any change — new track, stop-then-same-track, fanfare
        // switch, CD ↔ MIDI — updates the stored key and fires normally, so
        // mid-match music transitions still work. Only the GekkoNet save-ring
        // scroll's identical re-trigger gets filtered.
        // Also skip during replay so the forward-first dispatch wins.
        if (g_is_rolling_back) {
            return 0;
        }
        // +36 = buffer_array ptr (MIDI/CD paths don't use it but reading is
        // harmless since the script item is always 42 bytes of valid memory)
        // +41 = CD track number (case 3 only)
        uint32_t payload = *reinterpret_cast<uint32_t*>(script_item + 36)
                         ^ *reinterpret_cast<uint8_t*> (script_item + 41);
        if (SoundRollback::IsRedundantMusicDispatch(cmd, payload)) {
            return 0;  // identical music command as last time — leave MCI alone
        }
        return original_dispatch_script_sound(script_item);
    }

    void* arr = *reinterpret_cast<void**>(script_item + 36);
    if (!arr || !SoundRollback::RecordDesired(arr, script_item, Netplay_GetFrame())) {
        // Unknown / null channel — not in g_sound_channel_table. Fall through
        // to the original dispatcher so the sound still plays (without
        // rollback tracking). The vast majority of FM2K SFX buffer_arrays
        // appear to be allocated outside the system table; we only Mike-Z the
        // ones we can identify.
        return original_dispatch_script_sound(script_item);
    }
    // Known channel — desired[] updated; defer the real play to
    // SoundRollback::SyncAfterAdvance at end of the displayed frame.
    return 1;
}

void __cdecl Hook_RenderGame() {
    // In trampoline mode, render goes through RenderFrameWithSnapshot in
    // main_loop_trampoline.cpp. This hook still catches direct calls from
    // the game (e.g. init/menu paths) — run diagnostics and pass through.
    Hook_RenderDiagnostics_Tick();

    // CRITICAL: Save/restore RNG around render to prevent render-path RNG
    // consumption from breaking determinism. ProcessShakeEffect and
    // ProcessColorInterpolation call game_rand() during rendering, and
    // Render-path state protection.
    //
    // Stress-mode desync dump (FM2K_stress_desync_f158.log) showed that
    // after a LOAD+replay the four regions below diverged from the forward
    // save even though memcpy restore ran correctly. Cause: the render
    // path (ProcessShakeEffect / ProcessColorInterpolation / sprite
    // updates) mutates these regions, and our render gate skips render
    // during replay frames. Forward ran N renders, replay ran 0 renders,
    // so render-side mutations accumulated only in forward. Result:
    //   RNG_Seed, ObjectPool, AfterimagePool, InputTracking all drifted.
    // CharDynamic / GameState / Object topology stayed matched because
    // render doesn't touch them.
    //
    // Fix: snapshot these regions before render, restore after render.
    // Same idea as the existing RNG protection, extended to the other
    // three. That way render can freely update visual counters but the
    // gameplay-authoritative memory image is unchanged across renders.
    bool protect_regions = Netplay_IsActive();

    uint32_t saved_rng = 0;
    static uint8_t s_saved_object_pool[0x5F800];
    static uint8_t s_saved_afterimage_pool[WaveCAddrs::AFTERIMAGE_POOL_SZ];
    static uint8_t s_saved_input_tracking[0xA0];

    if (protect_regions) {
        saved_rng = *(uint32_t*)FM2K::ADDR_RANDOM_SEED;
        memcpy(s_saved_object_pool,     (void*)FM2K::ADDR_OBJECT_POOL,       FM2K::SIZE_OBJECT_POOL);
        memcpy(s_saved_afterimage_pool, (void*)WaveCAddrs::AFTERIMAGE_POOL,  WaveCAddrs::AFTERIMAGE_POOL_SZ);
        memcpy(s_saved_input_tracking,  (void*)0x447EE0,                     0xA0);
    }

    // In battle mode under GekkoNet, render only when a new sim tick has
    // been produced since the last render. Otherwise render mutates
    // object-pool animation state on wall-clock cadence and desyncs peers.
    bool gate_render = Netplay_IsActive();
    bool do_render = !gate_render || g_frame_pending_render;
    if (do_render && original_render_game) {
        original_render_game();
        g_frame_pending_render = false;
    }

    if (protect_regions) {
        *(uint32_t*)FM2K::ADDR_RANDOM_SEED = saved_rng;
        memcpy((void*)FM2K::ADDR_OBJECT_POOL,      s_saved_object_pool,     FM2K::SIZE_OBJECT_POOL);
        memcpy((void*)WaveCAddrs::AFTERIMAGE_POOL, s_saved_afterimage_pool, WaveCAddrs::AFTERIMAGE_POOL_SZ);
        memcpy((void*)0x447EE0,                    s_saved_input_tracking,  0xA0);
    }

    // Update shared memory with current stats for launcher
    SharedMem_Update();

    // GekkoNet frame pacing - called after render, matching GekkoNet examples.
    // Uses precise QPC timing to hit 10ms target (100fps).
    // When ahead of remote, slows down by 1.6% to prevent rollback cascade.
    if (Netplay_IsActive()) {
        Netplay_HandleFrameTime();
    }
}

// Hook: RunGameLoop
// Detours to the main-loop trampoline; we own the outer game loop from this
// point forward. Pre-trampoline side effects (VS-mode patch) fire before the
// hand-off so CSS behavior is unchanged.
BOOL __cdecl Hook_RunGameLoop() {
    // Set VS player mode once
    static bool vs_mode_set = false;
    if (!vs_mode_set) {
        uint8_t* char_select_mode = (uint8_t*)0x470058;
        DWORD old_protect;
        if (VirtualProtect(char_select_mode, 1, PAGE_READWRITE, &old_protect)) {
            *char_select_mode = 1;  // VS player mode
            VirtualProtect(char_select_mode, 1, old_protect, &old_protect);
            vs_mode_set = true;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Hooks: Set VS player mode");
        }
    }

    return TrampolineMainLoop();
}

// Hook: GameRand - pass through
uint32_t __cdecl Hook_GameRand() {
    return original_game_rand ? original_game_rand() : 0;
}

// Hook: ProcessGameInputs
// During battle: get synced inputs from GekkoNet and write to game memory
int __cdecl Hook_ProcessGameInputs() {
    // Re-pin the FPU control word on every game tick. DirectDraw's
    // SetCooperativeLevel is called without DDSCL_FPUPRESERVE, so DD is
    // allowed to mutate x87 precision at fullscreen toggle / driver callback
    // time. Without this line, two peers can run at different float
    // precision and float-heavy code (movement vectors, hit-rect math)
    // diverges on the first substantial physics tick — which matches the
    // "desync starts when you move" signature exactly.
    PinFPUControlWord();

    uint32_t game_mode = *(uint32_t*)FM2K::ADDR_GAME_MODE;

    // Stress-test mode: block game's own process_game_inputs during battle -
    // GekkoNet drives sim via AdvanceEvent (which calls original_process_game_inputs
    // internally). Outside battle, pass through normally.
    if (g_stress_mode) {
        if (IsBattleMode(game_mode) && Netplay_IsActive()) {
            return 0;
        }
        return original_process_game_inputs ? original_process_game_inputs() : 0;
    }

    // Battle mode with GekkoNet - block during sync, override inputs when active
    if (IsBattleMode(game_mode) && !g_offline_mode && Netplay_IsConnected()) {
        // Block ProcessGameInputs during battle sync barrier and GekkoNet handshake
        // Same reason as CSS: prevents buf_idx advance and edge detection desync
        if (!Netplay_IsActive() || !Netplay_IsSessionReady()) {
            return 0;
        }

        // GekkoNet active: ProcessBattleInputPhase handles process_game_inputs
        // inside each AdvanceEvent. Don't call original here - it would double-tick.
        // Just log periodically.
        static uint32_t log_count = 0;
        if (log_count < 10 || log_count % 200 == 0) {
            uint32_t p1_stored = *(uint32_t*)FM2K::ADDR_P1_INPUT;
            uint32_t p2_stored = *(uint32_t*)FM2K::ADDR_P2_INPUT;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[PROCESS_INPUTS] Synced: P1=0x%03X P2=0x%03X (buf_idx=%u)",
                p1_stored, p2_stored, *(uint32_t*)0x447EE0);
        }
        log_count++;

        return 0;  // Skip - GekkoNet drives input processing
    }

    // CSS mode - block ProcessGameInputs during stalls!
    // Game loop calls ProcessGameInputs BEFORE UpdateGameState.
    // If we let it run during stalls, it advances g_input_buffer_index
    // and runs edge detection out of sync between clients.
    if (IsCSSMode(game_mode) && Netplay_IsConnected()) {
        Netplay_PollCSS();  // Receive pending data

        if (!Netplay_CanAdvanceCSS()) {
            // STALL: Don't call original - prevents buffer index advance
            // and edge detection from consuming inputs during stall
            return 0;
        }

        // Not stalling - let original run (it calls GetPlayerInput which
        // returns synced CSS input through our hook)
        return original_process_game_inputs ? original_process_game_inputs() : 0;
    }

    // Connection barrier - block while waiting for connection
    // Prevents buf_idx divergence before game even starts
    if (!g_offline_mode && !Netplay_IsConnected()) {
        return 0;
    }

    // Offline or connected non-CSS/non-battle: use original
    return original_process_game_inputs ? original_process_game_inputs() : 0;
}

// ============================================================================
// HOOK SETUP
// ============================================================================

bool InitializeHooks() {
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Hooks: Initializing MinHook...");

    PinFPUControlWord();
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Hooks: Pinned x87 FPU control word to _PC_53 | _RC_NEAR");

    if (MH_Initialize() != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Hooks: MH_Initialize failed");
        return false;
    }

    // Hook GetPlayerInput
    if (MH_CreateHook((void*)FM2K::ADDR_GET_PLAYER_INPUT, (void*)Hook_GetPlayerInput,
                      (void**)&original_get_player_input) != MH_OK ||
        MH_EnableHook((void*)FM2K::ADDR_GET_PLAYER_INPUT) != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Hooks: Failed to hook GetPlayerInput");
        return false;
    }

    // Hook UpdateGameState
    if (MH_CreateHook((void*)FM2K::ADDR_UPDATE_GAME, (void*)Hook_UpdateGameState,
                      (void**)&original_update_game) != MH_OK ||
        MH_EnableHook((void*)FM2K::ADDR_UPDATE_GAME) != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Hooks: Failed to hook UpdateGameState");
        return false;
    }

    // Hook RenderGame
    if (MH_CreateHook((void*)FM2K::ADDR_RENDER_GAME, (void*)Hook_RenderGame,
                      (void**)&original_render_game) != MH_OK ||
        MH_EnableHook((void*)FM2K::ADDR_RENDER_GAME) != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Hooks: Failed to hook RenderGame");
        return false;
    }

    // Hook RunGameLoop
    if (MH_CreateHook((void*)FM2K::ADDR_RUN_GAME_LOOP, (void*)Hook_RunGameLoop,
                      (void**)&original_run_game_loop) != MH_OK ||
        MH_EnableHook((void*)FM2K::ADDR_RUN_GAME_LOOP) != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Hooks: Failed to hook RunGameLoop");
        return false;
    }

    // Hook GameRand
    if (MH_CreateHook((void*)FM2K::ADDR_GAME_RAND, (void*)Hook_GameRand,
                      (void**)&original_game_rand) != MH_OK ||
        MH_EnableHook((void*)FM2K::ADDR_GAME_RAND) != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Hooks: Failed to hook GameRand");
        return false;
    }

    // Hook ProcessGameInputs
    if (MH_CreateHook((void*)FM2K::ADDR_PROCESS_INPUTS, (void*)Hook_ProcessGameInputs,
                      (void**)&original_process_game_inputs) != MH_OK ||
        MH_EnableHook((void*)FM2K::ADDR_PROCESS_INPUTS) != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Hooks: Failed to hook ProcessGameInputs");
        return false;
    }

    // Hook DispatchScriptSoundCommand — Mike Z desired/actual sound layer.
    // During battle the hook records `desired[channel]` instead of playing;
    // SoundRollback::SyncAfterAdvance reconciles once per displayed frame by
    // calling back through the original trampoline.
    if (MH_CreateHook((void*)FM2K::ADDR_DISPATCH_SCRIPT_SOUND,
                      (void*)Hook_DispatchScriptSoundCommand,
                      (void**)&original_dispatch_script_sound) != MH_OK ||
        MH_EnableHook((void*)FM2K::ADDR_DISPATCH_SCRIPT_SOUND) != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Hooks: Failed to hook DispatchScriptSoundCommand");
        return false;
    }
    SoundRollback::SetOriginalDispatcher(original_dispatch_script_sound);

    // Hook timeGetTime (winmm.dll) — make the game's frame-skip pacing
    // deterministic across peers. See comment on Hook_timeGetTime for the
    // rationale. Resolve the real address dynamically so the hook works
    // regardless of IAT layout.
    HMODULE winmm = GetModuleHandleA("winmm.dll");
    if (!winmm) winmm = LoadLibraryA("winmm.dll");
    if (winmm) {
        void* real_timeGetTime = (void*)GetProcAddress(winmm, "timeGetTime");
        if (real_timeGetTime) {
            if (MH_CreateHook(real_timeGetTime, (void*)Hook_timeGetTime,
                              (void**)&original_timeGetTime) != MH_OK ||
                MH_EnableHook(real_timeGetTime) != MH_OK) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                             "Hooks: Failed to hook timeGetTime");
                return false;
            }
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Hooks: timeGetTime hooked for deterministic frame pacing");
        } else {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Hooks: GetProcAddress(timeGetTime) failed");
        }
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Hooks: All hooks installed successfully");
    return true;
}

void ShutdownHooks() {
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Hooks: Shutdown complete");
}
