#ifndef GLOBALS_H
#define GLOBALS_H

#include <windows.h>
#include <cstdint>

// ============================================================================
// FUNCTION TYPEDEFS - for hooking original game functions
// ============================================================================
typedef int(__cdecl* GetPlayerInputFunc)(int, int);
typedef int(__cdecl* GetPlayerInputFM95Func)(int);  // FM95 split: p1/p2 are separate single-arg fns
typedef int(__cdecl* UpdateGameStateFunc)();
typedef BOOL(__cdecl* RunGameLoopFunc)();
typedef void(__cdecl* RenderGameFunc)();
typedef uint32_t(__cdecl* GameRandFunc)();
typedef int(__cdecl* ProcessGameInputsFunc)();

// Original function pointers (set by MinHook)
extern GetPlayerInputFunc original_get_player_input;
extern GetPlayerInputFM95Func original_get_player_input_p1;  // FM95 only — 0x408AE0
extern GetPlayerInputFM95Func original_get_player_input_p2;  // FM95 only — 0x408D60
extern UpdateGameStateFunc original_update_game;
extern RunGameLoopFunc original_run_game_loop;
extern RenderGameFunc original_render_game;
extern GameRandFunc original_game_rand;
extern ProcessGameInputsFunc original_process_game_inputs;

// ============================================================================
// ENGINE TARGET — the same source compiles into:
//   FM2KHook.dll : default build, addresses target Fighter Maker 2nd binaries
//                  (e.g. WonderfulWorld_ver_0946.exe — base 0x400000)
//   FM95Hook.dll : -DENGINE_FM95=1 build, addresses target Fighter Maker 95
//                  prototype binaries (e.g. CPW.exe — base 0x400000)
//
// Both DLLs export the same hook surface; only the address values change.
// The launcher picks which to inject based on FM2K::FM2KGameInfo::engine.
//
// FM95 addresses are sourced from /mnt/c/dev/wanwan/FM95_Integration.h (verified
// against the live CPW IDB at C:\dev\fm95\CPW\ＣＰＷ.exe.i64). Address values
// without a direct FM95 equivalent are set to 0 and must be guarded with
// `#if !defined(ENGINE_FM95)` at consumer sites — the build will surface them.
// ============================================================================
#if defined(ENGINE_FM95)
namespace FM2K {
    // Function addresses (FM95 / CPW.exe — verified 2026-05-05)
    // FM95 has SEPARATE per-player input fns; the trampoline assumes a single
    // GET_PLAYER_INPUT — leave the P1 entry as the canonical hook target and
    // expose P2 separately for hooks that need it.
    constexpr uintptr_t ADDR_GET_PLAYER_INPUT = 0x408AE0;   // FM95: get_player_input_p1
    constexpr uintptr_t ADDR_GET_PLAYER_INPUT_P2 = 0x408D60; // FM95-only second target
    constexpr uintptr_t ADDR_UPDATE_GAME = 0x40A060;
    constexpr uintptr_t ADDR_RUN_GAME_LOOP = 0x40AB60;       // FM95: _WinMain@16 (frame loop is inlined)
    constexpr uintptr_t ADDR_RENDER_GAME = 0x40A910;
    constexpr uintptr_t ADDR_GAME_RAND = 0x41A864;           // FM95: CRT _rand
    constexpr uintptr_t ADDR_PROCESS_INPUTS = 0x408FF0;
    constexpr uintptr_t ADDR_DISPATCH_SCRIPT_SOUND = 0x401FF0;

    // Game state addresses
    constexpr uintptr_t ADDR_FRAME_COUNTER = 0x4DD7A8;       // FM95: g_game_tick_counter
    constexpr uintptr_t ADDR_GAME_MODE = 0x425558;           // values 0/1/10/0x10 — see FM95_Integration.h delta #2
    constexpr uintptr_t ADDR_RANDOM_SEED = 0x4243FC;

    // Player current-frame inputs (post-poll cache)
    constexpr uintptr_t ADDR_P1_INPUT = 0x437750;
    constexpr uintptr_t ADDR_P2_INPUT = 0x437754;

    // Per-player HP — FM95 has no global HP scalar; the value lives at
    // offset +72 of each player's main-object slot in the pool. The
    // pointers to those slots are kept in the array below, indexed by
    // player_idx (0/1). HP read pattern (FM95-specific):
    //   void* main = *(void**)(ADDR_P_MAIN_OBJECT_PTR + player_idx*4);
    //   uint32_t hp = *(uint32_t*)((uint8_t*)main + 72);
    // Static globals stay 0 to force consumer sites to branch on
    // kIsFM95 and use the indirection.
    constexpr uintptr_t ADDR_P1_HP = 0;
    constexpr uintptr_t ADDR_P2_HP = 0;
    // g_p_main_object_ptr — array of pointers to each player's main
    // object in g_object_pool. IDA-verified in CPW @ 0x430e84
    // (referenced by process_combo_input, character_state_machine).
    // Index 0 = P1, 1 = P2. Per-player HP read pattern:
    //   void* main = *(void**)(ADDR_P_MAIN_OBJECT_PTR + pid*4);
    //   uint32_t hp = *(uint32_t*)((uint8_t*)main + 72);
    constexpr uintptr_t ADDR_P_MAIN_OBJECT_PTR = 0x430e84;

    // Round-win counters — IDA-verified canonical fields used by
    // obj_match_result_state @ 0x410db0 case 4 to decide the match
    // winner. Per-player struct stride is 100 bytes (25 dwords);
    // g_p1_win_counter[25*1] == g_p2_win_counter. These are the
    // safest fields to compare in Netplay_EndBattle (no pointer
    // indirection, monotonic across rounds, set to 0 only at the
    // start of a new match in vs_round_function case 1).
    constexpr uintptr_t ADDR_P1_WIN_COUNTER = 0x5e9914;
    constexpr uintptr_t ADDR_P2_WIN_COUNTER = 0x5e9978;

    // Damage-taken / HP-loss scalars — IDA shows
    // vs_round_function case 30 spawning the KO panel when
    // g_p1_damage_taken[0] == g_char_max_hp_table[0]. Same 100-byte
    // stride as win_counter; useful for in-match HP queries (e.g.
    // titlebar live HP%) without going through the main_object_ptr
    // indirection. (IDA's auto-name "g_p1_total_wins" is misleading —
    // these aren't win counts; they're cumulative damage taken.)
    constexpr uintptr_t ADDR_P1_DAMAGE_TAKEN = 0x5e9910;
    constexpr uintptr_t ADDR_P2_DAMAGE_TAKEN = 0x5e9974;

    // CSS state — FM95 layout differs (cursor is single int, not x/y pair).
    // Treat ADDR_P*_CURSOR_POS as the cursor index slot. ACTION_STATE maps to
    // the per-player 'confirmed' flag at +0x0C of the per-player CSS struct.
    constexpr uintptr_t ADDR_P1_CURSOR_POS = 0x432720;       // FM95: g_p1_css_char_cursor (int32)
    constexpr uintptr_t ADDR_P2_CURSOR_POS = 0x432730;       // FM95: g_p2_css_char_cursor
    constexpr uintptr_t ADDR_P1_ACTION_STATE = 0x43272C;     // FM95: g_p1_css_confirmed
    constexpr uintptr_t ADDR_P2_ACTION_STATE = 0x43273C;     // FM95: g_p2_css_confirmed

    // No direct FM95 analog — FM2K's "frames since both locked" is replaced by
    // walking the type=19 object's sub_state in [0x69, 0x6F] range.
    constexpr uintptr_t ADDR_ROUND_TIMER_COUNTER = 0;

    // Internal game timer — same address as frame counter on FM95 (single counter).
    constexpr uintptr_t ADDR_GAME_TIMER = 0x4DD7A8;

    // No global CSS_ACTIVE_PLAYER on FM95; phase classification walks the pool.
    constexpr uintptr_t ADDR_CSS_ACTIVE_PLAYER = 0;
    constexpr uintptr_t ADDR_PLAYER_STAGE_POSITIONS = 0;

    // Character data — FM95 has 5 slots × 229844 bytes at 0x509100.
    // Static (don't save during rollback).
    constexpr uintptr_t ADDR_CHAR_SLOTS       = 0x509100;
    constexpr size_t    CHAR_SLOT_TOTAL_SIZE  = 229844;
    constexpr size_t    CHAR_SLOT_COUNT       = 5;

    // Object pool — 256 × 0xA4 layout, base 0x426A40.
    constexpr uintptr_t ADDR_OBJECT_POOL      = 0x426A40;
    constexpr size_t    OBJECT_POOL_STRIDE    = 0xA4;        // 164 bytes/slot
    constexpr size_t    OBJECT_POOL_COUNT     = 256;
    constexpr size_t    SIZE_OBJECT_POOL      = OBJECT_POOL_COUNT * OBJECT_POOL_STRIDE;  // 0xA400

    // Input ring — 256 entries × 4 bytes (8-bit modulo idx).
    constexpr uintptr_t ADDR_INPUT_BUFFER_INDEX = 0x437700;
    constexpr uintptr_t ADDR_P1_INPUT_HISTORY   = 0x431720;
    constexpr uintptr_t ADDR_P2_INPUT_HISTORY   = 0x431B20;
    constexpr size_t    INPUT_HISTORY_LEN       = 256;
    constexpr uint32_t  INPUT_HISTORY_MASK      = INPUT_HISTORY_LEN - 1;  // 0xFF

    // CSS — selected character IDs (cursor index at confirm time on FM95).
    constexpr uintptr_t ADDR_P1_SELECTED_CHAR     = 0x432720;
    constexpr uintptr_t ADDR_P2_SELECTED_CHAR     = 0x432730;
    // Character filename table — 50 slots × 256-byte CP932 strings.
    constexpr uintptr_t ADDR_CHAR_FILENAME_TABLE  = 0x463CF0;
    constexpr size_t    CHAR_FILENAME_STRIDE      = 256;
    constexpr size_t    CHAR_FILENAME_COUNT       = 50;

    // Stage selection on FM95 splits two ways:
    //   - Practice mode: vs_round_function reads g_practice_stage_id
    //     (uint8) at 0x43274c — this IS the random-stage write target
    //     that mirrors FM2K's 0x43010c.
    //   - VS / Story mode: stage is character-driven, read from
    //     g_char_stage_per_round[char][round] at 0x540337. There's no
    //     single "selected stage" scalar; to override in vs-mode the
    //     hook would need a function-prologue trampoline on
    //     LoadStageFile_alt @ 0x4054b0 to rewrite its first arg.
    //     Deferred — random-stage on FM95 currently only takes effect
    //     in practice mode.
    constexpr uintptr_t ADDR_SELECTED_STAGE       = 0x43274c;
    constexpr uintptr_t ADDR_LOAD_STAGE_FILE_ALT  = 0x4054b0;
    constexpr uintptr_t ADDR_CHAR_STAGE_PER_ROUND = 0x540337;

    // Engine tag — runtime check for code that needs to branch on engine.
    constexpr bool kIsFM95 = true;
    constexpr bool kIsFM2K = false;
}
#else  // ENGINE_FM2K (default)
namespace FM2K {
    // Function addresses
    constexpr uintptr_t ADDR_GET_PLAYER_INPUT = 0x414340;
    // FM2K has a single dispatch fn; this stub exists only so the FM95-
    // specific install path in InitializeHooks compiles on FM2K builds.
    constexpr uintptr_t ADDR_GET_PLAYER_INPUT_P2 = 0;
    constexpr uintptr_t ADDR_UPDATE_GAME = 0x404CD0;
    constexpr uintptr_t ADDR_RUN_GAME_LOOP = 0x405AD0;
    constexpr uintptr_t ADDR_RENDER_GAME = 0x404DD0;
    constexpr uintptr_t ADDR_GAME_RAND = 0x417A22;
    constexpr uintptr_t ADDR_PROCESS_INPUTS = 0x4146D0;
    constexpr uintptr_t ADDR_DISPATCH_SCRIPT_SOUND = 0x403430;  // SFX dispatcher — rollback sound hook

    // Game state addresses
    constexpr uintptr_t ADDR_FRAME_COUNTER = 0x4456FC;
    constexpr uintptr_t ADDR_GAME_MODE = 0x470054;
    constexpr uintptr_t ADDR_RANDOM_SEED = 0x41FB1C;

    // Player data
    constexpr uintptr_t ADDR_P1_INPUT = 0x4259C0;
    constexpr uintptr_t ADDR_P2_INPUT = 0x4259C4;
    constexpr uintptr_t ADDR_P1_HP = 0x47010C;
    constexpr uintptr_t ADDR_P2_HP = 0x47030C;

    // CSS (Character Select Screen) state - from IDA analysis
    // Cursor positions: each is a struct with {int x, int y}
    constexpr uintptr_t ADDR_P1_CURSOR_POS = 0x424E50;  // g_p1_cursor_pos
    constexpr uintptr_t ADDR_P2_CURSOR_POS = 0x424E58;  // g_p2_cursor_pos

    // Action state: 0 = selecting, 1 = locked/confirmed
    constexpr uintptr_t ADDR_P1_ACTION_STATE = 0x47019C;  // g_p1_action_state
    constexpr uintptr_t ADDR_P2_ACTION_STATE = 0x4701A0;  // g_p2_action_state

    // Round timer counter: frames since both players locked (>100 triggers battle)
    constexpr uintptr_t ADDR_ROUND_TIMER_COUNTER = 0x47008E;  // g_round_timer_counter

    // Internal game timer - equivalent to CCCaster's CC_WORLD_TIMER_ADDR
    constexpr uintptr_t ADDR_GAME_TIMER = 0x470044;  // g_game_timer (increments every game frame)

    // CSS active player (for stage select mode)
    constexpr uintptr_t ADDR_CSS_ACTIVE_PLAYER = 0x424F24;  // g_css_active_player

    // CSS-selected character index pair (uint32 [2]). The IDA name on
    // this address used to be g_player_stage_positions, which was a
    // misnomer — neither field carries stage data; both are just the
    // currently-selected character grid index for each player. Renamed
    // 2026-05-05 to g_p1_selected_char_idx / g_p2_selected_char_idx
    // after verifying via ProcessCharacterSelectHandler (0x407D70).
    // Kept as a legacy alias here so any older code that still names
    // it the old way keeps building until they migrate to
    // ADDR_P1_SELECTED_CHAR / ADDR_P2_SELECTED_CHAR below.
    constexpr uintptr_t ADDR_PLAYER_STAGE_POSITIONS = 0x470020;

    // Character slots - OPTIMIZED: only save dynamic portion for rollback
    // Static data (sprites, animations, hitboxes) loaded from .player files doesn't change
    // See savestate.h for CHAR_SLOT_* constants
    constexpr uintptr_t ADDR_CHAR_SLOTS       = 0x4D1D90;  // g_character_data_base (Wave C audit corrected from 0x4D1D80)
    constexpr size_t    CHAR_SLOT_TOTAL_SIZE  = 57407;     // Per-slot size from IDA
    constexpr size_t    CHAR_SLOT_COUNT       = 8;

    // Object pool — 1024 × 382 layout, base 0x4701E0.
    constexpr uintptr_t ADDR_OBJECT_POOL      = 0x4701E0;
    constexpr size_t    OBJECT_POOL_STRIDE    = 382;       // bytes/slot
    constexpr size_t    OBJECT_POOL_COUNT     = 1024;
    constexpr size_t    SIZE_OBJECT_POOL      = 0x5F800;   // OBJECT_POOL_COUNT * OBJECT_POOL_STRIDE

    // Input ring — 1024 entries × 4 bytes (10-bit modulo idx).
    constexpr uintptr_t ADDR_INPUT_BUFFER_INDEX = 0x470000;
    constexpr uintptr_t ADDR_P1_INPUT_HISTORY   = 0x470200;
    constexpr uintptr_t ADDR_P2_INPUT_HISTORY   = 0x470400;
    constexpr size_t    INPUT_HISTORY_LEN       = 1024;
    constexpr uint32_t  INPUT_HISTORY_MASK      = INPUT_HISTORY_LEN - 1;  // 0x3FF

    // CSS — selected character indexes. Point to g_player_stage_positions,
    // a uint32 pair at 0x470020/0x470024. ProcessCharacterSelectHandler
    // (0x407D70 in WonderfulWorld_ver_0946) writes each player's pick
    // into [0]/[1] then immediately calls
    // `player_data_file_loader(player, g_player_stage_positions[player])`,
    // which loads `<g_char_slot_data[256 * idx]>.player`. So [0]/[1] are
    // the canonical "this is the char I picked" values that drive the
    // .player roster lookup. Verified via IDA xrefs 2026-05-05.
    //
    // The previous values 0x470180/0x470184 were unwritten zero memory
    // (no xrefs from any code) — every match shipped char_id=0 to the
    // hub, so both peers always got logged as the slot-0 character.
    constexpr uintptr_t ADDR_P1_SELECTED_CHAR     = 0x470020;
    constexpr uintptr_t ADDR_P2_SELECTED_CHAR     = 0x470024;

    // FM95-only fields (stubbed at 0 on FM2K). Consumers must guard
    // on `if constexpr (kIsFM95)`; without these stubs the FM2K-side
    // parse of an `if constexpr (kIsFM95) { FM2K::ADDR_P1_WIN_COUNTER }`
    // body fails name lookup before the branch is discarded.
    constexpr uintptr_t ADDR_P_MAIN_OBJECT_PTR  = 0;
    constexpr uintptr_t ADDR_P1_WIN_COUNTER     = 0;
    constexpr uintptr_t ADDR_P2_WIN_COUNTER     = 0;
    constexpr uintptr_t ADDR_P1_DAMAGE_TAKEN    = 0;
    constexpr uintptr_t ADDR_P2_DAMAGE_TAKEN    = 0;
    constexpr uintptr_t ADDR_LOAD_STAGE_FILE_ALT  = 0;
    constexpr uintptr_t ADDR_CHAR_STAGE_PER_ROUND = 0;
    // Character filename table — g_char_slot_data, 256-byte CP932 strings.
    constexpr uintptr_t ADDR_CHAR_FILENAME_TABLE  = 0x435474;
    constexpr size_t    CHAR_FILENAME_STRIDE      = 256;
    constexpr size_t    CHAR_FILENAME_COUNT       = 258;       // 0x1023C / 256

    // Stage selection — global int32 the round-init code reads as the
    // VS-mode stage_id. IDA-verified in WonderfulWorld_ver_0946:
    //   * vs_round_function @ 0x4087e0 calls LoadStageFile(wParam),
    //     where wParam is IDA's name for this address.
    //   * settings_dialog_proc @ 0x416387 stores the stage dropdown's
    //     CB_GETCURSEL into this address.
    // The previous value 0x470188 had ZERO xrefs in WW — every write
    // from the random-stage feature went to a dead address while
    // the game read the cursor's own pick from 0x43010c.
    constexpr uintptr_t ADDR_SELECTED_STAGE       = 0x43010c;

    // Engine tag — runtime check for code that needs to branch on engine.
    constexpr bool kIsFM95 = false;
    constexpr bool kIsFM2K = true;
}
#endif // ENGINE_FM95

// ============================================================================
// MINIMAL GLOBAL STATE
// ============================================================================

// Player identity (set at startup from environment)
extern int g_player_index;  // 0 = P1/Host, 1 = P2/Client

// Frame counter (for logging)
extern uint32_t g_frame_counter;

// Rollback state flag - true during GekkoNet rollback replay frames
// Used by input hooks to avoid corrupting edge detection state during replay
extern bool g_is_rolling_back;

// Network config (parsed at startup, used when entering battle)
extern bool g_offline_mode;
extern uint16_t g_local_port;
extern char g_remote_addr[64];

// Stress-test mode: single-instance determinism check via GekkoStressSession.
// When enabled, FM2KHook creates a GekkoStressSession with both players local,
// no network, and GekkoNet artificially rolls back every `check_distance` frames.
// If the sim is deterministic, desync_detection stays silent. If it isn't,
// we get a local-only repro of the nondeterminism bug without needing a peer.
// Set via FM2K_STRESS_MODE=1 env var. Implies !g_offline_mode (we still want
// GekkoNet active to drive save/load/advance events) but skips socket setup.
extern bool g_stress_mode;

// Spectator mode: this instance is a passive viewer, not a player. Skips
// the HELLO/HELLO_ACK netplay handshake and instead sends SPEC_JOIN_REQ to
// the configured remote (the host) at startup. The trampoline pins the
// phase to SPECTATOR_PLAYBACK regardless of game_mode so the local FM2K's
// CSS doesn't run native lockstep code while waiting for upstream.
// Set via FM2K_SPECTATOR_MODE=1 env var.
extern bool g_spectator_mode;

// FM95 host-driven trampoline: Hook_UpdateGameState sets this to tell
// Hook_RenderGame to skip the host's natural render call this frame
// (the trampoline tick's RenderFrameWithSnapshot already rendered).
// Cleared by Hook_RenderGame on consumption.
extern bool g_fm95_skip_next_render;

// ============================================================================
// LOG-FILE PATH HELPER
// ============================================================================
// All hook-side diagnostic / debug log files (FM2K_P*_Debug.log, EB diag,
// rng trace, parity recorder, replay diff log, etc.) go through here so they
// land in `<game_dir>/logs/<filename>` instead of cluttering the game folder.
// `out` must be at least MAX_PATH bytes. Creates the logs directory lazily
// on first call. Returns true on success; on failure leaves `out` unchanged
// (callers can fall back to writing in cwd).
bool Fm2k_BuildLogPath(char* out, size_t out_size, const char* filename);

#endif // GLOBALS_H
