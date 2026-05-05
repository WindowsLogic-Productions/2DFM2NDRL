// See main_loop_trampoline.h for rationale.
//
// We own the outer game loop. main_game_loop @0x405AD0 is hooked and detours
// to TrampolineMainLoop(); the original is never called. Eliminating the
// native wrapper eliminates its per-iteration state writes (g_last_frame_time,
// per-slot buf-idx fan-out, etc.) that run at a different cadence on
// forward-sim versus rollback-replay. All sim state transitions happen
// inside GekkoAdvanceEvent (in netplay.cpp) which is deterministic.
//
// Phase dispatch per iteration:
//   TRAMPOLINE_BATTLE — Netplay_ProcessBattleInputPhase drives sim via gekko,
//                       render fires post-advance, pacing via HandleFrameTime.
//   CSS               — lockstep via control channel; native sim functions
//                       called directly; pacing via a 10 ms sleep.
//   NATIVE            — menu/intro/results; native sim functions called
//                       directly; pacing via a 10 ms sleep.
#include "main_loop_trampoline.h"
#include "globals.h"
#include "../hooks/hooks.h"
#include "../hooks/wndproc_subclass.h"
#include "../netplay/netplay.h"
#include "../netplay/control_channel.h"
#include "../netplay/game_hash.h"
#include "../netplay/spectator_node.h"
#include "../ui/shared_mem.h"
#include "../parity/parity_recorder.h"
#include <SDL3/SDL_log.h>
#include <SDL3/SDL_timer.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <windows.h>

// Game-side symbols we call directly (bypassing the hooks that wrap them).
extern RunGameLoopFunc        original_run_game_loop;
extern ProcessGameInputsFunc  original_process_game_inputs;
extern UpdateGameStateFunc    original_update_game;
extern RenderGameFunc         original_render_game;

// Shared state exposed by hooks.cpp / netplay.cpp.
extern bool g_frame_pending_render;

// CheckGameModeTransition is a static in hooks.cpp. We need the same behavior
// here; declared with a public shim.
extern "C" void Hook_CheckGameModeTransition_Public();
extern "C" void Hook_RenderDiagnostics_Tick();
extern "C" void Hook_BattleDiag_TickIfActive();

// ============================================================================
// RENDER SNAPSHOT/RESTORE
// ============================================================================
// Render mutates sim-authoritative memory (object animation counters, RNG via
// ProcessShakeEffect / ProcessColorInterpolation, input-tracking fields).
// If render runs at wall-clock cadence it leaks extra mutations into the save
// state. We snapshot the affected regions, run render, then restore.
// Identical to what Hook_RenderGame used to do; moved here so the trampoline
// owns the render step end-to-end.
#include "../netplay/savestate.h"  // WaveCAddrs

static uint8_t s_render_saved_object_pool[FM2K::SIZE_OBJECT_POOL];
static uint8_t s_render_saved_afterimage[WaveCAddrs::AFTERIMAGE_POOL_SZ];
static uint8_t s_render_saved_input_tracking[0xA0];

// SHAKE_EFFECTS block (40 B at 0x447DA9..0x447DD1) must be carved out of
// the render-side afterimage snapshot so ProcessShakeEffect's per-render
// timer decrement (a1[3]--) reaches sim memory. The dev-scripted
// Duration in each character's KGT drives how long shake lasts; we need
// that countdown to propagate. Without this carve, the snapshot reverts
// the decrement every render, sim state is permanently pinned at
// timer == duration, and any character whose script re-SETs the opcode
// each frame rumbles forever.
constexpr uintptr_t SHAKE_EFFECTS_ADDR  = 0x447DA9;
constexpr size_t    SHAKE_EFFECTS_SZ    = 40;
constexpr size_t    SHAKE_OFFSET_IN_AI  = SHAKE_EFFECTS_ADDR - WaveCAddrs::AFTERIMAGE_POOL;  // 0x479

// EFFECT_SYS1 (palette flash 1 struct) sits inside the afterimage_pool slice
// at 0x447D7D, 42 B, immediately BEFORE the shake block. Render-side carve-
// out must skip this region too — ProcessColorInterpolation writes
// per-frame interpolation values into g_object_data_ptr from inputs read out
// of EFFECT_SYS1, and the [EB] handler in update_game writes the struct's
// timer/duration. If the render-side restore stomps these every render, the
// palette flash either snaps back to its pre-render state (visible flicker)
// or never reflects the current sim's progress.
constexpr uintptr_t PFLASH1_ADDR        = 0x447D7D;
constexpr size_t    PFLASH1_SZ          = 42;
constexpr size_t    PFLASH1_OFFSET_IN_AI = PFLASH1_ADDR - WaveCAddrs::AFTERIMAGE_POOL;
static_assert(PFLASH1_OFFSET_IN_AI + PFLASH1_SZ <= SHAKE_OFFSET_IN_AI,
              "EFFECT_SYS1 must end before shake block");

// [EB] diagnostic — see header for full doc. Defined here so both the
// trampoline path and Hook_RenderGame share state (frame counter, log fp,
// post-shake window). Only one of those two paths fires per frame depending
// on FM2K_BYPASS_TRAMPOLINE: trampoline mode → RenderFrameWithSnapshot calls
// us; bypass mode → Hook_RenderGame (the MinHook detour) calls us.
void EbDiag_Dump(const char* tag) {
    static const bool s_eb_diag = []() {
        const char* v = std::getenv("FM2K_EB_DIAG");
        return v && v[0] == '1';
    }();
    if (!s_eb_diag) return;

    static FILE* s_eb_diag_fp = nullptr;
    static uint32_t s_eb_frame = 0;
    static int s_eb_post_window = 0;

    const uint32_t s1_mode  = *(uint32_t*)(SHAKE_EFFECTS_ADDR + 0);
    const uint32_t s1_off   = *(uint32_t*)(SHAKE_EFFECTS_ADDR + 4);
    const uint32_t s1_amp   = *(uint32_t*)(SHAKE_EFFECTS_ADDR + 8);
    const uint32_t s1_timer = *(uint32_t*)(SHAKE_EFFECTS_ADDR + 12);
    const uint32_t s1_dur   = *(uint32_t*)(SHAKE_EFFECTS_ADDR + 16);
    const uint32_t s2_off   = *(uint32_t*)(SHAKE_EFFECTS_ADDR + 4 + 20);
    const uint32_t s2_timer = *(uint32_t*)(SHAKE_EFFECTS_ADDR + 12 + 20);
    // Palette flash state — same [EB] opcode handler, separate timers.
    // p1: g_effect_id_1 @ 0x447D7D; timer (a1[5]) @ +0x14 (g_timer_countdown2 0x447D91)
    // p2: g_effect_id_2 @ 0x4456D0; timer (a1[5]) @ +0x14 (g_timer_countdown1 0x4456E4)
    // dur (a1[10]) @ +0x28
    const uint32_t p1_mode  = *(uint32_t*)0x447D7D;
    const uint32_t p1_timer = *(uint32_t*)0x447D91;
    const uint32_t p1_dur   = *(uint32_t*)0x447DA5;  // 0x447D7D + 0x28
    const uint32_t p2_mode  = *(uint32_t*)0x4456D0;
    const uint32_t p2_timer = *(uint32_t*)0x4456E4;
    const uint32_t p2_dur   = *(uint32_t*)0x4456F8;  // 0x4456D0 + 0x28
    // Global RNG seed — ProcessColorInterpolation mode 3 calls game_rand()
    // each render. Comparing rng across vanilla (FM2K_BYPASS_TRAMPOLINE=1)
    // vs our trampoline path tells us whether render-time RNG matches
    // vanilla. If rng differs, palette colors will visibly differ even
    // when timer/duration are identical.
    const uint32_t rng_seed = *(uint32_t*)0x41FB1C;
    const bool active = (s1_timer != 0 || s2_timer != 0
                         || p1_timer != 0 || p2_timer != 0);

    // Bump the per-render-boundary frame counter on PRE-SAVE only so all four
    // tags in a single render cycle share the same `f=` value.
    const bool is_pre_save = (tag[0] == 'P' && tag[1] == 'R' && tag[2] == 'E'
                              && tag[4] == 'S');
    if (is_pre_save) ++s_eb_frame;

    if (active) s_eb_post_window = 60;  // ~0.6s of post-shake camera tracking
    else if (s_eb_post_window > 0 && is_pre_save) --s_eb_post_window;
    else if (s_eb_post_window == 0 && !active) return;

    // g_screen_x/y at 0x447F2C/30 — sprite_rendering_engine reads these for
    // every sprite (stage AND characters). If they drift after shake ends,
    // that's the stage-offset bug.
    const int32_t scr_x = *(int32_t*)0x447F2C;
    const int32_t scr_y = *(int32_t*)0x447F30;
    if (!s_eb_diag_fp) {
        char base[64];
        std::snprintf(base, sizeof(base),
                      "FM2K_eb_diag_pid%lu.log",
                      (unsigned long)GetCurrentProcessId());
        char path[MAX_PATH];
        if (!Fm2k_BuildLogPath(path, sizeof(path), base)) {
            std::snprintf(path, sizeof(path), "%s", base);
        }
        s_eb_diag_fp = std::fopen(path, "w");
        if (s_eb_diag_fp) {
            std::fprintf(s_eb_diag_fp,
                "# tag, frame, shake_s1{mode,off,amp,tmr,dur}, shake_s2{off,tmr}, "
                "palette_p1{m,tmr,dur}, palette_p2{m,tmr,dur}, rng, scr_x, scr_y\n");
            std::fflush(s_eb_diag_fp);
        }
    }
    if (s_eb_diag_fp) {
        std::fprintf(s_eb_diag_fp,
            "%-12s f=%u s1{m=%u o=%d amp=%u tmr=%u dur=%u} s2{o=%d tmr=%u} "
            "p1{m=%u tmr=%u dur=%u} p2{m=%u tmr=%u dur=%u} rng=%08x scr=(%d,%d)%s\n",
            tag, s_eb_frame,
            s1_mode, (int32_t)s1_off, s1_amp, s1_timer, s1_dur,
            (int32_t)s2_off, s2_timer,
            p1_mode, p1_timer, p1_dur,
            p2_mode, p2_timer, p2_dur,
            rng_seed, scr_x, scr_y,
            active ? "" : " [post-shake]");
        // Flush only on POST-RESTORE so a crash mid-frame leaves the log
        // readable. One fflush per frame instead of four.
        if (tag[0] == 'P' && tag[1] == 'O' && tag[5] == 'R' && tag[6] == 'E') {
            std::fflush(s_eb_diag_fp);
        }
    }
}

static void RenderFrameWithSnapshot() {
    if (!original_render_game) return;

    // FPS counter + title-bar stats (hotkey probe too). Hook_RenderGame used
    // to do this; in trampoline mode render goes through here instead.
    Hook_RenderDiagnostics_Tick();

    // Per-frame state dump during battle-transition windows. Cheap (single
    // counter check per frame); only emits log lines for ~3 sec around
    // every CSS↔battle boundary.
    Hook_BattleDiag_TickIfActive();

    // Render isolates sim state when we're driving the simulation
    // deterministically — either as a player under GekkoNet (host) or as
    // a spectator replaying confirmed inputs. Without protection, render's
    // mutations to RNG / object pool / afterimage / input tracking leak into
    // sim memory, and the spectator's evolution diverges from the host's
    // (which IS protected). One frame is enough to desync RNG.
    const bool protect = Netplay_IsActive() || SpectatorNode_IsPlayingBack();
    EbDiag_Dump("PRE-SAVE");
    if (protect) {
        // RNG is intentionally NOT save+restored across render anymore.
        // Render-side game_rand() calls (ProcessColorInterpolation mode 3,
        // ProcessShakeEffect mode 4, sprite effects) need to propagate to
        // sim memory or palette flash visuals freeze on a single static
        // interpolation factor instead of the animated random gradient
        // vanilla shows. Both peers run render once per wall-clock frame
        // and consume identical RNG amounts, so they stay in lockstep
        // without explicit RNG restore. Verified via offline/bypass diff —
        // offline (no protection) matches vanilla; online (with protection)
        // showed RNG frozen at the pre-render value across 20+ frames.
        //
        // Object pool is also intentionally NOT saved/restored. The render
        // path writes per-object color override values via
        // ProcessColorInterpolation (g_object_data_ptr + 68/72/76/80) AND
        // various sprite/animation timers. Reverting those after render
        // makes palette flash mode 1 (Tyrogue fade-to-black) visually
        // "undone" — sim-side timer keeps decrementing, but the persistent
        // color state needed across frames gets wiped. Both peers run the
        // same renders, so object pool drift stays symmetric.
        (void)s_render_saved_object_pool; // (kept allocated for backward compat)
        // Afterimage save: three slices skipping both EFFECT_SYS1 (palette
        // flash 1) and SHAKE_EFFECTS so render-side state evolution for
        // each propagates back into sim memory:
        //   [0                     .. PFLASH1_OFFSET)   — save (head)
        //   [PFLASH1_OFFSET        .. PFLASH1_END)      — SKIP (palette flash 1)
        //   [PFLASH1_END           .. SHAKE_OFFSET)     — save (gap, currently 0 B)
        //   [SHAKE_OFFSET          .. SHAKE_END)        — SKIP (shake block)
        //   [SHAKE_END             .. POOL_END)         — save (tail)
        constexpr size_t PFLASH1_END_IN_AI = PFLASH1_OFFSET_IN_AI + PFLASH1_SZ;
        constexpr size_t SHAKE_END_IN_AI   = SHAKE_OFFSET_IN_AI + SHAKE_EFFECTS_SZ;
        memcpy(s_render_saved_afterimage,
               (void*)WaveCAddrs::AFTERIMAGE_POOL,
               PFLASH1_OFFSET_IN_AI);
        memcpy(s_render_saved_afterimage + PFLASH1_END_IN_AI,
               (void*)(WaveCAddrs::AFTERIMAGE_POOL + PFLASH1_END_IN_AI),
               SHAKE_OFFSET_IN_AI - PFLASH1_END_IN_AI);
        memcpy(s_render_saved_afterimage + SHAKE_END_IN_AI,
               (void*)(WaveCAddrs::AFTERIMAGE_POOL + SHAKE_END_IN_AI),
               WaveCAddrs::AFTERIMAGE_POOL_SZ - SHAKE_END_IN_AI);
        memcpy(s_render_saved_input_tracking, (void*)0x447EE0,                     0xA0);
    }

    EbDiag_Dump("PRE-RENDER");
    original_render_game();
    EbDiag_Dump("POST-RENDER");

    if (protect) {
        // (RNG restore removed — see PRE-RENDER comment above.)
        // (Object pool restore removed too — see PRE-RENDER comment.
        // Tyrogue's mode-1 fade-to-black depends on the last per-frame
        // ProcessColorInterpolation write to g_object_data_ptr+68/72/76/80
        // PERSISTING into the next frame's object_pool. When the timer
        // hits 0, ProcessColorInterpolation skips the write — and the
        // sprite render reads the persisted last-frame value (mostly
        // black) so the screen stays black. With object_pool restore,
        // that persistence is wiped each frame and the visual snaps back
        // to the default palette.)
        // Afterimage restore: mirror of the 3-slice split save.
        // EFFECT_SYS1 + shake_effects regions in live memory keep whatever
        // ProcessColorInterpolation / ProcessShakeEffect just wrote.
        constexpr size_t PFLASH1_END_IN_AI = PFLASH1_OFFSET_IN_AI + PFLASH1_SZ;
        constexpr size_t SHAKE_END_IN_AI   = SHAKE_OFFSET_IN_AI + SHAKE_EFFECTS_SZ;
        memcpy((void*)WaveCAddrs::AFTERIMAGE_POOL,
               s_render_saved_afterimage,
               PFLASH1_OFFSET_IN_AI);
        memcpy((void*)(WaveCAddrs::AFTERIMAGE_POOL + PFLASH1_END_IN_AI),
               s_render_saved_afterimage + PFLASH1_END_IN_AI,
               SHAKE_OFFSET_IN_AI - PFLASH1_END_IN_AI);
        memcpy((void*)(WaveCAddrs::AFTERIMAGE_POOL + SHAKE_END_IN_AI),
               s_render_saved_afterimage + SHAKE_END_IN_AI,
               WaveCAddrs::AFTERIMAGE_POOL_SZ - SHAKE_END_IN_AI);
        memcpy((void*)0x447EE0,                    s_render_saved_input_tracking, 0xA0);
    }
    EbDiag_Dump("POST-RESTORE");

    // Back-patch the most-recently-saved slot's rng_seed with the post-
    // render rng so rollback Load gives replay sim the same starting rng
    // forward sim sees on the next frame. Without this, forward accumulated
    // render-side game_rand calls (ProcessShakeEffect mode 4 +
    // ProcessColorInterpolation mode 3) but replay didn't, and they
    // diverged on the rng region after any rollback — exactly the
    // "RNG_Seed forward 0x... replay 0x... DIFF" dump in
    // FM2K_P*_desync_f*.log. Cheap (single uint32 store).
    SaveState_PatchPostRenderRng(*(uint32_t*)FM2K::ADDR_RANDOM_SEED);

    SharedMem_Update();
}

// ============================================================================
// UTILITIES
// ============================================================================

static LoopPhase ClassifyPhase() {
    // Spectator mode pins the phase: regardless of game_mode or whether the
    // upstream JOIN_ACK has arrived yet, run RunSpectatorTick. Pre-ACK the
    // queue is empty so the trampoline holds the boot frame; once inputs
    // start flowing the sim animates through CSS, locks chars, transitions
    // to battle, etc. — all driven by the streamed input pipe.
    if (g_spectator_mode || SpectatorNode_IsPlayingBack()) {
        return LoopPhase::SPECTATOR_PLAYBACK;
    }

    // Battle session still alive (e.g. game_mode just flipped 3000→2000 inside
    // the final battle UG, but the GekkoNet battle session hasn't been
    // destroyed yet — it's waiting on Netplay_IsBattleEndSynced). Stay in the
    // BATTLE phase so RunBattleTick keeps polling the swap-frame gate and
    // calling Netplay_EndBattle. Without this, the battle session sticks
    // forever and Netplay_StartCSSSession spins in a "session already exists"
    // retry loop.
    if (Netplay_GetSessionKind() == NetplaySessionKind::BATTLE) {
        return LoopPhase::TRAMPOLINE_BATTLE;
    }

    // Engine-aware phase mapping. On FM2K g_game_mode hits 2000/3000+; on
    // FM95 it stays near 0 and phase lives in the object pool — IsCSSMode /
    // IsBattleMode (hooks.cpp, declared in hooks.h) do the right walk for
    // each engine.
    uint32_t mode = *(uint32_t*)FM2K::ADDR_GAME_MODE;
    if (IsBattleMode(mode)) return LoopPhase::TRAMPOLINE_BATTLE;
    if (IsCSSMode(mode))    return LoopPhase::CSS;
    return LoopPhase::NATIVE;
}

// Non-blocking Windows message pump. Mirrors main_game_loop @ 0x405B0D.
// Returns false on WM_QUIT so the trampoline can exit cleanly.
static bool PumpMessages() {
    MSG msg;
    while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            // Surface the wParam (exit code from PostQuitMessage) AND
            // who the message was for. wParam=0 is normal close;
            // anything else is "something inside the game asked to
            // quit immediately." A user reporting WM_QUIT 4 ms after
            // 'main loop ACTIVE' means the game window self-destructed
            // during creation — usually DirectDraw/DirectInput init
            // failure on the user's machine (driver, fullscreen, or
            // antivirus interference).
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "PumpMessages: WM_QUIT received (wParam=%u hwnd=%p) — main loop will exit",
                (unsigned)msg.wParam, (void*)msg.hwnd);
            return false;
        }
        // Diagnostic: dump every key/cheat-relevant message coming through.
        // FM2K's WindowProc maps F1-F12 to debug cheats (F1 hit-boxes, F5/F6
        // instant-KO, F12 force round-end). StudioS games are mysteriously
        // receiving F12 events the user isn't actually pressing. Need to see
        // what the OS / synthesizer is queuing. Filter is gone for now —
        // we want to *see* the F12s before deciding how to handle.
        if (msg.message == WM_KEYDOWN || msg.message == WM_KEYUP
            || msg.message == WM_SYSKEYDOWN || msg.message == WM_SYSKEYUP
            || msg.message == WM_CHAR)
        {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[MSG] msg=0x%04X wParam=0x%02X (%u) lParam=0x%08lX hwnd=%p",
                        (unsigned)msg.message, (unsigned)msg.wParam,
                        (unsigned)msg.wParam, (unsigned long)msg.lParam,
                        msg.hwnd);
        }
        // Suppress solo-Alt menu activation. When the user taps Alt with
        // no follow-up key, Windows sends WM_SYSCOMMAND/SC_KEYMENU which
        // makes the game's WindowProc enter modal menu mode (DefWindowProc
        // blocks the message pump until the user picks a menu item or
        // presses Esc). FM2K has no menu, so the game just appears to
        // freeze for a beat. Discard the no-key SC_KEYMENU but let other
        // SC_* commands (SC_CLOSE on the X button, SC_RESTORE on un-min,
        // SC_KEYMENU with an actual hotkey) pass through.
        if (msg.message == WM_SYSCOMMAND
            && (msg.wParam & 0xFFF0) == SC_KEYMENU
            && msg.lParam == 0)
        {
            continue;  // swallowed
        }
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return true;
}

// Sleep until at least `target_ms` have elapsed since `start_qpc`. QPC-based
// for sub-ms accuracy: SDL_Delay/Sleep on Windows overshoots by 1-2 ms even
// with timeBeginPeriod(1) active, which caps the frame loop at ~90 FPS on a
// 10 ms target. Strategy: coarse SDL_Delay until within 2 ms of the
// deadline, then busy-wait the remainder using QueryPerformanceCounter.
// Burns a fraction of a millisecond of CPU at the tail of each frame in
// exchange for actual-100-FPS pacing.
static void SleepToTarget(uint64_t start_qpc, uint32_t target_ms,
                          float frames_ahead = 0.0f) {
    const uint64_t freq = SDL_GetPerformanceFrequency();
    uint64_t target_ticks_count = (freq * target_ms) / 1000;
    // GekkoNet drift correction. When we're more than half a frame
    // ahead of the peer's confirmed input, lengthen this frame's
    // target by 1.6%. Over time the local sim slows just enough for
    // the lagging peer to catch up. Matches the canonical pattern
    // in vendored/GekkoNet/Examples/OnlineSession/OnlineSession.cpp.
    // Without this the host's frame loop holds rigid 10 ms while the
    // client falls 7-8 frames behind permanently — the symptom of
    // "host says rb=300, client says rb=0, ahead pinned at ±8.5".
    if (frames_ahead > 0.5f) {
        target_ticks_count = (target_ticks_count * 1016) / 1000;
    }
    const uint64_t target_ticks = start_qpc + target_ticks_count;
    for (;;) {
        uint64_t now = SDL_GetPerformanceCounter();
        if (now >= target_ticks) return;
        uint64_t remaining_ticks = target_ticks - now;
        uint64_t remaining_ms = (remaining_ticks * 1000) / freq;
        if (remaining_ms > 2) {
            // Sleep all but the last ~1.5ms worth — leaves overshoot headroom.
            SDL_Delay((uint32_t)(remaining_ms - 1));
        }
        // Fall through and re-check; the tail gets busy-waited.
    }
}

// ============================================================================
// PER-PHASE TICK BODIES
// ============================================================================

// Battle: GekkoNet owns state transitions. main_game_loop's prologue writes
// are gone — we neither replicate them here nor let the native wrapper run.
// Any per-advance setup (like the per-slot buf-idx fan-out) already lives
// inside the AdvanceEvent handler in netplay.cpp.
static void RunBattleTick() {
    Hook_CheckGameModeTransition_Public();

    // Stress-mode path: no network, no peer sync. GekkoStressSession is the
    // determinism check — it rolls back every check_distance frames and
    // compares save hashes.
    if (g_stress_mode) {
        if (!Netplay_IsActive()) {
            if (!Netplay_StartStressBattle()) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                             "Trampoline: Netplay_StartStressBattle failed");
                return;
            }
        }
        Netplay_ProcessBattleInputPhase();
        if (g_frame_pending_render) {
            RenderFrameWithSnapshot();
            g_frame_pending_render = false;
        }
        // Drift adjustment is now handled by SleepToTarget at the
        // outer loop call site, which applies the 1.6% slowdown when
        // ahead. Sleep(0)/Sleep(extra_ms) trick that lived in
        // Netplay_HandleFrameTime was unreliable and never converged.
        return;
    }

    // Offline path: no peer, no GekkoNet, no sync barrier. Just run the sim
    // natively — same shape as RunNativeTick but invoked from the battle
    // phase. Without this branch, g_battle_entry_signaled_pub stays true and
    // Netplay_IsBattleSynced never returns true (there's no remote to sync
    // with), so RunBattleTick would hang forever the moment game_mode hits
    // 3000. Sound rollback / GekkoNet-driven state machine are inert here;
    // the hook gates on Netplay_IsActive() which stays false.
    if (g_offline_mode) {
        // [REMOVED] per-slot fan-out at slot+0xDF79 / slot+0xDF7D.
        // Originally added to mimic main_game_loop's prologue, but IDA
        // xref scan reveals those fields are DirectPlay-specific:
        //   slot+0xDF7D = g_p1_input_buffer_index_field — read only by
        //     check_game_continue (DirectPlay packet handler, no-op
        //     when g_directplay_interface == NULL, which is always the
        //     case for us)
        //   slot+0xDF79 = g_net_sync_frame_counter — written/read only
        //     by check_game_continue
        // Neither KGT scripts nor update_game/process_game_inputs read
        // these fields. Writing them every frame did nothing useful and
        // interfered with adjacent memory the StudioS chars apparently
        // touch. Per-slot fan-out also removed from spectator + GekkoNet
        // paths (same reason — DirectPlay isn't used anywhere).

        // Round-end-flag tripwire — leave in place to confirm the writer.
        constexpr uintptr_t REF_ADDR = 0x424718;
        uint32_t ref_before = *(volatile uint32_t*)REF_ADDR;

        // Pre-update snapshot of the case-200 exit conditions in
        // vs_round_function. Case 200 transitions to state 300 (round
        // end) on:
        //   (1) g_score_value crosses below 0 in the same frame
        //   (2) active type-4 fighter count < 2 in story/team mode
        //   (3) g_round_end_flag != 0
        // Render-time logs show -1 / 2 / 0 — none should trigger. But
        // the bail still happens, so one of these MUST be firing during
        // update_game (the per-frame snapshot at render time misses
        // transient values). Log the fields right before update_game so
        // we capture the value the game's case-200 walk actually sees.
        int32_t  pre_score = *(int32_t*)0x470050;
        uint32_t pre_ref   = *(uint32_t*)0x424718;
        // Recount t4 with the same exact walk vs_round_function uses.
        // Pool @ 0x4701E0, stride 382, type uint32 == 4, alive @ +346
        // == 0, hp[entry+342] != 0.
        constexpr uintptr_t HP_BASE_LOCAL = 0x4DFC85;
        constexpr uintptr_t HP_STRIDE_L   = 57407;
        int pre_t4 = 0;
        {
            const uint8_t* pool = (const uint8_t*)0x4701E0;
            for (int i = 0; i < 1024; i++) {
                const uint8_t* e = pool + i * 382;
                if (*(const uint32_t*)(e + 0) != 4) continue;
                if (*(const uint32_t*)(e + 346) != 0) continue;
                uint32_t s = *(const uint32_t*)(e + 342);
                if (s >= 8) continue;
                if (*(const uint32_t*)(HP_BASE_LOCAL + s * HP_STRIDE_L) == 0)
                    continue;
                pre_t4++;
            }
        }
        uint32_t pre_mode = *(uint32_t*)FM2K::ADDR_GAME_MODE;

        if (original_process_game_inputs) original_process_game_inputs();
        uint32_t ref_after_pgi = *(volatile uint32_t*)REF_ADDR;
        if (ref_after_pgi != ref_before && ref_after_pgi != 0) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[REF-TRIP] round_end_flag flipped %u->%u during "
                        "original_process_game_inputs",
                        ref_before, ref_after_pgi);
        }

        if (original_update_game) original_update_game();
        uint32_t ref_after_ug = *(volatile uint32_t*)REF_ADDR;
        if (ref_after_ug != ref_after_pgi && ref_after_ug != 0) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[REF-TRIP] round_end_flag flipped %u->%u during "
                        "original_update_game",
                        ref_after_pgi, ref_after_ug);
        }

        // Post-update bracket of case-200 exit conditions. If pre/post
        // differ on score from non-negative to negative, score path
        // fired. If pre_t4 was < 2, t4 path fired. If pre_ref was non-0,
        // round_end_flag path fired (already covered by REF-TRIP above
        // if it persisted). Log only when game_mode is battle and
        // anything actionable changed — avoid log spam.
        int32_t  post_score = *(int32_t*)0x470050;
        int post_t4 = 0;
        {
            const uint8_t* pool = (const uint8_t*)0x4701E0;
            for (int i = 0; i < 1024; i++) {
                const uint8_t* e = pool + i * 382;
                if (*(const uint32_t*)(e + 0) != 4) continue;
                if (*(const uint32_t*)(e + 346) != 0) continue;
                uint32_t s = *(const uint32_t*)(e + 342);
                if (s >= 8) continue;
                if (*(const uint32_t*)(HP_BASE_LOCAL + s * HP_STRIDE_L) == 0)
                    continue;
                post_t4++;
            }
        }
        if (pre_mode >= 3000 && pre_mode < 4000) {
            // Score-cross trigger: only the actual zero crossing
            // (pre>=0, post<0) fires the case-200 → state-300 path.
            // Some games (Vanguard Princess, etc.) use score as a
            // per-frame ticking meter, so any-decrement detection
            // floods the log without capturing a real transition.
            if (pre_score >= 0 && post_score < 0) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[CASE200-TRIP] score path fired: %d -> %d (case 200 "
                    "decremented past 0 → state 300)",
                    pre_score, post_score);
            }
            // t4 trigger: at the moment case 200 walked, t4 was < 2
            if (pre_t4 < 2) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[CASE200-TRIP] t4 path candidate: pre_t4=%d (post=%d) "
                    "— if case 200 saw <2, transitioned to state 300",
                    pre_t4, post_t4);
            }
            // round_end_flag trigger: pre_ref was non-zero
            if (pre_ref != 0) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[CASE200-TRIP] round_end_flag path fired: pre_ref=%u",
                    pre_ref);
            }
        }

        ParityRecorder::Capture();
        RenderFrameWithSnapshot();
        uint32_t ref_after_render = *(volatile uint32_t*)REF_ADDR;
        if (ref_after_render != ref_after_ug && ref_after_render != 0) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[REF-TRIP] round_end_flag flipped %u->%u during "
                        "RenderFrameWithSnapshot",
                        ref_after_ug, ref_after_render);
        }
        return;
    }

    // Networked path: wait for remote peer to also enter battle mode, then
    // start GekkoNet.
    extern bool g_battle_entry_signaled_pub;
    if (g_battle_entry_signaled_pub && !Netplay_IsActive()) {
        Netplay_PollBattleSync();
        if (!Netplay_IsBattleSynced()) {
            return;  // still waiting for remote
        }
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Trampoline: battle sync complete, starting GekkoNet");
        if (!Netplay_StartBattle()) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "Trampoline: Netplay_StartBattle failed");
        }
    }

    if (Netplay_IsActive()) {
        Netplay_ProcessBattleInputPhase();
        if (g_frame_pending_render) {
            RenderFrameWithSnapshot();
            g_frame_pending_render = false;
        }
        // Drift adjustment now lives in SleepToTarget at the outer
        // loop (see RunBattleTick comment in TRAMPOLINE_BATTLE case).

        // Battle-exit swap-frame gate. Once CheckGameModeTransition has
        // detected game_mode leaving battle range it called
        // Netplay_SignalBattleEnd() which sent BATTLE_END(swap_frame).
        // Both peers + any spectators wait until they reach that frame
        // before tearing down the GekkoNet battle session, so the swap
        // is observed at the same logical point on every node.
        Netplay_PollBattleEndSync();
        if (Netplay_IsBattleEndSynced()) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Trampoline: battle-end swap_frame reached — destroying battle session");
            Netplay_EndBattle();
        }
    }
}

// CSS: lockstep via control channel. Same sim functions as native, plus a
// stall check that waits for the remote peer's input before advancing.
// When stalling we only pump messages and poll the control channel; we do
// NOT run the sim (so the peers stay frame-aligned).
static void RunCssTick() {
    Hook_CheckGameModeTransition_Public();

    // Stress mode + offline both run CSS natively — no peer, no lockstep.
    const bool skip_netplay = g_offline_mode || g_stress_mode;

    // No connection barrier here. Same reasoning as RunNativeTick:
    // freezing the render path while we wait for handshake leaves the
    // user staring at an unresponsive CSS window for several seconds —
    // they tab out, see "not responding", and force-close. The CSS
    // state machine is fine to tick unsynchronized in this window:
    // Netplay_ProcessCSS already handles the !g_remote_css_ready case
    // by running the game locally without driving GekkoNet, and on the
    // first synced frame it reseeds the RNG so peers re-converge. We
    // still pump ControlChannel/HELLO here so handshake makes
    // progress; if the MM-timer-driven Poll didn't already finish it,
    // this completes it.
    if (!skip_netplay && !Netplay_IsConnected()) {
        ControlChannel_Poll();
        static uint32_t last_poll = 0;
        uint32_t now = GetTickCount();
        if (now - last_poll > 500) {
            ControlChannel_SendHello((uint8_t)g_player_index,
                                     fm2k::game_hash::Compute());
            last_poll = now;
        }
        // Fall through — let the CSS tick.
    }

    // CSS lockstep + stall: Netplay_ProcessCSS returns false while we're
    // waiting on a remote input for the current CSS frame.
    if (!skip_netplay) {
        if (!Netplay_ProcessCSS()) {
            return;
        }
    }

    // Run the native CSS tick.
    if (original_process_game_inputs) original_process_game_inputs();
    if (original_update_game)         original_update_game();
    ParityRecorder::Capture();  // post-update snapshot for parity .pty

    // Advance virtual_time to match the spectator's per-pop bump cadence.
    // Hook_timeGetTime returns g_virtual_time_ms whenever a session is
    // active OR a spectator is subscribed; if host doesn't bump per CSS
    // tick, host's timeGetTime stays at zero while spectator's accelerates
    // by 10/pop, and any FM2K CSS code that reads timeGetTime (animation
    // timers, fade counters) sees different deltas → desync.
    extern uint32_t g_virtual_time_ms;
    g_virtual_time_ms += 10;

    RenderFrameWithSnapshot();
}

// Native: menu / intro / results. No netplay state, run sim at wall-clock
// cadence. We don't replicate main_game_loop's frame-skip math — a simple
// one-tick-per-10ms gets the same perceived behavior without any of the
// prologue writes that broke determinism in battle.
static void RunNativeTick() {
    Hook_CheckGameModeTransition_Public();

    const bool skip_netplay = g_offline_mode || g_stress_mode;

    // Title screen: no netplay state, no sim-determinism requirement.
    // We used to block here on Netplay_IsConnected() to avoid the
    // "Connecting → snap-to-ready at CSS" UX flash, but it caused the
    // peer whose window came up first to sit frozen on the title
    // screen for the duration of the handshake gap (up to ~1s on
    // loopback even after the NAT-skip optimization, more on real
    // networks). Result: asymmetric CSS arrival.
    //
    // Tick the title screen freely; pump the control channel and
    // resend HELLO until handshake completes. The CSS barrier
    // (RunCssTick) and the BATTLE_READY barrier downstream still hold
    // peers in lockstep before any sim-relevant frame runs.
    if (!skip_netplay && !Netplay_IsConnected()) {
        ControlChannel_Poll();
        static uint32_t last_poll = 0;
        uint32_t now = GetTickCount();
        if (now - last_poll > 500) {
            ControlChannel_SendHello((uint8_t)g_player_index,
                                     fm2k::game_hash::Compute());
            last_poll = now;
        }
        // Fall through — let the title screen advance.
    }

    if (!skip_netplay) {
        Netplay_ProcessMenu();
    }

    if (original_process_game_inputs) original_process_game_inputs();
    if (original_update_game)         original_update_game();
    ParityRecorder::Capture();  // post-update snapshot for parity .pty

    // Same virtual-time alignment as RunCssTick — keep timeGetTime cadence
    // 1:1 with the spectator's per-pop bump on title/menu frames so any
    // FM2K menu animation code that reads timeGetTime sees identical deltas
    // on both sides.
    extern uint32_t g_virtual_time_ms;
    g_virtual_time_ms += 10;

    RenderFrameWithSnapshot();
}

// Spectator playback — pure input-replay model.
//
// CONTRACT: spectator's local FM2K only advances sim when it has a confirmed
// (p1, p2) input pair from the host for the next frame. Pop one → run one
// frame → render → wait for the next pair. That's it.
//
// Why this works: every change to FM2K's state is input-driven from a
// canonical default state at boot. Host records every confirmed (p1, p2)
// it returns from Hook_GetPlayerInput (title-screen auto-mash, CSS cursor
// moves, battle commands — the whole stream). Spectator drains the same
// stream → identical state by construction.
//
// Three regimes by queue depth:
//   QUEUE EMPTY                 → freeze, render last frame, wait
//   QUEUE >= LIVE_TARGET        → pop one, run one, render (live edge)
//   QUEUE > FF_ENTER (catch-up) → outer loop drops to 1 ms sleep so we
//                                 burn through backfill at ~1000 Hz
//
// Pre-JOIN_ACK: queue is empty, no sim runs, screen holds the boot snapshot.
// JOIN_ACK fires SendSessionBackfillTo from the host → queue fills with
// every confirmed frame from session start → spectator drains those at
// FF speed → catches up to live edge.
constexpr size_t SPECTATOR_LIVE_TARGET = 8;   // jitter buffer floor
constexpr size_t SPECTATOR_FF_ENTER    = 100; // ~1 sec of host frames queued
constexpr size_t SPECTATOR_FF_EXIT     = 16;  // hysteresis: 2x live target

static void RunSpectatorTick() {
    Hook_CheckGameModeTransition_Public();

    // Drain UDP — control channel (0xCC: SPEC_JOIN_ACK / HEARTBEAT) and
    // 0xCE INPUT_BATCH datagrams land on the same multiplex socket.
    ControlChannel_Poll();

    // Health: heartbeat send, silence-failover, daisy-chain reconnect.
    SpectatorNode_TickHealth();

    const size_t qd = SpectatorNode_PendingFrameCount();
    if (qd < SPECTATOR_LIVE_TARGET) {
        // Queue starved — hold the current rendered frame. Don't tick sim,
        // don't read inputs, don't run process_game_inputs/update_game.
        RenderFrameWithSnapshot();
        return;
    }

    // Pop the next confirmed (p1, p2). PopFrameInputs caches them into
    // pb_current_p1/p2 which Hook_GetPlayerInput reads as its single
    // source of truth this tick.
    uint16_t p1 = 0, p2 = 0;
    if (!SpectatorNode_PopFrameInputs(&p1, &p2)) {
        RenderFrameWithSnapshot();
        return;
    }

    // PER-FRAME alignment-trace log: capture RNG before PGI runs so we can
    // pair with [HOST-TRACE] on the host. If host's bf=N rng_pre matches
    // spectator's bf=N rng_pre, alignment is correct. If they diverge, the
    // leak is between frame N-1 post-UG and frame N pre-PGI (inter-frame).
    // If they match but rng_post diverges, leak is inside PGI+UG itself.
    static uint32_t s_spec_trace_bf = 0;
    static bool     s_spec_trace_in_battle = false;
    const uint32_t mode_pre = *(uint32_t*)FM2K::ADDR_GAME_MODE;
    const uint32_t rng_pre_pgi_spec = *(uint32_t*)0x41FB1C;
    if (mode_pre >= 3000 && mode_pre < 4000) {
        if (!s_spec_trace_in_battle) {
            s_spec_trace_in_battle = true;
            s_spec_trace_bf = 0;
        }
    } else {
        s_spec_trace_in_battle = false;
    }

    if (original_process_game_inputs) original_process_game_inputs();
    if (original_update_game)         original_update_game();
    ParityRecorder::Capture();

    if (s_spec_trace_in_battle && s_spec_trace_bf < 100) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "[SPEC-TRACE] bf=%u rng_pre=0x%08X rng_post=0x%08X "
            "p1=0x%03X p2=0x%03X",
            s_spec_trace_bf, rng_pre_pgi_spec,
            *(uint32_t*)0x41FB1C, p1, p2);
        s_spec_trace_bf++;
    }

    // Per-frame state fingerprint log for desync diagnosis. Counts pops
    // (= sim frames executed) since spectator boot. Logs once we're in
    // battle (mode 3000) at battle-frame multiples of 30 (~3x per sec).
    // Pairs with the host's matching log at netplay.cpp's AdvanceEvent
    // recording site so we can grep both .log files for the same
    // sim_frame and find the first divergent state by hand.
    {
        static uint32_t s_pop_count = 0;
        ++s_pop_count;
        static uint32_t s_battle_frame_at_entry = 0;
        static bool s_in_battle = false;
        const uint32_t mode_now = *(uint32_t*)FM2K::ADDR_GAME_MODE;
        if (mode_now >= 3000 && mode_now < 4000) {
            if (!s_in_battle) {
                s_in_battle = true;
                s_battle_frame_at_entry = s_pop_count;
            }
            const uint32_t bf = s_pop_count - s_battle_frame_at_entry;
            if ((bf % 30) == 0) {
                const uint32_t rng     = *(uint32_t*)0x41FB1C;
                const uint32_t buf_idx = *(uint32_t*)0x447EE0;
                const uint32_t p1_hp   = *(uint32_t*)0x4DFC85;
                const uint32_t p2_hp   = *(uint32_t*)0x4EDCC4;
                const uint32_t timer   = *(uint32_t*)0x470044;
                // Real battle positions in object pool. (Earlier this read
                // 0x470020 which is the CSS-selected character index
                // (g_p1_selected_char_idx) — useless for detecting
                // in-match drift since it's set during char-select and
                // doesn't change once battle starts.)
                constexpr uintptr_t POOL = 0x4701E0;
                constexpr size_t    SLOT = 382;
                const int32_t p1_x = *(int32_t*)(POOL + 0 * SLOT + 0x08);
                const int32_t p1_y = *(int32_t*)(POOL + 0 * SLOT + 0x0C);
                const int32_t p2_x = *(int32_t*)(POOL + 1 * SLOT + 0x08);
                const int32_t p2_y = *(int32_t*)(POOL + 1 * SLOT + 0x0C);
                const int32_t p1_script = *(int32_t*)(POOL + 0 * SLOT + 0x30);
                const int32_t p2_script = *(int32_t*)(POOL + 1 * SLOT + 0x30);
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[SPEC-FP] bf=%u (pop=%u) rng=0x%08X buf=%u "
                    "p1_hp=%u p2_hp=%u timer=%u "
                    "p1_pos=(%d,%d) p2_pos=(%d,%d) "
                    "p1_script=%d p2_script=%d "
                    "p1_in=0x%03X p2_in=0x%03X",
                    bf, s_pop_count, rng, buf_idx,
                    p1_hp, p2_hp, timer,
                    p1_x, p1_y, p2_x, p2_y,
                    p1_script, p2_script,
                    p1, p2);
            }
        } else {
            s_in_battle = false;
        }
    }

    // Advance the deterministic virtual clock (Hook_timeGetTime reads it).
    extern uint32_t g_virtual_time_ms;
    g_virtual_time_ms += 10;

    RenderFrameWithSnapshot();
}

// ============================================================================
// ENTRY POINT
// ============================================================================

// Per-frame engine work, callable from either the FM2K trampoline loop OR
// FM95's host-driven update_game callsite. Returns the LoopPhase actually
// executed so callers can decide whether the host's natural render call
// should still fire (NATIVE) or be skipped (BATTLE / CSS — we already
// drove update + render via AdvanceEvent / RenderFrameWithSnapshot).
//
// FM2K caller: TrampolineMainLoop wraps this with QPC-anchored pacing +
// SleepToTarget. Host main_game_loop is replaced wholesale.
//
// FM95 caller (Hook_UpdateGameState path — Phase 2 wiring): the host's
// natural WinMain pumps messages and runs its own timeGetTime catchup
// loop, so the FM95 path skips PumpMessages and SleepToTarget — calls
// this directly to drive one frame of GekkoNet save/load/advance dispatch.
LoopPhase TrampolineFrameTick() {
    LoopPhase phase = ClassifyPhase();
    switch (phase) {
        case LoopPhase::TRAMPOLINE_BATTLE:
            RunBattleTick();
            break;
        case LoopPhase::CSS:
            RunCssTick();
            break;
        case LoopPhase::NATIVE:
            RunNativeTick();
            break;
        case LoopPhase::SPECTATOR_PLAYBACK:
            RunSpectatorTick();
            break;
    }
    return phase;
}

BOOL TrampolineMainLoop() {
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Trampoline: main loop ACTIVE — GekkoNet-driven outer loop");

    // g_frame_time_ms @ 0x41E2F0 was set to 10 by the original main_game_loop
    // at 0x405AD3. We no longer run that code, so this global stays at
    // whatever the DLL-load-time default was (0 or garbage). Any game code
    // still reading it (CSS frame-skip math, debug BATTLE STATUS log) would
    // misbehave. Set it to the expected 100-fps target on trampoline entry.
    *(uint32_t*)0x41E2F0 = 10;

    // Parity recorder: if FM2K_PARITY_RECORD_PATH is set, open the .pty file
    // here (DllMain runs too early — the game globals aren't initialized yet,
    // and we want to record AFTER the warmup ticks below). MaybeAutoOpen
    // is a no-op if the env var isn't set, so this costs nothing in normal
    // play. Pairs with kgtengine's recorder for byte-level cross-engine
    // parity gates via tools/kgt_diff_pty.
    if (ParityRecorder::MaybeAutoOpen()) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "ParityRecorder: opened .pty output (FM2K_PARITY_RECORD_PATH)");
    }

    // One-time warmup — matches main_game_loop's 8× update_game_state at
    // 0x405AF3. Required to initialize game subsystems before any input.
    if (original_update_game) {
        for (int i = 0; i < 8; i++) original_update_game();
    }

    // Main loop. Runs forever until WM_QUIT.
    while (true) {
        // QPC (microsecond-resolution) anchor for the frame pacing sleep.
        // SDL_GetTicks()'s 1ms resolution was causing pacing to land at
        // 10.5-11 ms per frame on average due to SDL_Delay overshoot —
        // capping the loop at ~90 FPS. Using QPC + a busy-wait tail on
        // the final ~1 ms hits 100 Hz cleanly.
        uint64_t tick_start = SDL_GetPerformanceCounter();

        if (!PumpMessages()) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Trampoline: WM_QUIT — exiting");
            // Patch the .pty header's frame_count and close the file before
            // process teardown. DllMain's DLL_PROCESS_DETACH runs at a point
            // where CRT FILE* state may already be torn down, so close here.
            ParityRecorder::Close();
            return TRUE;
        }

        // Try installing the WndProc subclass each tick until it sticks.
        // Cheap: bails immediately once already-installed. The window
        // class "KGT2KGAME" comes up shortly after WinMain registers its
        // class, so this typically lands within the first few frames.
        // Subclass swallows Alt-tap menu activation + drives a
        // WM_TIMER inside DefWindowProc's modal loops so window drag /
        // system menu open don't pause our networking.
        FM2KWndProc::TryInstall();

        // Run one frame's worth of engine work. The body lives in
        // TrampolineFrameTick so the FM95 host-driven path can reuse it.
        LoopPhase phase = TrampolineFrameTick();

        // Phase-specific pacing.
        switch (phase) {
            case LoopPhase::TRAMPOLINE_BATTLE:
                // Pass the live frames_ahead so SleepToTarget applies the
                // 1.6% slowdown when the host is ahead of the client (or
                // vice versa). Without this the host runs rigid 10 ms
                // forever, the client trails by 7-8 frames permanently,
                // and FA stays pinned at ±8.5 — exactly the symptom we
                // saw. Stress / offline / pre-session paths all return
                // 0.0 from Netplay_GetFramesAhead so the slowdown is a
                // no-op there.
                SleepToTarget(tick_start, 10, Netplay_GetFramesAhead());
                break;
            case LoopPhase::CSS:
            case LoopPhase::NATIVE:
                SleepToTarget(tick_start, 10);
                break;
            case LoopPhase::SPECTATOR_PLAYBACK: {
                // FF with hysteresis. Live edge: queue oscillates around
                // LIVE_TARGET, sleep 10 ms (100 Hz). Backfill (>FF_ENTER):
                // sleep 1 ms (~1000 Hz) to drain the catch-up window.
                // Hysteresis: only EXIT fast-forward when queue drops
                // below FF_EXIT, so routine batch-arrival jitter doesn't
                // toggle pacing every burst.
                static bool s_fast_fwd = false;
                size_t qd = SpectatorNode_PendingFrameCount();
                if      (qd > SPECTATOR_FF_ENTER) s_fast_fwd = true;
                else if (qd < SPECTATOR_FF_EXIT)  s_fast_fwd = false;
                SleepToTarget(tick_start, s_fast_fwd ? 1 : 10);
                break;
            }
        }
    }
}
