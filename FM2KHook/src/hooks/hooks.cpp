// State-aware hooks implementation - uses netplay state machine
#include "hooks.h"
#include "globals.h"
#include "netplay.h"
#include "netplay_state.h"
#include <MinHook.h>
#include <SDL3/SDL_log.h>
#include <windows.h>
#include <cstdio>

// ============================================================================
// GAME MODE HELPERS
// ============================================================================

static bool IsCSSMode(uint32_t mode) {
    return mode == 2000;
}

static bool IsBattleMode(uint32_t mode) {
    return mode >= 3000 && mode < 4000;
}

// ============================================================================
// HOOK IMPLEMENTATIONS
// ============================================================================

// Hook: GetPlayerInput
// Returns synchronized input from netplay - NO FLIP LOGIC
// The original game function handles facing-based input flip internally
int __cdecl Hook_GetPlayerInput(int player_id, int input_type) {
    // Battle mode with GekkoNet active - return raw synced input
    if (Netplay_IsActive()) {
        uint16_t input = Netplay_GetInput(player_id);
        return input;  // Raw synced input - game handles flip
    }

    // CSS mode with connection - use lockstep inputs
    uint32_t game_mode = *(uint32_t*)FM2K::ADDR_GAME_MODE;
    if (IsCSSMode(game_mode) && Netplay_IsConnected()) {
        uint16_t input = Netplay_GetCSSInput(player_id);
        return input;
    }

    // Offline: use original function (it handles flipping internally)
    return original_get_player_input ? original_get_player_input(player_id, input_type) : 0;
}

// Hook: UpdateGameState
// Main control point for netplay - routes to correct processing based on game mode
int __cdecl Hook_UpdateGameState() {
    uint32_t game_mode = *(uint32_t*)FM2K::ADDR_GAME_MODE;

    // Log game mode changes
    static uint32_t last_logged_mode = 0;
    if (game_mode != last_logged_mode) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "Hooks: game_mode changed: %u -> %u", last_logged_mode, game_mode);
        last_logged_mode = game_mode;
    }

    // Offline mode - just pass through
    if (g_offline_mode) {
        g_frame_counter++;
        return original_update_game ? original_update_game() : 0;
    }

    // ========================================================================
    // CSS MODE (game_mode == 2000) - LOCKSTEP
    // ========================================================================
    if (IsCSSMode(game_mode)) {
        // Process lockstep CSS - blocks until both players have input
        if (!Netplay_ProcessCSS()) {
            return 0;  // Waiting for remote input
        }

        // Check state for countdown blocking (before battle transition)
        NetplayState state = Netplay_GetState();

        // ALWAYS freeze countdown timer during networked CSS
        // Only let game transition when we explicitly allow it (GekkoNet ready)
        uint16_t* timer = (uint16_t*)FM2K::ADDR_ROUND_TIMER_COUNTER;
        *timer = 0;  // Keep frozen until BATTLE_RUNNING

        // BATTLE INIT: Both players locked, start GekkoNet
        if (state == NetplayState::CSS_BOTH_READY ||
            state == NetplayState::BATTLE_INIT ||
            state == NetplayState::BATTLE_SYNCING) {

            // Try to start GekkoNet session if we haven't yet
            if (state == NetplayState::CSS_BOTH_READY) {
                static bool logged_starting = false;
                if (!logged_starting) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Hooks: Both players ready, starting GekkoNet session...");
                    logged_starting = true;
                }
                if (Netplay_StartGekkoSession()) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Hooks: GekkoNet session started, waiting for sync...");
                    logged_starting = false;  // Reset for next time
                }
            }

            // Poll GekkoNet to process session events (must do this to get SessionStarted!)
            // Re-read state since Netplay_StartGekkoSession may have changed it
            state = Netplay_GetState();
            if (state == NetplayState::BATTLE_INIT || state == NetplayState::BATTLE_SYNCING) {
                Netplay_PollGekkoNet();  // Poll without advancing game
            }

            // Check if GekkoNet is now ready
            bool is_ready = Netplay_IsSessionReady();
            static int ready_check_count = 0;
            if (++ready_check_count % 50 == 0) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Hooks: Ready check #%d, state=%d, session_ready=%s",
                    ready_check_count, (int)state, is_ready ? "YES" : "NO");
            }

            if ((state == NetplayState::BATTLE_INIT || state == NetplayState::BATTLE_SYNCING) && is_ready) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Hooks: GekkoNet synced! Releasing countdown...");
                Netplay_SetState(NetplayState::BATTLE_RUNNING);
                // Release timer - set to 101 to trigger battle transition
                *timer = 101;
                ready_check_count = 0;  // Reset for next time
            }
        }

        // If BATTLE_RUNNING, release the timer
        if (state == NetplayState::BATTLE_RUNNING) {
            *timer = 101;  // Force transition to battle
        }

        g_frame_counter++;
        return original_update_game ? original_update_game() : 0;
    }

    // ========================================================================
    // BATTLE MODE (game_mode >= 3000 && < 4000)
    // ========================================================================
    if (IsBattleMode(game_mode)) {
        // GekkoNet must be active for battle
        if (!Netplay_IsActive()) {
            // This shouldn't happen if state machine is working correctly
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "Hooks: Battle mode without GekkoNet! Running offline.");
            g_frame_counter++;
            return original_update_game ? original_update_game() : 0;
        }

        // Let GekkoNet control frame advancement
        // This will block until sync, handle rollbacks, etc.
        if (!Netplay_ProcessBattle()) {
            // Waiting for sync - don't advance game state
            return 0;
        }

        g_frame_counter++;
        return original_update_game ? original_update_game() : 0;
    }

    // ========================================================================
    // OTHER MODES (menu, results, etc.)
    // ========================================================================
    Netplay_ProcessMenu();

    // If we were in battle, transition back to CSS_LOBBY for potential rematch
    NetplayState state = Netplay_GetState();
    if (state == NetplayState::BATTLE_RUNNING || state == NetplayState::BATTLE_END) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "Hooks: Left battle mode, returning to CSS_LOBBY");
        Netplay_StopGekkoSession();  // Stop GekkoNet but keep control channel
        Netplay_SetState(NetplayState::CSS_LOBBY);
    }

    g_frame_counter++;
    return original_update_game ? original_update_game() : 0;
}

// Find window belonging to our process
static HWND g_cached_window = NULL;

static BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam) {
    DWORD pid;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId()) {
        char class_name[64];
        if (GetClassNameA(hwnd, class_name, sizeof(class_name))) {
            if (strcmp(class_name, "KGT2KGAME") == 0) {
                *(HWND*)lParam = hwnd;
                return FALSE;  // Stop enumeration
            }
        }
    }
    return TRUE;  // Continue enumeration
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
void __cdecl Hook_RenderGame() {
    // Track FPS
    g_fps_frame_count++;
    DWORD now = GetTickCount();
    if (now - g_last_fps_time >= 1000) {
        g_current_fps = g_fps_frame_count;
        g_fps_frame_count = 0;
        g_last_fps_time = now;
    }

    // Update window title with player, state, and FPS
    static NetplayState last_state = NetplayState::DISCONNECTED;
    static int last_fps = 0;

    NetplayState state = Netplay_GetState();
    if (state != last_state || g_current_fps != last_fps) {
        HWND game_window = GetOurGameWindow();
        if (game_window) {
            char title[128];
            const char* state_str = "";

            if (!g_offline_mode) {
                switch (state) {
                    case NetplayState::DISCONNECTED: state_str = "Disconnected"; break;
                    case NetplayState::CONNECTING: state_str = "Connecting..."; break;
                    case NetplayState::SYNCED: state_str = "Connected"; break;
                    case NetplayState::CSS_LOBBY: state_str = "CSS"; break;
                    case NetplayState::CSS_LOCAL_READY: state_str = "Waiting"; break;
                    case NetplayState::CSS_REMOTE_READY: state_str = "Opponent ready"; break;
                    case NetplayState::CSS_BOTH_READY: state_str = "Starting..."; break;
                    case NetplayState::BATTLE_INIT: state_str = "Init"; break;
                    case NetplayState::BATTLE_SYNCING: state_str = "Syncing"; break;
                    case NetplayState::BATTLE_RUNNING: state_str = "Battle"; break;
                    case NetplayState::BATTLE_PAUSED: state_str = "Paused"; break;
                    case NetplayState::BATTLE_END: state_str = "End"; break;
                }
            }

            snprintf(title, sizeof(title), "FM2K P%d [%s] %d FPS",
                     g_player_index + 1, state_str, g_current_fps);
            SetWindowTextA(game_window, title);
            last_state = state;
            last_fps = g_current_fps;
        }
    }

    if (original_render_game) {
        original_render_game();
    }
}

// Hook: RunGameLoop
BOOL __cdecl Hook_RunGameLoop() {
    // Re-apply VS player mode patch (game may overwrite during init)
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

    return original_run_game_loop ? original_run_game_loop() : FALSE;
}

// Hook: GameRand - for deterministic RNG during rollback
uint32_t __cdecl Hook_GameRand() {
    // TODO: During rollback, we may need to restore RNG state
    // For now, pass through to original
    return original_game_rand ? original_game_rand() : 0;
}

// Hook: ProcessGameInputs
int __cdecl Hook_ProcessGameInputs() {
    return original_process_game_inputs ? original_process_game_inputs() : 0;
}

// ============================================================================
// HOOK SETUP
// ============================================================================

bool InitializeHooks() {
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Hooks: Initializing MinHook...");

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

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Hooks: All hooks installed successfully");
    return true;
}

void ShutdownHooks() {
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Hooks: Shutdown complete");
}
