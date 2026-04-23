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
#include "../netplay/netplay.h"
#include "../netplay/control_channel.h"
#include "../ui/shared_mem.h"
#include <SDL3/SDL_log.h>
#include <SDL3/SDL_timer.h>
#include <cstring>

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

static void RenderFrameWithSnapshot() {
    if (!original_render_game) return;

    // FPS counter + title-bar stats (hotkey probe too). Hook_RenderGame used
    // to do this; in trampoline mode render goes through here instead.
    Hook_RenderDiagnostics_Tick();

    // Save
    const bool protect = Netplay_IsActive();
    uint32_t saved_rng = 0;
    if (protect) {
        saved_rng = *(uint32_t*)FM2K::ADDR_RANDOM_SEED;
        memcpy(s_render_saved_object_pool,    (void*)FM2K::ADDR_OBJECT_POOL,       FM2K::SIZE_OBJECT_POOL);
        memcpy(s_render_saved_afterimage,     (void*)WaveCAddrs::AFTERIMAGE_POOL,  WaveCAddrs::AFTERIMAGE_POOL_SZ);
        memcpy(s_render_saved_input_tracking, (void*)0x447EE0,                     0xA0);
    }

    original_render_game();

    if (protect) {
        *(uint32_t*)FM2K::ADDR_RANDOM_SEED = saved_rng;
        memcpy((void*)FM2K::ADDR_OBJECT_POOL,      s_render_saved_object_pool,    FM2K::SIZE_OBJECT_POOL);
        memcpy((void*)WaveCAddrs::AFTERIMAGE_POOL, s_render_saved_afterimage,     WaveCAddrs::AFTERIMAGE_POOL_SZ);
        memcpy((void*)0x447EE0,                    s_render_saved_input_tracking, 0xA0);
    }

    // Launcher stats handshake (was in Hook_RenderGame).
    SharedMem_Update();
}

// ============================================================================
// UTILITIES
// ============================================================================

static LoopPhase ClassifyPhase() {
    uint32_t mode = *(uint32_t*)FM2K::ADDR_GAME_MODE;
    if (mode >= 3000 && mode < 4000) return LoopPhase::TRAMPOLINE_BATTLE;
    if (mode == 2000)                return LoopPhase::CSS;
    return LoopPhase::NATIVE;
}

// Non-blocking Windows message pump. Mirrors main_game_loop @ 0x405B0D.
// Returns false on WM_QUIT so the trampoline can exit cleanly.
static bool PumpMessages() {
    MSG msg;
    while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) return false;
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
static void SleepToTarget(uint64_t start_qpc, uint32_t target_ms) {
    const uint64_t freq = SDL_GetPerformanceFrequency();
    const uint64_t target_ticks = start_qpc + (freq * target_ms) / 1000;
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
        Netplay_HandleFrameTime();
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
        Netplay_HandleFrameTime();
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

    // Connection barrier — networked mode only. Block until both peers have
    // done the HELLO / ACK handshake. During this period neither peer has run
    // any sim yet.
    if (!skip_netplay && !Netplay_IsConnected()) {
        ControlChannel_Poll();
        static uint32_t last_poll = 0;
        uint32_t now = GetTickCount();
        if (now - last_poll > 500) {
            ControlChannel_SendHello((uint8_t)g_player_index, 0);
            last_poll = now;
        }
        return;
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
    RenderFrameWithSnapshot();
}

// Native: menu / intro / results. No netplay state, run sim at wall-clock
// cadence. We don't replicate main_game_loop's frame-skip math — a simple
// one-tick-per-10ms gets the same perceived behavior without any of the
// prologue writes that broke determinism in battle.
static void RunNativeTick() {
    Hook_CheckGameModeTransition_Public();

    if (!g_offline_mode && !g_stress_mode) {
        Netplay_ProcessMenu();
    }

    if (original_process_game_inputs) original_process_game_inputs();
    if (original_update_game)         original_update_game();
    RenderFrameWithSnapshot();
}

// ============================================================================
// ENTRY POINT
// ============================================================================

BOOL TrampolineMainLoop() {
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Trampoline: main loop ACTIVE — GekkoNet-driven outer loop");

    // g_frame_time_ms @ 0x41E2F0 was set to 10 by the original main_game_loop
    // at 0x405AD3. We no longer run that code, so this global stays at
    // whatever the DLL-load-time default was (0 or garbage). Any game code
    // still reading it (CSS frame-skip math, debug BATTLE STATUS log) would
    // misbehave. Set it to the expected 100-fps target on trampoline entry.
    *(uint32_t*)0x41E2F0 = 10;

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
            return TRUE;
        }

        switch (ClassifyPhase()) {
            case LoopPhase::TRAMPOLINE_BATTLE:
                RunBattleTick();
                // Netplay_HandleFrameTime only slows when AHEAD of peer; with
                // two localhost instances nobody is ever ahead so it becomes a
                // no-op and the loop spins at ~260 FPS (observed). Stress
                // mode has no peer at all. A hard 10 ms floor matches the
                // original main_game_loop @0x405AD3 target (100 Hz) and
                // doesn't fight the GekkoNet catch-up logic because that
                // only kicks in when the delta is POSITIVE.
                SleepToTarget(tick_start, 10);
                break;

            case LoopPhase::CSS:
                RunCssTick();
                SleepToTarget(tick_start, 10);
                break;

            case LoopPhase::NATIVE:
                RunNativeTick();
                SleepToTarget(tick_start, 10);
                break;
        }
    }
}
