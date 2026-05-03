// Simplified hooks - detect battle mode transitions, delegate to netplay
// Sync barrier: block game until both clients connected (CCCaster-style)
#include "hooks.h"
#include "globals.h"

#include <cstdlib>
#include <cstring>
#include "netplay.h"
#include "control_channel.h"
#include "imgui_overlay.h"
#include "shared_mem.h"
#include "savestate.h"  // CHAR_SLOT_BASE, CHAR_SLOT_SIZE (corrected by Wave C audit)
#include "../core/main_loop_trampoline.h"  // TrampolineMainLoop — owns the outer loop
#include "../audio/sound_rollback.h"        // Mike Z desired/actual sound layer
#include "../netplay/spectator_node.h"      // spectator playback queue accessors
#include "../ui/input_binder.h"             // FM2KInputBinder::Sample_Win32 + Bindings
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
    // Host: virtual clock during an active GekkoNet session so the per-peer
    // simulation evolves on a deterministic 10 ms/frame schedule.
    // Spectator: same — must return virtual clock once playback is driving
    // the sim, otherwise game code that consumes timeGetTime (animations,
    // particle pacing, etc.) sees wall-clock time and diverges from the
    // host's recorded execution every single frame. RunSpectatorTick is
    // responsible for bumping g_virtual_time_ms each successful advance.
    if (Netplay_IsActive() || SpectatorNode_IsPlayingBack()) {
        return g_virtual_time_ms;
    }
    return original_timeGetTime ? original_timeGetTime() : 0;
}

// ============================================================================
// CreateFile share-mode override
// ============================================================================
// FM2K opens character files (`.player`, etc.) with dwShareMode=0 — exclusive.
// When two instances launch from the same folder, the second hits
// ERROR_SHARING_VIOLATION ("Player Open error[…]"). Force-OR the shared-read
// flags so multiple readers can coexist. Writes are still serialized by
// the OS — we only widen sharing, never narrow it.
//
// Hooked at the kernel32 entry points; both A and W variants because old
// VC runtimes route through CreateFileA but newer code paths may use W.
using CreateFileA_t = HANDLE(WINAPI*)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES,
                                       DWORD, DWORD, HANDLE);
using CreateFileW_t = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES,
                                       DWORD, DWORD, HANDLE);
static CreateFileA_t original_CreateFileA = nullptr;
static CreateFileW_t original_CreateFileW = nullptr;

static constexpr DWORD kRelaxedShareMode =
    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;

static HANDLE WINAPI Hook_CreateFileA(LPCSTR name, DWORD access, DWORD share,
                                      LPSECURITY_ATTRIBUTES sa, DWORD disp,
                                      DWORD flags, HANDLE tmpl) {
    return original_CreateFileA(name, access, share | kRelaxedShareMode,
                                sa, disp, flags, tmpl);
}

static HANDLE WINAPI Hook_CreateFileW(LPCWSTR name, DWORD access, DWORD share,
                                      LPSECURITY_ATTRIBUTES sa, DWORD disp,
                                      DWORD flags, HANDLE tmpl) {
    return original_CreateFileW(name, access, share | kRelaxedShareMode,
                                sa, disp, flags, tmpl);
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

// =============================================================================
// SOCD (Simultaneous Opposite Cardinal Direction) cleaner
// =============================================================================
//
// Ported from CCCaster's filterSimulDirState() in
// targets/DllControllerUtils.hpp. Applied to the 11-bit FM2K input before
// returning from Hook_GetPlayerInput, AFTER the facing-direction swap so
// modes operate on game-internal F/B/U/D semantics rather than raw stick.
//
// FM2K bits: LEFT=0x001 RIGHT=0x002 UP=0x004 DOWN=0x008.
//
// Modes (FM2K_SOCD_MODE env var, default = 1):
//   0 — Default        L+R held → R wins  | U+D held → U wins
//   1 — L/R Cancel     L+R held → neutral | U+D held → U wins   <-- DEFAULT (Hitbox-style)
//   2 — U/D Cancel     L+R held → R wins  | U+D held → neutral
//   3 — Both Cancel    L+R held → neutral | U+D held → neutral
//   4 — Up Bias        L+R held → R wins  | U+D held → U wins   (= 0 today)
//   5 — Hitbox+Up      L+R held → neutral | U+D held → U wins   (= 1 today)
//
// Modes 4/5 are kept for CCCaster compat; behavior currently matches 0/1
// because FM2K already drops DOWN on U+D (no separate "down-wins" branch
// to disambiguate). If we want a DOWN-priority crouch-bias mode later
// it'd be a 7th value.
static int g_socd_mode_runtime = -1;  // -1 = uninitialized; runtime override of env var.

static int Hook_GetSOCDMode() {
    if (g_socd_mode_runtime < 0) {
        const char* env = std::getenv("FM2K_SOCD_MODE");
        g_socd_mode_runtime = 1;  // tournament default
        if (env && env[0] >= '0' && env[0] <= '5' && env[1] == '\0') {
            g_socd_mode_runtime = env[0] - '0';
        }
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "SOCD: mode=%d (FM2K_SOCD_MODE override='%s')",
            g_socd_mode_runtime, env ? env : "<unset>");
    }
    return g_socd_mode_runtime;
}

// Set the SOCD mode at runtime. Used by HOST_CONFIG receiver to adopt
// host's mode, and by future settings UI. Mode must be 0..5; out-of-range
// values are ignored.
extern "C" void Hook_SetSOCDMode(int mode) {
    if (mode < 0 || mode > 5) return;
    if (g_socd_mode_runtime != mode) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "SOCD: mode change %d -> %d", g_socd_mode_runtime, mode);
        g_socd_mode_runtime = mode;
    }
}

// Public accessor for the SOCD mode — needed by netplay.cpp's HOST_CONFIG
// broadcaster which can't reach the static.
extern "C" int Hook_GetSOCDModePublic() {
    return Hook_GetSOCDMode();
}

static inline uint16_t Hook_ApplySOCD(uint16_t input) {
    constexpr uint16_t LEFT  = 0x001;
    constexpr uint16_t RIGHT = 0x002;
    constexpr uint16_t UP    = 0x004;
    constexpr uint16_t DOWN  = 0x008;
    const int mode = Hook_GetSOCDMode();

    if ((input & (LEFT | RIGHT)) == (LEFT | RIGHT)) {
        if (mode & 1) {
            input &= ~(LEFT | RIGHT);  // cancel both
        } else {
            input &= ~LEFT;            // R wins
        }
    }
    if ((input & (UP | DOWN)) == (UP | DOWN)) {
        if (mode & 2) {
            input &= ~(UP | DOWN);     // cancel both
        } else {
            input &= ~DOWN;            // U wins
        }
    }
    return input;
}

// Pending input pair captured by Hook_GetPlayerInput's capture_and_return,
// at file scope so Hook_FlushPendingCapture can drain it from outside the
// hook (specifically at CSS→battle transition, before the battle session
// gates capture_and_return out). Without this drain the trailing CSS frame
// — the one carrying the confirm input that flips game_mode 2000→3000 —
// stays trapped in g_capture_p[] and the spectator never receives it.
static uint16_t g_capture_p[2] = {0, 0};
static uint32_t g_capture_recorded_idx = UINT32_MAX;

// Flush the pending (p1, p2) capture to the spectator stream and clear
// the pending state so the next capture starts from scratch. Idempotent:
// safe to call when nothing is pending (no-op).
extern "C" void Hook_FlushPendingCapture() {
    if (g_capture_recorded_idx == UINT32_MAX) return;
    SpectatorNode_OnFrameConfirmed(g_capture_p[0], g_capture_p[1]);
    g_capture_p[0] = g_capture_p[1] = 0;
    g_capture_recorded_idx = UINT32_MAX;
}

// Battle-transition diagnostic. Activates a frame-by-frame state-dump
// window centered on every game_mode transition so we can compare what
// games like StudioS Fighters / SFZ are doing differently from
// WonderfulWorld at battle entry. The window is a few hundred frames so
// we catch the bail-back-to-CSS pattern (typical: ~140 frames in
// "battle" before reverting).
static int  g_battle_diag_frames_remaining = 0;  // counts down per call
static uint32_t g_battle_diag_frame_idx = 0;     // counter for log lines

extern "C" void Hook_BattleDiag_TickIfActive() {
    if (g_battle_diag_frames_remaining <= 0) return;

    uint32_t mode  = *(uint32_t*)FM2K::ADDR_GAME_MODE;
    uint32_t rng   = *(uint32_t*)FM2K::ADDR_RANDOM_SEED;
    // g_input_buffer_index @ 0x447EE0
    uint32_t buf_idx = *(uint32_t*)0x447EE0;
    // g_last_frame_time @ 0x447DD4
    uint32_t lft   = *(uint32_t*)0x447DD4;
    // g_frame_time_ms @ 0x41E2F0
    uint32_t fr_ms = *(uint32_t*)0x41E2F0;

    // Char slot active flags (0xDF41 from each slot base) + state flags
    // (0x7CA6) — battle init reads these to decide whether to stay in
    // battle. Slots 0 and 1 are P1/P2.
    constexpr uintptr_t SLOT_BASE = 0x4D1D90;
    constexpr size_t    SLOT_SZ   = 57407;
    uint8_t s0_active = *(uint8_t*)(SLOT_BASE + 0xDF41);
    uint8_t s1_active = *(uint8_t*)(SLOT_BASE + SLOT_SZ + 0xDF41);
    uint8_t s0_flags  = *(uint8_t*)(SLOT_BASE + 0x7CA6);
    uint8_t s1_flags  = *(uint8_t*)(SLOT_BASE + SLOT_SZ + 0x7CA6);
    // The two main_game_loop prologue writes — slot+0xDF75, slot+0xDF79
    uint32_t s0_dF75 = *(uint32_t*)(SLOT_BASE + 0xDF75);
    uint32_t s0_dF79 = *(uint32_t*)(SLOT_BASE + 0xDF79);

    // Round timer + HPs (real addresses from IDA: g_p1_hp = 0x4DFC85,
    // p2 = +0xDF40 stride. The ones at 0x47010C / 0x47030C are
    // g_demo_mode_player_id, NOT player HP. Wrong addresses earlier
    // showed hp=0/0 always which masked an important signal.)
    uint32_t round_timer = *(uint32_t*)0x470060;
    constexpr uintptr_t HP_BASE = 0x4DFC85;
    constexpr uintptr_t HP_STRIDE = 57407;
    uint32_t p1_hp = *(uint32_t*)(HP_BASE);
    uint32_t p2_hp = *(uint32_t*)(HP_BASE + HP_STRIDE);

    // vs_round_function state machine — these decide whether battle
    // proceeds or bails back to CSS:
    //   g_score_value @ 0x470050 — countdown timer; case 200 decrements
    //     each frame, reaching -1 routes to case 300 (round end → CSS).
    //     If battle init sets this to a small/negative value, battle
    //     immediately bails. Almost certainly StudioS's bug signature.
    //   round_state_field — *(g_object_data_ptr + 338) — the case
    //     selector itself (1 → 100 → 110 → 200 → 300 etc).
    //   g_round_end_flag @ 0x424718 — case 200 also bails to 300 if
    //     this is set.
    //   g_round_state @ 0x47004C — top-level round phase (0/1/2).
    //   g_game_mode_flag @ 0x470058 — VS / Story / Team selector that
    //     changes which init-path runs in case 1.
    //   g_round_limit @ 0x470048 — number of rounds to play.
    //   g_game_timer @ 0x470044 — match clock.
    int32_t score_value     = *(int32_t*)0x470050;
    uint32_t round_end_flag = *(uint32_t*)0x424718;
    uint32_t round_state    = *(uint32_t*)0x47004C;
    uint8_t  game_mode_flag = *(uint8_t*)0x470058;
    uint32_t round_limit    = *(uint32_t*)0x470048;
    uint32_t game_timer     = *(uint32_t*)0x470044;
    // LABEL_201 in vs_round_function: bail to CSS when these hit round_limit
    // in Story/Team mode. Both incremented by case 410/420/430/510/520/530.
    // g_match_phase @ 0x4DFC6D (in P1 char slot), g_round_sub_state @ 0x4EDCAC
    uint32_t match_phase    = *(uint32_t*)0x4DFC6D;
    uint32_t round_sub      = *(uint32_t*)0x4EDCAC;
    // g_round_timer_counter @ 0x424F00 — used by game_state_manager's
    // CSS-confirm timer (counts to 100 then transitions to battle)
    uint32_t round_tick_ctr = *(uint32_t*)0x424F00;
    uint32_t obj0_state338  = 0;
    int32_t  obj0_state342  = 0;  // case-110/112 countdown timer
    void* obj_data_ptr = *(void**)0x4CFA00;
    if (obj_data_ptr) {
        obj0_state338 = *(uint32_t*)((uint8_t*)obj_data_ptr + 338);
        obj0_state342 = *(int32_t*)((uint8_t*)obj_data_ptr + 342);
    }
    // lParam @ 0x430114 — case-100 Story-mode score = 100*lParam - 1
    uint32_t lparam_save = *(uint32_t*)0x430114;
    // Object pool active type-4 fighter count — case-200 Story-mode bails
    // when this < 2. Replicate the EXACT walk from vs_round_function:
    //   if (entry.type == 4 &&
    //       entry[+4] == 0 &&
    //       hp[char_slot[entry[+342]]] != 0) ++count;
    // Pool: 0x4701E0, 1024 entries × 382 bytes. Per-entry +342 holds
    // the char-slot index this object belongs to; HP at HP_BASE +
    // slot_idx * 57407.
    // Re-derived from the actual asm at 0x408E90:
    //   pool base = 0x4701E0 (g_object_pool.payload)
    //   eax starts at base + 0x156, iterates +0x17E (382) per entry.
    //   cmp [eax-0x156], 4   → entry+0   = type     (uint32)
    //   cmp [eax+4],     0   → entry+0x15A (=346) = alive_flag (uint32, 0=alive)
    //   mov ebp, [eax]       → entry+0x156 (=342) = slot_idx (uint32)
    //   * 57407, +g_p1_hp, hp != 0
    int active_type4 = 0;
    int total_type4 = 0;
    {
        const uint8_t* pool = (const uint8_t*)0x4701E0;
        for (int i = 0; i < 1024; i++) {
            const uint8_t* e = pool + i * 382;
            uint32_t type = *(const uint32_t*)(e + 0);
            if (type != 4) continue;
            total_type4++;
            uint32_t alive_flag = *(const uint32_t*)(e + 346);
            if (alive_flag != 0) continue;
            uint32_t slot_idx = *(const uint32_t*)(e + 342);
            if (slot_idx >= 8) continue;
            uint32_t slot_hp = *(const uint32_t*)(HP_BASE + slot_idx * HP_STRIDE);
            if (slot_hp == 0) continue;
            active_type4++;
        }
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "[BD %4u] mode=%u rng=0x%08X "
        "mp=%u rs=%u rtc=%u "         // LABEL_201 trigger: mp/rs hitting rlim
        "rs338=%u score=%d t4=%d/%d "
        "ref=%u gmf=%u rlim=%u gt=%u "
        "hp=%u/%u",
        g_battle_diag_frame_idx, mode, rng,
        match_phase, round_sub, round_tick_ctr,
        obj0_state338, score_value, active_type4, total_type4,
        round_end_flag, game_mode_flag, round_limit, game_timer,
        p1_hp, p2_hp);
    (void)fr_ms; (void)s0_dF75; (void)s0_dF79;
    (void)buf_idx; (void)lft; (void)round_timer;
    (void)obj0_state342; (void)round_state; (void)lparam_save;
    (void)s0_active; (void)s0_flags; (void)s1_active; (void)s1_flags;

    g_battle_diag_frame_idx++;
    g_battle_diag_frames_remaining--;
}

static void CheckGameModeTransition() {
    uint32_t current_mode = *(uint32_t*)FM2K::ADDR_GAME_MODE;

    if (current_mode != g_last_game_mode) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "Hooks: game_mode changed: %u -> %u", g_last_game_mode, current_mode);

        // Whenever the game crosses any CSS↔battle boundary, kick off a
        // 300-frame (3 second @ 100 Hz) state dump so we can diff
        // working games (WonderfulWorld) vs broken ones (SFZ, StudioS
        // Fighters) at battle entry. Reset the per-window counter.
        bool boundary =
            (IsCSSMode(g_last_game_mode)    && IsBattleMode(current_mode)) ||
            (IsBattleMode(g_last_game_mode) && IsCSSMode(current_mode));
        if (boundary) {
            g_battle_diag_frames_remaining = 300;
            g_battle_diag_frame_idx = 0;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                ">>> BATTLE-DIAG window OPEN (300 frames) <<<");
        }

        // Spectator: never run player-state-machine code on transitions
        // (no BATTLE_ENTERING, no GekkoNet session, etc.). But we DO need
        // to mirror the deterministic RNG reseeds the host performs at
        // both 2000→3000 (Netplay_StartBattle) and at every CSS-SYNC
        // (Netplay_ProcessCSS). Without this mirroring the spectator's RNG
        // diverges from the host the moment battle starts: spectator's
        // RNG is whatever CSS evolved to, host's was just force-written
        // back to 0x12345678. One frame is enough to desync.
        if (g_spectator_mode) {
            constexpr uint32_t HOST_FIXED_SEED = 0x12345678;
            bool was_battle = IsBattleMode(g_last_game_mode);
            bool is_battle  = IsBattleMode(current_mode);
            bool was_css    = IsCSSMode(g_last_game_mode);
            bool is_css     = IsCSSMode(current_mode);

            if (!was_battle && is_battle) {
                // CSS → battle. Mirror Netplay_StartBattle's seed pin AND
                // SaveState_Save's first-call "initial sync" reset of input
                // tracking state. Host hits this reset on its first battle
                // SaveEvent (savestate.cpp:223-237). Spectator never runs
                // Save/Load, so without this mirror the spectator carries
                // its CSS-evolved input_buffer_index (~600) into battle while
                // the host's is reset to 0 — every input_history[idx-N]
                // read (combo windows, charge inputs) returns different
                // values on the two sides. Permanent battle desync.
                *(uint32_t*)FM2K::ADDR_RANDOM_SEED = HOST_FIXED_SEED;
                *(uint32_t*)0x447EE0 = 0;            // g_input_buffer_index
                *(uint32_t*)0x4456FC = 0;            // render frame counter
                memset((void*)0x447F00, 0, 0x20);    // g_prev_input_state
                memset((void*)0x447F40, 0, 0x20);    // g_processed_input
                memset((void*)0x447F60, 0, 0x20);    // g_input_changes
                memset((void*)0x4280D8, 0, 0x2008);  // input_history rings (P1+P2)

                // CRITICAL: initialize the SoundRollback channel table so the
                // dispatch_script_sound hook's Mike-Z dedup logic returns the
                // same suppress/pass decisions on spectator as on host. Without
                // this, host suppresses ~thousands of redundant sound dispatches
                // (RecordDesired returns true → original dispatcher skipped),
                // but spectator's table is empty (RecordDesired returns false)
                // → spectator runs original_dispatch_script_sound for EVERY
                // call. Whatever game_rand calls live inside FM2K's
                // dispatch_script_sound (pitch/pan jitter most likely) then
                // run on spectator but not host → RNG drifts on every frame
                // with sound activity. Verified via paired [HOST-FP]/[SPEC-FP]
                // logs: bf=0..270 RNG matched, bf=300+ diverged after first
                // sound dispatch, with all other state (HP/timer/pos/input)
                // still matching = isolated to RNG.
                SoundRollback::Init();

                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Spectator: battle entry — pinned RNG to 0x%08X, "
                            "reset input_buf_idx + edge state + history rings, "
                            "SoundRollback::Init",
                            HOST_FIXED_SEED);
            } else if (!was_css && is_css) {
                // Anything → CSS (rematch). Mirror Netplay_ProcessCSS's
                // CSS-SYNC reseed so the next match's CSS evolves the
                // RNG identically to the host's.
                *(uint32_t*)FM2K::ADDR_RANDOM_SEED = HOST_FIXED_SEED;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Spectator: CSS entry — pinned RNG to 0x%08X",
                            HOST_FIXED_SEED);
                // (Save/load mirror tear-down removed — see battle-entry comment.)
            }

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

// ============================================================================
// HOOK IMPLEMENTATIONS
// ============================================================================

// Hook: GetPlayerInput
// CSS: return synced input from control channel
// Battle: return synchronized input from GekkoNet with facing adjustment
int __cdecl Hook_GetPlayerInput(int player_id, int input_type) {
    uint32_t game_mode = *(uint32_t*)FM2K::ADDR_GAME_MODE;
    // (Removed FM2K_INPUT_DUMP block — calling original_get_player_input
    // for diagnostic purposes had a SIDE EFFECT: it ran FM2K's keyboard-
    // poll → .ini-binding pipeline, which leaked .ini-bound key presses
    // into FM2K's internal edge-detection state EVEN WHEN our binder was
    // supposed to be the sole input source. Symptom was that custom
    // binder binds AND game.ini binds both fired simultaneously. Bit
    // mappings are confirmed (A=0x010 .. F=0x200, START=0x400 by
    // induction); diagnostic served its purpose, now it's gone.)

    // Single capture-and-return funnel. EVERY return path goes through
    // this lambda so we record the (p1, p2) input pair into the host's
    // session_history every time the input-buffer-index ticks (= one
    // full FM2K frame). Recording starts at FM2K boot — captures title-
    // screen no-ops, auto-mash, pre-rendezvous CSS, post-rendezvous
    // GekkoNet-merged inputs, battle frames — one canonical log spanning
    // the entire connection. Late-joining spectators get this whole log
    // via SendSessionBackfillTo and replay deterministically from frame 0.
    //
    // Spectators (g_spectator_mode) DO NOT record — they consume from
    // pb_queue, not produce. Stress / offline DO record but the log is
    // never sent (no subscribers).
    // capture_and_return: every returned input from this hook on the HOST
    // side is the source of truth for the spectator stream. We pair the
    // current frame's (p1, p2) returns and emit them via
    // SpectatorNode_OnFrameConfirmed at the moment the frame boundary
    // ticks (g_input_buffer_index advances). The spectator drives its
    // local FM2K from that exact same input pair, popped one per sim
    // tick. Because every change to FM2K's state is input-driven from a
    // canonical default state at boot, replaying the input log in order
    // produces a 1:1 sim — title-screen auto-mash, CSS cursor moves,
    // battle commands all included.
    //
    // The pending pair lives at file scope (g_capture_*) instead of
    // lambda statics so Hook_FlushPendingCapture() can drain the trailing
    // CSS frame at CSS→battle transition — without that flush, the LAST
    // CSS frame's pair (the one whose confirm input flips game_mode) sits
    // in g_capture_p[] forever because the next frame's capture is gated
    // out by Netplay_IsActive once the battle session starts. Spectator
    // never sees that frame, never flips game_mode, desync.
    //
    // SKIP CONDITIONS:
    //   * g_spectator_mode: spectator only consumes, never produces.
    //   * Netplay_IsActive() (battle): GekkoNet runahead+rollback fires
    //       this hook ~5x per real frame. Battle confirmed-frame capture
    //       is gated in netplay.cpp's AdvanceEvent handler instead.
    auto capture_and_return = [player_id](int result) -> int {
        if (g_spectator_mode) return result;
        if (Netplay_IsActive()) return result;
        const uint32_t cur_idx = *(uint32_t*)0x447EE0;
        if (cur_idx != g_capture_recorded_idx) {
            if (g_capture_recorded_idx != UINT32_MAX) {
                SpectatorNode_OnFrameConfirmed(g_capture_p[0], g_capture_p[1]);
            }
            g_capture_recorded_idx = cur_idx;
        }
        g_capture_p[player_id & 1] = (uint16_t)result;
        return result;
    };

    // Title-screen menu-cursor write. Must fire on BOTH host AND spectator —
    // it's a state side-effect of the auto-title-skip protocol, not an
    // input. session_history only records returned input values, so a
    // spectator replaying host's recorded auto-mash button-A pulses would
    // navigate from g_menu_selection=0 (default = "VS CPU"/first option)
    // and end up in the wrong scene tree. We force g_menu_selection=1
    // ("VS Player") on every node so the same recorded input pattern
    // resolves to the same menu transitions everywhere.
    {
        static const char* env_skip = std::getenv("FM2K_AUTO_TITLE_SKIP");
        const bool auto_skip = !(env_skip && std::strcmp(env_skip, "0") == 0);
        static bool s_cursor_set_global = false;
        if (auto_skip && !s_cursor_set_global && game_mode == 1000) {
            *(uint32_t*)0x424780 = 1;  // g_menu_selection = VS Player
            s_cursor_set_global = true;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "TitleMenuCursor: pre-set g_menu_selection=1 (host or spectator)");
        }
    }

    // Spectator process — SINGLE source of truth: the popped input pair
    // (host's recorded p1/p2 for the current sim frame). No keyboard read,
    // no auto-mash, no fall-through to anything else. The hook is only
    // ever called from inside RunSpectatorTick → original_process_game_inputs,
    // which we only invoke after popping a frame from the queue.
    //
    // Battle-mode facing fix mirrors host's branch — same 11-bit input,
    // same left/right swap when char_active && !state_flag_8.
    if (g_spectator_mode) {
        uint16_t input = (player_id == 0)
            ? SpectatorNode_GetCurrentP1Input()
            : SpectatorNode_GetCurrentP2Input();

        constexpr size_t CHAR_ACTIVE_FLAG_OFFSET = 0xDF41;
        constexpr size_t CHAR_STATE_FLAGS_OFFSET = 0x7CA6;
        bool facing_reversed = true;
        if (game_mode >= 3000 && game_mode < 4000) {
            uintptr_t slot_base = CHAR_SLOT_BASE + (size_t)input_type * CHAR_SLOT_SIZE;
            uint8_t char_active = *(uint8_t*)(slot_base + CHAR_ACTIVE_FLAG_OFFSET);
            if (char_active != 0) {
                uint8_t char_flags = *(uint8_t*)(slot_base + CHAR_STATE_FLAGS_OFFSET);
                if ((char_flags & 8) == 0) facing_reversed = false;
            }
        }
        if (!facing_reversed) {
            uint16_t left_bit  = (input & 0x001);
            uint16_t right_bit = (input & 0x002);
            input = (input & ~0x003) | (left_bit << 1) | (right_bit >> 1);
        }
        input = Hook_ApplySOCD(input);
        return (int)input;  // spectator path doesn't record
    }

    // Auto-mash through title screen → menu → CSS. Default ON unless
    // FM2K_AUTO_TITLE_SKIP=0. With the boot-to-title patch (push 0x0C)
    // the game starts in title_screen_manager (g_game_mode=1000). We
    // pre-set g_menu_selection=1 (VS Player is always index 1 in
    // g_titleMenu_modeList[]) and pulse button A (bit 4 = 0x010) on
    // alternate frames until g_game_mode flips to 2000 (CSS reached).
    //
    // Critical: alternate per-FRAME, not per-CALL. get_player_input
    // is called twice per frame (once for each player) — if we
    // increment a counter on every call and use parity, P1 and P2
    // get opposite values and neither sees a rising edge after frame
    // 1. Use g_input_buffer_index @ 0x447EE0 instead — it ticks once
    // per frame from the game's own input pipeline, so both players'
    // calls in the same frame return the same value.
    {
        static const char* env_skip = std::getenv("FM2K_AUTO_TITLE_SKIP");
        const bool auto_skip = !(env_skip && std::strcmp(env_skip, "0") == 0);
        static bool s_done = false;
        static bool s_cursor_set = false;
        static uint32_t s_started_frame = 0;
        if (auto_skip && !s_done) {
            if (game_mode >= 2000) {
                s_done = true;
                uint32_t now = *(uint32_t*)0x447EE0;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "AutoTitleSkip: reached g_game_mode=%u, handing input "
                    "back to user (took %u input-buffer ticks)",
                    game_mode, now - s_started_frame);
            } else if (game_mode == 1000) {
                if (!s_cursor_set) {
                    // Menu cursor itself is now written in the hoisted
                    // block at the top of this function (runs on host AND
                    // spectator). Here we just record the start-frame for
                    // the auto-mash duration log and flip s_cursor_set.
                    s_cursor_set = true;
                    s_started_frame = *(uint32_t*)0x447EE0;
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "AutoTitleSkip: starting button-A mash from input_buf=%u",
                        s_started_frame);
                }
                // 4-tick pattern (0x010, 0x010, 0, 0) ensures a rising
                // edge every 4 frames: prev=0 → cur=0x010 fires the
                // edge detector. Holding the button for 2 frames lets
                // the title menu's "any-button" check (& 0x3F0) sample
                // a stable value before the release. ~25 Hz of
                // confirms — fast enough to march title → menu → CSS
                // in ~16 frames, slow enough not to skip past states
                // the menu hasn't latched yet.
                uint32_t buf_idx = *(uint32_t*)0x447EE0;
                return capture_and_return(((buf_idx >> 1) & 1) ? 0 : 0x010);
            }
        }
    }


    // FM2K_PARITY_AUTOPLAY: drive title→CSS→battle via a deterministic
    // input sequence. Short-circuits the netplay/CSS/spectator branches
    // — autoplay owns input completely. Phase-aware (uses live
    // game_mode to pick inputs per CSS section). The game's CSS state
    // machine (game_state_manager @ 0x406FC0) reads:
    //   g_processed_input[i] for direction (bits 0..3 = L/R/U/D)
    //   g_input_changes[i] & 0x3F0 for attack-button rising-edge (confirm)
    // The original get_player_input returns RAW input; the game's
    // process_game_inputs converts raw → processed_input and computes
    // changes from prev frame. So returning a clean rising-edge of
    // bit 0x10 (button A) on a CSS frame triggers the confirm path.
    {
        static const char* env_autoplay = std::getenv("FM2K_PARITY_AUTOPLAY");
        if (env_autoplay && std::strcmp(env_autoplay, "1") == 0) {
            static uint32_t s_call_count = 0;
            const uint32_t call = s_call_count++;
            const uint32_t frame = call / 2u;
            const uint16_t Z = 0x010u;
            uint16_t out = 0u;

            // WW game_mode states (per vs_round_function @ 0x4086A0 +
            // game_state_manager @ 0x406FC0):
            //   0     = boot/title intro
            //   2000  = CSS active
            //   3000  = battle active
            // No 4000+ values; battle stays at 3000 throughout match.
            // For clean idle-parity capture, send Z every 30 frames
            // until we reach battle (advances title prompts + confirms
            // both CSS slots) and then idle (out=0) so the captured
            // frames show pure-idle physics with no synthetic attack
            // inputs polluting the comparison against kgt's idle run.
            if (game_mode < 3000u) {
                if ((frame % 30u) == 0u) out = Z;
            }
            // mode >= 3000: idle (out stays 0)

            /* Diagnostic: log on first hit + every game_mode change AND
             * every 120 frames once we hit battle (game_mode >= 3000) so
             * we can see HP/active-flag populating after CSS exit.
             * HP source corrected: g_p1_hp=0x4DFC85, g_p2_hp=0x4EDCC4
             * (verified via IDA xref of vs_round_function @ 0x4086A0). */
            static uint32_t s_last_logged_mode = 0xFFFFFFFFu;
            static uint32_t s_last_periodic = 0u;
            const bool mode_changed = (game_mode != s_last_logged_mode);
            const bool periodic = (game_mode >= 3000u) &&
                                  ((frame - s_last_periodic) >= 120u);
            if (mode_changed || periodic) {
                const uint32_t p1_action = *(uint32_t*)0x47019Cu;
                const uint32_t p2_action = *(uint32_t*)0x4701A0u;
                const int32_t  p1_pos    = *(int32_t*) 0x470020u;
                const int32_t  p2_pos    = *(int32_t*) 0x470024u;
                const uint32_t timer     = *(uint32_t*)0x424F00u;
                const uint8_t  char0_act = *(uint8_t*) 0x4DFCD1u;
                const uint8_t  char1_act = *(uint8_t*)(0x4DFCD1u + 57407u);
                const uint32_t p1_hp     = *(uint32_t*)0x4DFC85u;
                const uint32_t p1_max_hp = *(uint32_t*)0x4DFC91u;
                const uint32_t p2_hp     = *(uint32_t*)0x4EDCC4u;
                const uint32_t p2_max_hp = *(uint32_t*)0x4EDCD0u;
                const int32_t  cam_x     = *(int32_t*) 0x447F2Cu;
                const int32_t  cam_y     = *(int32_t*) 0x447F30u;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[AUTOPLAY] frame=%u mode=%u p1act=%u p2act=%u "
                    "p1pos=%d p2pos=%d ctimer=%u "
                    "char0_act=%u char1_act=%u "
                    "p1_hp=%u/%u p2_hp=%u/%u cam=(%d,%d) out=0x%03X",
                    frame, game_mode, p1_action, p2_action,
                    p1_pos, p2_pos, timer,
                    char0_act, char1_act,
                    p1_hp, p1_max_hp, p2_hp, p2_max_hp,
                    cam_x, cam_y, out);
                s_last_logged_mode = game_mode;
                if (periodic) s_last_periodic = frame;
            }
            return capture_and_return((int)out);
        }
    }

    // (Spectator branch lifted to the top of the function — runs before
    // auto-mash so spectator's local FM2K replays host's recorded inputs
    // instead of generating its own auto-mash sequence.)

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
        input = Hook_ApplySOCD(input);

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

        return capture_and_return((int)input);
    }

    // CSS mode with connection - return CSS input from control channel
    if (IsCSSMode(game_mode) && Netplay_IsConnected()) {
        uint16_t input = Netplay_GetCSSInput(player_id);
        input = Hook_ApplySOCD(input);
        return capture_and_return((int)input);
    }

    // Offline or menu: use the binder if active, else fall through to FM2K's
    // own get_player_input. Same gating as Input_CaptureLocal — Init() is
    // idempotent and resolves to %APPDATA%\FM2K_Rollback\fm2k_inputs.ini
    // (matching the launcher's save path) so launcher-bound keys / pads
    // drive offline play here, GekkoNet-online play through Input_CaptureLocal.
    {
        static int  s_last_check_tick = 0;
        static bool s_binder_active   = false;
        const int now_tick = (int)GetTickCount();
        if ((now_tick - s_last_check_tick) > 1000 || s_last_check_tick == 0) {
            s_last_check_tick = now_tick;
            FM2KInputBinder::Init();
            const auto& pb = FM2KInputBinder::Bindings(0);
            s_binder_active = false;
            for (const auto& b : pb.bits) {
                if (b.source != FM2KInputBinder::Binding::Source::NONE) {
                    s_binder_active = true;
                    break;
                }
            }
        }
        if (s_binder_active) {
            // input_type is the character slot (same convention the battle /
            // spectator branches above use to compute slot_base for facing-fix).
            // 0 = P1 character → P1 bindings, 1 = P2 character → P2 bindings.
            // Without this distinction both players get the SAME input from
            // P1's bindings — the bug we just fixed.
            const int slot = (input_type & 1);
            uint16_t bound = FM2KInputBinder::Sample_Win32(slot);
            // Apply the same battle facing-fix the original_get_player_input
            // path applies (so offline matches behave the same way as the
            // game's own input flow).
            constexpr size_t CHAR_ACTIVE_FLAG_OFFSET_OFL = 0xDF41;
            constexpr size_t CHAR_STATE_FLAGS_OFFSET_OFL = 0x7CA6;
            bool facing_reversed = true;
            if (game_mode >= 3000 && game_mode < 4000) {
                uintptr_t slot_base = CHAR_SLOT_BASE + (size_t)input_type * CHAR_SLOT_SIZE;
                uint8_t char_active = *(uint8_t*)(slot_base + CHAR_ACTIVE_FLAG_OFFSET_OFL);
                if (char_active != 0) {
                    uint8_t char_flags = *(uint8_t*)(slot_base + CHAR_STATE_FLAGS_OFFSET_OFL);
                    if ((char_flags & 8) == 0) facing_reversed = false;
                }
            }
            if (!facing_reversed) {
                uint16_t left_bit  = (bound & 0x001);
                uint16_t right_bit = (bound & 0x002);
                bound = (bound & ~0x003) | (left_bit << 1) | (right_bit >> 1);
            }
            bound = Hook_ApplySOCD(bound);
            return capture_and_return((int)bound);
        }
    }

    // No binder config — vanilla FM2K input path.
    int orig = original_get_player_input
        ? original_get_player_input(player_id, input_type)
        : 0;

    orig = (int)Hook_ApplySOCD((uint16_t)orig);
    return capture_and_return(orig);
}

// Hook: UpdateGameState
// Main control point - check transitions, process netplay
int __cdecl Hook_UpdateGameState() {
    uint32_t game_mode = *(uint32_t*)FM2K::ADDR_GAME_MODE;

    // Offline mode - just pass through
    if (g_offline_mode) {
        // T4 probe: when FM2K_T4_PROBE=1, walk the fighter object pool
        // before each update_game tick using the EXACT same logic as
        // vs_round_function case-200 (type==4, flag@+346==0, HP@slot>0).
        // Log when count<2 with details on which entry failed which
        // condition. This captures what case-200 will see when it runs
        // inside this update_game call, so we can pinpoint why the t4
        // walk false-trips on StudioS games (whereas WW always shows 2).
        static const char* env_t4probe = std::getenv("FM2K_T4_PROBE");
        if (game_mode >= 3000 && game_mode < 4000
            && env_t4probe && std::strcmp(env_t4probe, "1") == 0)
        {
            const uint8_t* pool = (const uint8_t*)0x4701E0;
            constexpr uintptr_t HP_BASE   = 0x4DFC85;
            constexpr uintptr_t HP_STRIDE = 57407;
            int count = 0;
            int t4_seen = 0;
            int fail_flag = 0, fail_hp = 0, fail_slot = 0;
            uint32_t fail_e[4] = {0,0,0,0};
            uint32_t fail_why[4] = {0,0,0,0};  // 1=flag, 2=hp, 3=slot
            int fail_n = 0;
            for (int i = 0; i < 1024; i++) {
                const uint8_t* e = pool + i * 382;
                uint32_t type = *(const uint32_t*)(e + 0);
                if (type != 4) continue;
                t4_seen++;
                uint32_t flag346 = *(const uint32_t*)(e + 346);
                uint32_t slot    = *(const uint32_t*)(e + 342);
                if (flag346 != 0) {
                    fail_flag++;
                    if (fail_n < 4) { fail_e[fail_n]=i; fail_why[fail_n]=1; fail_n++; }
                    continue;
                }
                if (slot >= 8) {
                    fail_slot++;
                    if (fail_n < 4) { fail_e[fail_n]=i; fail_why[fail_n]=3; fail_n++; }
                    continue;
                }
                uint32_t hp = *(const uint32_t*)(HP_BASE + slot * HP_STRIDE);
                if (hp == 0) {
                    fail_hp++;
                    if (fail_n < 4) { fail_e[fail_n]=i; fail_why[fail_n]=2; fail_n++; }
                    continue;
                }
                count++;
            }
            static int s_last_count = -1;
            if (count < 2 && (count != s_last_count || (g_frame_counter % 60) == 0)) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[T4-PROBE f=%u] count=%d t4_seen=%d "
                    "fails: flag@346=%d hp=%d slot=%d "
                    "first4=[e=%u why=%u, e=%u why=%u, e=%u why=%u, e=%u why=%u] "
                    "(why: 1=flag@+346!=0, 2=HP[slot]==0, 3=slot>=8)",
                    g_frame_counter, count, t4_seen,
                    fail_flag, fail_hp, fail_slot,
                    fail_e[0], fail_why[0], fail_e[1], fail_why[1],
                    fail_e[2], fail_why[2], fail_e[3], fail_why[3]);
            }
            s_last_count = count;
        }

        g_frame_counter++;
        return original_update_game ? original_update_game() : 0;
    }

    // Spectator mode: the trampoline's RunSpectatorTick owns the sim drive
    // (it pops streamed inputs and calls original_update_game itself). This
    // hook still fires because update_game runs from inside that trampoline
    // call — but we must not run any of the player-side battle-sync /
    // Netplay_StartBattle / GekkoStressSession paths below. Just bump the
    // frame counter and pass through to the real update_game.
    if (g_spectator_mode) {
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
            const char* role =
                g_spectator_mode      ? "SPECTATOR" :
                (g_player_index == 0) ? "HOST"      : "CLIENT";
            bool active = Netplay_IsActive();
            bool connected = Netplay_IsConnected();

            // Spectator title — queue depth tells you at a glance whether
            // we're at live edge (~SPECTATOR_LIVE_TARGET buffered),
            // backfilling (>FF_ENTER), or starved.
            if (g_spectator_mode) {
                size_t qd = SpectatorNode_PendingFrameCount();
                bool sub = SpectatorNode_IsSubscribedUpstream();
                snprintf(title, sizeof(title),
                    "FM2K [P3 SPECTATOR] %s | FPS:%d | queue:%zu",
                    sub ? "Subscribed" : "Connecting...", g_current_fps, qd);
                SetWindowTextA(game_window, title);
                return;
            }

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
                // Label the actual phase from g_game_mode, not just
                // "CSS". The title bar previously stuck on "CSS" the
                // moment handshake completed even though the game was
                // still in the title state machine.
                //   0       boot/early init
                //   1000    title — splash + demo loop + main menu
                //   2000    CSS (character select)
                //   3000-3999 battle (handled by `active` branch above)
                //   anything else → fall through to the numeric value
                uint32_t mode = *(uint32_t*)FM2K::ADDR_GAME_MODE;
                const char* phase = nullptr;
                char phase_buf[16] = {};
                if      (mode == 0)               phase = "BOOT";
                else if (mode == 1000)            phase = "TITLE";
                else if (mode == 2000)            phase = "CSS";
                else if (mode >= 3000 && mode < 4000) phase = "BATTLE";
                else {
                    std::snprintf(phase_buf, sizeof(phase_buf), "MODE %u",
                                  (unsigned)mode);
                    phase = phase_buf;
                }
                snprintf(title, sizeof(title),
                    "FM2K [%s] %s | FPS:%d | RTT:%ums",
                    role, phase, g_current_fps, ping);
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
    //
    // SPECTATOR FIX: include SpectatorNode_IsPlayingBack() — without it,
    // Hook_RenderGame (this function) ran UNPROTECTED on spectators when
    // FM2K-internal code triggered render directly (instead of through the
    // trampoline's RenderFrameWithSnapshot, which already had this check).
    // ProcessShakeEffect / ProcessColorInterpolation / sprite_rendering_engine
    // call game_rand() — those calls accumulated into spectator's RNG but
    // were rolled back on host. RNG drifted by exactly that delta over time,
    // showing up as paired [HOST-FP]/[SPEC-FP] divergence with all other
    // sim state matching (HP/timer/pos/input identical, only RNG differed).
    bool protect_regions = Netplay_IsActive() || SpectatorNode_IsPlayingBack();

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

    // GekkoNet frame-pacing drift correction now lives in the
    // trampoline's SleepToTarget — that function applies the 1.6%
    // slowdown when ahead of peer. The Sleep(extra_ms) trick that
    // used to be in Netplay_HandleFrameTime was unreliable (Sleep
    // granularity, fights with QPC-based outer loop) and didn't
    // actually converge frames_ahead.
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

    // Diagnostic: FM2K_BYPASS_TRAMPOLINE=1 falls through to vanilla
    // main_game_loop. All other hooks (input, update, render, RNG) still
    // fire as detours, so we can isolate the trampoline as a cause vs the
    // individual hooks. Use only for offline tests — netplay/spectator
    // require the trampoline's phase dispatcher to drive Save/Load/Advance.
    static const char* env_bypass = std::getenv("FM2K_BYPASS_TRAMPOLINE");
    static bool bypass = (env_bypass && std::strcmp(env_bypass, "1") == 0);
    if (bypass) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Hooks: FM2K_BYPASS_TRAMPOLINE=1 — calling vanilla "
                    "main_game_loop. Trampoline phase dispatcher "
                    "DISABLED. Netplay/spectator will not work.");
        return original_run_game_loop ? original_run_game_loop() : TRUE;
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

    // CreateFileA/W share-mode override — force shared reads so two
    // instances launched from the same game folder don't get
    // ERROR_SHARING_VIOLATION on .player / .kgt opens.
    HMODULE kernel32 = GetModuleHandleA("kernel32.dll");
    if (kernel32) {
        void* real_CreateFileA = (void*)GetProcAddress(kernel32, "CreateFileA");
        void* real_CreateFileW = (void*)GetProcAddress(kernel32, "CreateFileW");
        if (real_CreateFileA) {
            if (MH_CreateHook(real_CreateFileA, (void*)Hook_CreateFileA,
                              (void**)&original_CreateFileA) != MH_OK ||
                MH_EnableHook(real_CreateFileA) != MH_OK) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "Hooks: Failed to hook CreateFileA");
            } else {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Hooks: CreateFileA hooked (relaxed share mode)");
            }
        }
        if (real_CreateFileW) {
            if (MH_CreateHook(real_CreateFileW, (void*)Hook_CreateFileW,
                              (void**)&original_CreateFileW) != MH_OK ||
                MH_EnableHook(real_CreateFileW) != MH_OK) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "Hooks: Failed to hook CreateFileW");
            } else {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Hooks: CreateFileW hooked (relaxed share mode)");
            }
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
