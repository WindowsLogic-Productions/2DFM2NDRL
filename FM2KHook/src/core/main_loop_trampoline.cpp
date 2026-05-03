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
#include "../netplay/spectator_node.h"
#include "../ui/shared_mem.h"
#include "../parity/parity_recorder.h"
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
    uint32_t saved_rng = 0;
    if (protect) {
        saved_rng = *(uint32_t*)FM2K::ADDR_RANDOM_SEED;
        memcpy(s_render_saved_object_pool,    (void*)FM2K::ADDR_OBJECT_POOL,       FM2K::SIZE_OBJECT_POOL);
        // Afterimage save: two halves skipping the shake block so render's
        // decrement of the shake timer propagates into sim memory and the
        // KGT-scripted duration actually plays out.
        memcpy(s_render_saved_afterimage,
               (void*)WaveCAddrs::AFTERIMAGE_POOL,
               SHAKE_OFFSET_IN_AI);
        memcpy(s_render_saved_afterimage + SHAKE_OFFSET_IN_AI + SHAKE_EFFECTS_SZ,
               (void*)(WaveCAddrs::AFTERIMAGE_POOL + SHAKE_OFFSET_IN_AI + SHAKE_EFFECTS_SZ),
               WaveCAddrs::AFTERIMAGE_POOL_SZ - SHAKE_OFFSET_IN_AI - SHAKE_EFFECTS_SZ);
        memcpy(s_render_saved_input_tracking, (void*)0x447EE0,                     0xA0);
    }

    original_render_game();

    if (protect) {
        *(uint32_t*)FM2K::ADDR_RANDOM_SEED = saved_rng;
        memcpy((void*)FM2K::ADDR_OBJECT_POOL,      s_render_saved_object_pool,    FM2K::SIZE_OBJECT_POOL);
        // Afterimage restore: mirror of the split save — shake block in
        // live memory keeps whatever ProcessShakeEffect just wrote.
        memcpy((void*)WaveCAddrs::AFTERIMAGE_POOL,
               s_render_saved_afterimage,
               SHAKE_OFFSET_IN_AI);
        memcpy((void*)(WaveCAddrs::AFTERIMAGE_POOL + SHAKE_OFFSET_IN_AI + SHAKE_EFFECTS_SZ),
               s_render_saved_afterimage + SHAKE_OFFSET_IN_AI + SHAKE_EFFECTS_SZ,
               WaveCAddrs::AFTERIMAGE_POOL_SZ - SHAKE_OFFSET_IN_AI - SHAKE_EFFECTS_SZ);
        memcpy((void*)0x447EE0,                    s_render_saved_input_tracking, 0xA0);
    }

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
            // Score-cross trigger: pre_score >= 0 AND post_score < pre
            if (pre_score >= 0 && post_score < pre_score) {
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

    // Connection gate — networked mode only. Don't tick the game (no
    // process_game_inputs, no update_game, no render-snapshot mutation)
    // until both peers have HELLO/HELLO_ACK'd. Without this, both games
    // launch freely, the auto-mash drives them through title → CSS, and
    // the existing CSS-phase barrier ends up the fallback — which the
    // user sees as "stays in Connecting until CSS, then snaps."
    //
    // Mirror of RunCssTick's barrier. Both phases (NATIVE = title,
    // CSS = char select) block on Netplay_IsConnected. Battle mode has
    // its own BATTLE_READY barrier downstream.
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
                // 0x470020 which is the CSS slot index — useless for
                // detecting in-match drift.)
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

        switch (ClassifyPhase()) {
            case LoopPhase::TRAMPOLINE_BATTLE:
                RunBattleTick();
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
                RunCssTick();
                SleepToTarget(tick_start, 10);
                break;

            case LoopPhase::NATIVE:
                RunNativeTick();
                SleepToTarget(tick_start, 10);
                break;

            case LoopPhase::SPECTATOR_PLAYBACK: {
                RunSpectatorTick();
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
