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
    constexpr uintptr_t ADDR_CHAR_SLOTS = 0x4D1D80;  // g_character_data_base
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

// Network config (parsed at startup, used when entering battle)
extern bool g_offline_mode;
extern uint16_t g_local_port;
extern char g_remote_addr[64];

#endif // GLOBALS_H
