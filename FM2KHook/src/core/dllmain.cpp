// Clean DLL entry point with persistent network connection
#include "globals.h"
#include "hooks.h"
#include "netplay.h"
#include "netplay_state.h"
#include "control_channel.h"
#include <SDL3/SDL_log.h>
#include <windows.h>
#include <mmsystem.h>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <ctime>

// =============================================================================
// FILE LOGGING
// =============================================================================
static FILE* g_log_file = nullptr;

static void SDLCALL LogOutputFunction(void* userdata, int category, SDL_LogPriority priority, const char* message) {
    // Get timestamp
    SYSTEMTIME st;
    GetLocalTime(&st);

    const char* priority_str = "INFO";
    switch (priority) {
        case SDL_LOG_PRIORITY_VERBOSE: priority_str = "VERBOSE"; break;
        case SDL_LOG_PRIORITY_DEBUG: priority_str = "DEBUG"; break;
        case SDL_LOG_PRIORITY_INFO: priority_str = "INFO"; break;
        case SDL_LOG_PRIORITY_WARN: priority_str = "WARN"; break;
        case SDL_LOG_PRIORITY_ERROR: priority_str = "ERROR"; break;
        case SDL_LOG_PRIORITY_CRITICAL: priority_str = "CRITICAL"; break;
        default: break;
    }

    // Format: [HH:MM:SS.mmm] [PRIORITY] message
    char formatted[2048];
    snprintf(formatted, sizeof(formatted), "[%02d:%02d:%02d.%03d] [P%d] [%s] %s\n",
             st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
             g_player_index + 1, priority_str, message);

    // Write to console
    printf("%s", formatted);

    // Write to file
    if (g_log_file) {
        fprintf(g_log_file, "%s", formatted);
        fflush(g_log_file);
    }
}

static void InitFileLogging() {
    char filename[256];
    snprintf(filename, sizeof(filename), "FM2K_P%d_Debug.log", g_player_index + 1);

    g_log_file = fopen(filename, "w");
    if (g_log_file) {
        // Write header
        SYSTEMTIME st;
        GetLocalTime(&st);
        fprintf(g_log_file, "=== FM2K Hook Debug Log - Player %d ===\n", g_player_index + 1);
        fprintf(g_log_file, "Session started: %04d-%02d-%02d %02d:%02d:%02d\n",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        fprintf(g_log_file, "==========================================\n");
        fflush(g_log_file);
    }

    // Set SDL log callback
    SDL_SetLogOutputFunction(LogOutputFunction, nullptr);
}

static void ShutdownFileLogging() {
    if (g_log_file) {
        fprintf(g_log_file, "=== Session ended ===\n");
        fclose(g_log_file);
        g_log_file = nullptr;
    }
}

// =============================================================================
// GAME PATCHES
// =============================================================================

// Forward declarations for game patches (from original game_patches.cpp)
static void BypassMultiInstanceCheck() {
    // At 0x405d05: jz loc_405D15 (0x74 0x0E) - jump if no window found
    // Change to: jmp loc_405D15 (0xEB 0x0E) - always jump (bypass instance check)
    uint8_t* jz_addr = (uint8_t*)0x405d05;

    DWORD old_protect;
    if (VirtualProtect(jz_addr, 1, PAGE_EXECUTE_READWRITE, &old_protect)) {
        *jz_addr = 0xEB;  // Change jz (0x74) to jmp short (0xEB)
        VirtualProtect(jz_addr, 1, old_protect, &old_protect);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Patch: Bypassed multi-instance check");
    }
}

static void ApplyBootToCharacterSelectPatches() {
    // Change initialization object from 0x11 to 0x0A to boot to character select
    // At 0x409CD9: push 0x11 -> push 0x0A
    uint8_t* init_object_ptr = (uint8_t*)0x409CD9;

    DWORD old_protect;
    if (VirtualProtect(init_object_ptr, 2, PAGE_EXECUTE_READWRITE, &old_protect)) {
        init_object_ptr[0] = 0x6A;  // push instruction
        init_object_ptr[1] = 0x0A;  // immediate value (character select)
        VirtualProtect(init_object_ptr, 2, old_protect, &old_protect);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Patch: Boot to character select enabled");
    }
}

static void ApplyCharacterSelectModePatches() {
    // Set character select mode to VS player (1) instead of VS CPU (0)
    uint8_t* char_select_mode_ptr = (uint8_t*)0x470058;  // CHARACTER_SELECT_MODE_ADDR

    DWORD old_protect;
    if (VirtualProtect(char_select_mode_ptr, 1, PAGE_READWRITE, &old_protect)) {
        *char_select_mode_ptr = 1;
        VirtualProtect(char_select_mode_ptr, 1, old_protect, &old_protect);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Patch: Set VS player mode");
    }
}

static void DisableCursorHiding() {
    // In InitializeDirectDraw, the game hides cursor and clips it to 1x1 pixel in fullscreen:
    //   0x4049e0: call ds:ClipCursor   ; clips to tiny rect
    //   0x4049e6: push ebx (0)         ; arg for ShowCursor
    //   0x4049e7: call ds:ShowCursor   ; hides cursor
    //
    // Patch 1: Replace ClipCursor call with pop eax + 5 NOPs (removes pushed rect arg)
    // Patch 2: Replace push + ShowCursor call with 7 NOPs

    DWORD old_protect;

    // Patch ClipCursor at 0x4049e0 (6 bytes)
    // Original: FF 15 AC C1 41 00 (call ds:ClipCursor)
    // Patched:  58 90 90 90 90 90 (pop eax + 5 nops) - pops the pushed rect pointer
    uint8_t* clip_cursor_addr = (uint8_t*)0x4049e0;
    if (VirtualProtect(clip_cursor_addr, 6, PAGE_EXECUTE_READWRITE, &old_protect)) {
        clip_cursor_addr[0] = 0x58;  // pop eax
        clip_cursor_addr[1] = 0x90;  // nop
        clip_cursor_addr[2] = 0x90;  // nop
        clip_cursor_addr[3] = 0x90;  // nop
        clip_cursor_addr[4] = 0x90;  // nop
        clip_cursor_addr[5] = 0x90;  // nop
        VirtualProtect(clip_cursor_addr, 6, old_protect, &old_protect);
    }

    // Patch ShowCursor at 0x4049e6 (7 bytes: push + call)
    // Original: 53 FF 15 74 C1 41 00 (push ebx; call ds:ShowCursor)
    // Patched:  90 90 90 90 90 90 90 (7 nops)
    uint8_t* show_cursor_addr = (uint8_t*)0x4049e6;
    if (VirtualProtect(show_cursor_addr, 7, PAGE_EXECUTE_READWRITE, &old_protect)) {
        show_cursor_addr[0] = 0x90;  // nop (was push ebx)
        show_cursor_addr[1] = 0x90;  // nop
        show_cursor_addr[2] = 0x90;  // nop
        show_cursor_addr[3] = 0x90;  // nop
        show_cursor_addr[4] = 0x90;  // nop
        show_cursor_addr[5] = 0x90;  // nop
        show_cursor_addr[6] = 0x90;  // nop
        VirtualProtect(show_cursor_addr, 7, old_protect, &old_protect);
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Patch: Disabled cursor hiding in fullscreen");
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    switch (ul_reason_for_call) {
    case DLL_PROCESS_ATTACH:
        {
            DisableThreadLibraryCalls(hModule);

            // Force 1ms timer resolution for accurate 100fps frame timing.
            // Without this, timeGetTime() has ~15.6ms resolution and
            // background windows drop to ~64fps (appears as 60fps).
            timeBeginPeriod(1);

            // Apply game patches FIRST before anything else
            BypassMultiInstanceCheck();
            ApplyBootToCharacterSelectPatches();
            ApplyCharacterSelectModePatches();
            DisableCursorHiding();

            // Create our own console
            FreeConsole();
            AllocConsole();
            FILE* fDummy;
            freopen_s(&fDummy, "CONOUT$", "w", stdout);
            freopen_s(&fDummy, "CONOUT$", "w", stderr);
            freopen_s(&fDummy, "CONIN$", "r", stdin);

            // Read player index from environment
            char* env_player = getenv("FM2K_PLAYER_INDEX");
            if (env_player) {
                g_player_index = atoi(env_player);
            }

            // Set console title
            char title[64];
            snprintf(title, sizeof(title), "FM2K P%d Console", g_player_index + 1);
            SetConsoleTitleA(title);

            // Initialize file logging (writes to FM2K_P1_Debug.log or FM2K_P2_Debug.log)
            InitFileLogging();

            SDL_SetLogPriorities(SDL_LOG_PRIORITY_INFO);
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "=== FM2K Hook Starting (Player %d) ===", g_player_index + 1);

            // Parse network config from environment
            char* env_offline = getenv("FM2K_TRUE_OFFLINE");
            g_offline_mode = (env_offline && strcmp(env_offline, "1") == 0);

            if (!g_offline_mode) {
                char* env_port = getenv("FM2K_LOCAL_PORT");
                char* env_remote = getenv("FM2K_REMOTE_ADDR");

                g_local_port = env_port ? (uint16_t)atoi(env_port) : (7000 + g_player_index);

                if (env_remote) {
                    strncpy(g_remote_addr, env_remote, sizeof(g_remote_addr) - 1);
                } else {
                    uint16_t remote_port = 7000 + (1 - g_player_index);
                    snprintf(g_remote_addr, sizeof(g_remote_addr), "127.0.0.1:%d", remote_port);
                }

                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Network config: port=%d remote=%s", g_local_port, g_remote_addr);

                // ================================================================
                // PERSISTENT CONNECTION: Initialize netplay at DLL load
                // This sets up socket, callback, and starts connection handshake
                // ================================================================
                if (!Netplay_Init(g_player_index, g_local_port, g_remote_addr)) {
                    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                        "Failed to initialize netplay!");
                    // Continue anyway - will run offline
                }
            } else {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Offline mode - netplay disabled");
            }

            // Initialize hooks
            if (!InitializeHooks()) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to initialize hooks!");
                return FALSE;
            }

            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "=== FM2K Hook Ready ===");
            break;
        }

    case DLL_PROCESS_DETACH:
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K Hook shutting down...");

        // Restore default timer resolution
        timeEndPeriod(1);

        // Shutdown in reverse order
        ShutdownHooks();

        // Shutdown netplay (GekkoNet session if active)
        Netplay_Shutdown();

        // Shutdown network socket (persistent connection)
        NetSocket_Shutdown();

        // Close log file
        ShutdownFileLogging();
        break;
    }
    return TRUE;
}
