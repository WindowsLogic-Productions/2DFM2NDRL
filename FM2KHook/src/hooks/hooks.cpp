// Simplified hooks - detect battle mode transitions, delegate to netplay
// Sync barrier: block game until both clients connected (CCCaster-style)
#include "hooks.h"
#include "globals.h"
#include "netplay.h"
#include "control_channel.h"
#include "imgui_overlay.h"
#include <MinHook.h>
#include <SDL3/SDL_log.h>
#include <windows.h>
#include <cstdio>

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

// Battle sync state - ensures both clients start GekkoNet together
static bool g_battle_entry_signaled = false;

// Called every frame to check for game mode transitions
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

        // Apply facing direction swap (same logic as original get_player_input)
        // During battle (3000-3999), if character is active and not in special state,
        // left/right are swapped based on facing direction
        constexpr uintptr_t ADDR_CHAR_ACTIVE_FLAG = 0x4DFCD1;
        constexpr uintptr_t ADDR_CHAR_STATE_FLAGS = 0x4D9A36;
        constexpr size_t CHAR_SLOT_SIZE = 57407;

        bool facing_reversed = true;  // Default: no swap (normal directions)
        if (game_mode >= 3000 && game_mode < 4000) {
            uint8_t char_active = *(uint8_t*)(ADDR_CHAR_ACTIVE_FLAG + input_type * CHAR_SLOT_SIZE);
            if (char_active != 0) {
                uint8_t char_flags = *(uint8_t*)(ADDR_CHAR_STATE_FLAGS + input_type * CHAR_SLOT_SIZE);
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

        // Log periodically
        static uint32_t battle_log_count = 0;
        if (battle_log_count < 10 || battle_log_count % 100 == 0) {
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
        // Check if we can advance (have remote input)
        // ProcessGameInputs hook also checks this - both must agree
        if (!Netplay_CanAdvanceCSS()) {
            Netplay_PollCSS();  // Try to receive pending data
            return 0;  // Stall - don't update game state
        }

        // Remote input available - capture, send, advance CSS frame
        Netplay_ProcessCSS();

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
void __cdecl Hook_RenderGame() {
    // Check F9 hotkey for debug overlay
    CheckOverlayHotkey();

    // Track FPS
    g_fps_frame_count++;
    DWORD now = GetTickCount();
    if (now - g_last_fps_time >= 1000) {
        g_current_fps = g_fps_frame_count;
        g_fps_frame_count = 0;
        g_last_fps_time = now;
    }

    // Update window title - simplified states
    static int last_fps = 0;
    static bool last_connected = false;
    static bool last_active = false;

    bool connected = Netplay_IsConnected();
    bool active = Netplay_IsActive();

    if (g_current_fps != last_fps || connected != last_connected || active != last_active) {
        HWND game_window = GetOurGameWindow();
        if (game_window) {
            char title[128];
            const char* state_str = "Offline";

            if (!g_offline_mode) {
                if (active) {
                    state_str = "Battle";
                } else if (connected) {
                    state_str = "Connected";
                } else {
                    state_str = "Connecting...";
                }
            }

            snprintf(title, sizeof(title), "FM2K P%d [%s] %d FPS",
                     g_player_index + 1, state_str, g_current_fps);
            SetWindowTextA(game_window, title);

            last_fps = g_current_fps;
            last_connected = connected;
            last_active = active;
        }
    }

    // CRITICAL: Save/restore RNG around render to prevent render-path RNG
    // consumption from breaking determinism. ProcessShakeEffect and
    // ProcessColorInterpolation call game_rand() during rendering, and
    // render counts differ between clients (P2 has rollback overhead).
    // Without this, RNG diverges after ~5000 frames once effects trigger.
    uint32_t saved_rng = 0;
    bool protect_rng = Netplay_IsActive();
    if (protect_rng) {
        saved_rng = *(uint32_t*)FM2K::ADDR_RANDOM_SEED;
    }

    if (original_render_game) {
        original_render_game();
    }

    if (protect_rng) {
        *(uint32_t*)FM2K::ADDR_RANDOM_SEED = saved_rng;
    }
}

// Hook: RunGameLoop
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

    return original_run_game_loop ? original_run_game_loop() : FALSE;
}

// Hook: GameRand - pass through
uint32_t __cdecl Hook_GameRand() {
    return original_game_rand ? original_game_rand() : 0;
}

// Hook: ProcessGameInputs
// During battle: get synced inputs from GekkoNet and write to game memory
int __cdecl Hook_ProcessGameInputs() {
    uint32_t game_mode = *(uint32_t*)FM2K::ADDR_GAME_MODE;

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
