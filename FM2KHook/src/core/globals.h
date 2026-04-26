#ifndef GLOBALS_H
#define GLOBALS_H

#include <windows.h>
#include <cstdint>

// ============================================================================
// FUNCTION TYPEDEFS - for hooking original game functions
// ============================================================================
typedef int(__cdecl* GetPlayerInputFunc)(int, int);
typedef int(__cdecl* UpdateGameStateFunc)();
typedef BOOL(__cdecl* RunGameLoopFunc)();
typedef void(__cdecl* RenderGameFunc)();
typedef uint32_t(__cdecl* GameRandFunc)();
typedef int(__cdecl* ProcessGameInputsFunc)();

// Original function pointers (set by MinHook)
extern GetPlayerInputFunc original_get_player_input;
extern UpdateGameStateFunc original_update_game;
extern RunGameLoopFunc original_run_game_loop;
extern RenderGameFunc original_render_game;
extern GameRandFunc original_game_rand;
extern ProcessGameInputsFunc original_process_game_inputs;

// ============================================================================
// FM2K MEMORY ADDRESSES - verified via IDA
// ============================================================================
namespace FM2K {
    // Function addresses
    constexpr uintptr_t ADDR_GET_PLAYER_INPUT = 0x414340;
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

    // Player stage positions (selected character slot index, -1 = none)
    constexpr uintptr_t ADDR_PLAYER_STAGE_POSITIONS = 0x470020;  // g_player_stage_positions[2]

    // Character slots - OPTIMIZED: only save dynamic portion for rollback
    // Static data (sprites, animations, hitboxes) loaded from .player files doesn't change
    // See savestate.h for CHAR_SLOT_* constants
    constexpr uintptr_t ADDR_CHAR_SLOTS = 0x4D1D90;  // g_character_data_base (Wave C audit corrected from 0x4D1D80)
    constexpr size_t CHAR_SLOT_TOTAL_SIZE = 57407;   // Per-slot size from IDA

    // Object pool - projectiles, effects (still need full save)
    constexpr uintptr_t ADDR_OBJECT_POOL = 0x4701E0;
    constexpr size_t SIZE_OBJECT_POOL = 0x5F800;
}

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

#endif // GLOBALS_H
