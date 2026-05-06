// Clean DLL entry point with persistent network connection
#include "globals.h"
#include "hooks.h"
#include "netplay.h"
#include "netplay_state.h"
#include "control_channel.h"
#include "shared_mem.h"
#include "../ui/screenshot.h"
#include "../locale/locale_spoof.h"

// Forward-declare just the new patch function. We can't include
// game_patches.h here because dllmain.cpp keeps static duplicates of
// the older patch functions and the extern declarations would conflict.
extern void PatchVsRoundCase200T4FalsePositive();
extern void NeuterFullscreenTogglesForCncDdraw();
#include <SDL3/SDL_log.h>
#include <windows.h>
#include <mmsystem.h>
#include <psapi.h>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <ctime>
#include <cctype>
#include <string>

// =============================================================================
// LOG-FILE PATH HELPER
// =============================================================================
// All hook-side log files (FM2K_P*_Debug.log + EB diag + rng trace + parity
// recorder + replay diffs) route through here so they land in
// `<cwd>/logs/<filename>` instead of cluttering the game folder. Lazily
// creates the logs directory.
bool Fm2k_BuildLogPath(char* out, size_t out_size, const char* filename) {
    if (!out || out_size == 0 || !filename) return false;
    static bool s_dir_created = false;
    if (!s_dir_created) {
        // CreateDirectoryA returns FALSE + ERROR_ALREADY_EXISTS if it's
        // already there; either way the dir is usable afterwards.
        CreateDirectoryA("logs", nullptr);
        s_dir_created = true;
    }
    int n = std::snprintf(out, out_size, "logs\\%s", filename);
    return n > 0 && (size_t)n < out_size;
}

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

    // File-only for player clients (SoundRollback / ROLLBACK stats / BATTLE
    // STATUS spam too noisy on console). For the spectator instance we DO
    // mirror to console — its console is otherwise empty and tracing the
    // spectator-tree protocol live is the whole point of running it locally.
    if (g_spectator_mode) {
        fputs(formatted, stdout);
        fflush(stdout);
    }
    if (g_log_file) {
        fprintf(g_log_file, "%s", formatted);
        fflush(g_log_file);
    }
}

static void InitFileLogging() {
    char base_name[64];
    snprintf(base_name, sizeof(base_name), "FM2K_P%d_Debug.log", g_player_index + 1);
    char filename[MAX_PATH];
    if (!Fm2k_BuildLogPath(filename, sizeof(filename), base_name)) {
        // Fallback to cwd if the helper failed (extremely unlikely — only
        // CreateDirectoryA would have to fail with something other than
        // ERROR_ALREADY_EXISTS, e.g. permission denied).
        snprintf(filename, sizeof(filename), "%s", base_name);
    }

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
    // Three boot strategies, all hit the same patch site at 0x409CD9
    // (the `push <type>` immediate inside InitializeGameFromCommandLine).
    //
    //   no patch (FM2K_SKIP_BOOT_CSS_PATCH=1)
    //     vanilla path: push 0x11 → ProcessIntroCutscene (3 bitmaps,
    //     ~300-frame splash) → title_screen_manager → CSS.
    //
    //   default (safe boot)
    //     push 0x0C → title_screen_manager directly. Skips the splash.
    //     Title runs its init (ResetObjectsAndCalculateSpeed, populates
    //     g_titleMenu_modeList, etc.). The hook then auto-mashes button
    //     A while g_game_mode == 1000 and lands on CSS with VS Player
    //     selected. Universally compatible — every game we've tested
    //     boots fine through the title path.
    //
    //   fast boot (FM2K_BOOT_TO_CSS_DIRECT=1)
    //     push 0x0A → game_state_manager (CSS) directly. Skips title
    //     entirely. *NOT SAFE* on every game — StudioS Fighters /
    //     Strip Fighter Zero characters trip on uninitialized
    //     g_char_velocity_multiplier and self-execute KGT_OP_THROW_EXECUTE
    //     (opcode 0x15) on frame 0 of battle. Only enable for games
    //     verified to behave (WW, etc.).
    if (const char* e = std::getenv("FM2K_SKIP_BOOT_CSS_PATCH"); e && std::strcmp(e, "1") == 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "PATCH: FM2K_SKIP_BOOT_CSS_PATCH=1 — leaving vanilla intro "
            "path (cutscene + title screen).");
        return;
    }

    uint8_t boot_type = 0x0C;  // default: title_screen_manager
    const char* fast = std::getenv("FM2K_BOOT_TO_CSS_DIRECT");
    if (fast && std::strcmp(fast, "1") == 0) {
        boot_type = 0x0A;  // game_state_manager (CSS direct)
    }

    uint8_t* init_object_ptr = (uint8_t*)0x409CD9;
    DWORD old_protect;
    if (VirtualProtect(init_object_ptr, 2, PAGE_EXECUTE_READWRITE, &old_protect)) {
        init_object_ptr[0] = 0x6A;          // push imm8 (unchanged)
        init_object_ptr[1] = boot_type;
        VirtualProtect(init_object_ptr, 2, old_protect, &old_protect);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "Patch: Boot to %s (push 0x%02X)",
            boot_type == 0x0A ? "CSS direct (FAST)" :
            boot_type == 0x0C ? "title screen (SAFE)" : "?",
            boot_type);
    }
}

static void ApplyCharacterSelectModePatches() {
    if (const char* e = std::getenv("FM2K_SKIP_VS_MODE_PATCH"); e && std::strcmp(e, "1") == 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "PATCH: FM2K_SKIP_VS_MODE_PATCH=1 — not forcing VS player mode.");
        return;
    }
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

            // Read player index early so the log filename is right.
            if (char* env_player = std::getenv("FM2K_PLAYER_INDEX")) {
                g_player_index = std::atoi(env_player);
            }

            // Init console + file logging FIRST so every later step
            // (locale spoof, FM2K patches, hook init) is captured in
            // <game_dir>/logs/FM2K_P*_Debug.log.
            FreeConsole();
            AllocConsole();
            FILE* fDummy;
            freopen_s(&fDummy, "CONOUT$", "w", stdout);
            freopen_s(&fDummy, "CONOUT$", "w", stderr);
            freopen_s(&fDummy, "CONIN$", "r", stdin);
            char title[64];
            std::snprintf(title, sizeof(title), "FM2K P%d Console", g_player_index + 1);
            SetConsoleTitleA(title);
            InitFileLogging();
            SDL_SetLogPriorities(SDL_LOG_PRIORITY_INFO);
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "=== FM2K Hook Starting (Player %d) ===", g_player_index + 1);

            // ddraw-redirect diagnostic: enumerate every loaded module and
            // log the names + paths of anything matching ddraw / 2dfmd. This
            // runs from the FM2KHook injection thread, so process init has
            // already happened — the EXE's static imports are loaded by now,
            // including whichever ddraw flavor the loader picked. If our
            // patch worked, we should see 2DFMD.dll in the list. If we see
            // DDRAW.dll instead (from system32 / KnownDlls), the patch
            // didn't influence import resolution.
            {
                HMODULE mods[256] = {};
                DWORD needed = 0;
                HANDLE proc = GetCurrentProcess();
                if (EnumProcessModulesEx(proc, mods, sizeof(mods), &needed,
                                         LIST_MODULES_ALL)) {
                    const DWORD count = needed / sizeof(HMODULE);
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "DDrawDiag: %lu modules loaded; scanning for ddraw/2dfmd:",
                        (unsigned long)count);
                    bool any_match = false;
                    for (DWORD i = 0; i < count; ++i) {
                        char name[MAX_PATH] = {};
                        if (GetModuleBaseNameA(proc, mods[i], name, sizeof(name))) {
                            // Case-insensitive substring match
                            std::string lower = name;
                            for (auto& c : lower) c = (char)tolower((unsigned char)c);
                            if (lower.find("ddraw") != std::string::npos ||
                                lower.find("2dfmd") != std::string::npos) {
                                char path[MAX_PATH] = {};
                                GetModuleFileNameExA(proc, mods[i], path, sizeof(path));
                                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                    "DDrawDiag:   MATCH base='%s' path='%s' handle=0x%p",
                                    name, path, (void*)mods[i]);
                                any_match = true;
                            }
                        }
                    }
                    if (!any_match) {
                        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "DDrawDiag: NO ddraw/2dfmd module loaded — "
                            "neither system DDRAW.dll nor our 2DFMD.dll "
                            "is present (game might not have static-imported it).");
                    }
                } else {
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "DDrawDiag: EnumProcessModulesEx failed: %lu",
                        GetLastError());
                }
            }

            // Locale spoofing — Japanese codepage / LCID + GDI/USER32 ANSI
            // text rendering hooks. Always-on for BOTH FM2K and FM95 builds:
            // re-encoding ASCII strings through CP932 round-trips identically
            // so English-only games stay byte-equivalent, while JP games
            // (CPW, WW, AOB, etc.) get proper text rendering instead of
            // mojibake. Opt-OUT via FM2K_NO_JP_LOCALE=1 for diagnosis.
            // The legacy FM2K_JP_LOCALE env var is now a no-op (still
            // tolerated so older configs don't error).
            //
            // MUST run before host CRT init — i.e. before ResumeThread fires
            // the game's main thread. DllMain in our injection thread is
            // the right place.
            {
                const char* env_off = std::getenv("FM2K_NO_JP_LOCALE");
                const bool disabled = (env_off && std::strcmp(env_off, "1") == 0);
                if (!disabled) {
                    InstallLocaleSpoof();
                } else {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                "LocaleSpoof: DISABLED via FM2K_NO_JP_LOCALE=1");
                }
            }

            // FM2K-only binary patches. The literal addresses (0x405d05,
            // 0x409CD9, 0x470058, 0x4049e0/e6) are FM2K-engine-specific —
            // applying them to FM95 (CPW) just stomps random bytes in its
            // address space and breaks the host. Gate on the engine flag
            // so the FM95 build is a no-op here.
            if constexpr (FM2K::kIsFM2K) {
                BypassMultiInstanceCheck();
                ApplyBootToCharacterSelectPatches();
                ApplyCharacterSelectModePatches();
                DisableCursorHiding();
            }
            // Neuter F4 / Alt+Enter game-side fullscreen toggles when
            // cnc-ddraw is loaded — its keyboard hook owns those keys
            // now, the game's redundant toggle would fight cnc-ddraw's
            // render state. Detection: 2DFMD.dll is present in the
            // process iff the launcher applied the IAT redirect at
            // suspended-process start. Engine-aware: FM2K patches F4 +
            // Alt+Enter, FM95 (CPW) patches Alt+Enter only (no F4
            // binding in CPW's WndProc).
            if (GetModuleHandleA("2DFMD.dll") != nullptr) {
                NeuterFullscreenTogglesForCncDdraw();
            }
            // [DISABLED] t4-walk patch was a workaround that masked the
            // real bug — StudioS chars take script-driven damage on the
            // very first frame of battle (HW write bp on g_p1_hp caught
            // health_damage_manager+0x85 zeroing HP without input).
            // Patching the round controller's t4 transition only stops
            // the bail-to-CSS but lets the underlying damage corruption
            // continue. Re-enable only behind FM2K_FORCE_T4_PATCH=1
            // for diagnostic comparisons.
            if (const char* env_force = std::getenv("FM2K_FORCE_T4_PATCH");
                env_force && std::strcmp(env_force, "1") == 0)
            {
                PatchVsRoundCase200T4FalsePositive();
            }

            // (Console + file logging are set up at the top of DLL_PROCESS_ATTACH
            // so locale-spoof and patch logs land in the file too.)

            // Initialize shared memory for launcher status reporting
            if (!InitializeSharedMemory()) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "SharedMem: Init failed (non-fatal)");
            }

            // Auto-capture banner pipeline. No-op unless
            // FM2K_AUTO_CAPTURE=1 + FM2K_CAPTURE_DIR=<path> are set
            // in env (= the launcher's banner-capture session). When
            // active, main_loop_trampoline emits screenshots at
            // intro / title / CSS / battle phase boundaries.
            FM2KCapture::Init();

            // Parse network config from environment
            char* env_offline = getenv("FM2K_TRUE_OFFLINE");
            g_offline_mode = (env_offline && strcmp(env_offline, "1") == 0);

            // Stress-test mode: single-instance GekkoStressSession determinism check.
            // Mutually exclusive with true offline (stress still needs GekkoNet active).
            char* env_stress = getenv("FM2K_STRESS_MODE");
            g_stress_mode = (env_stress && strcmp(env_stress, "1") == 0);
            if (g_stress_mode) {
                g_offline_mode = false;  // stress requires GekkoNet session to be alive
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Stress-test mode ENABLED (GekkoStressSession): "
                    "single-instance determinism check, no network");
            }

            // Spectator mode: passive viewer subscribing to a remote host.
            char* env_spectator = getenv("FM2K_SPECTATOR_MODE");
            g_spectator_mode = (env_spectator && strcmp(env_spectator, "1") == 0);
            if (g_spectator_mode) {
                g_offline_mode = false;
                g_stress_mode  = false;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Spectator mode ENABLED — will SPEC_JOIN_REQ host on startup");
            }

            if (g_spectator_mode) {
                // Spectator path: socket + control callback + SpectatorNode.
                // Skips the player-mode HELLO handshake.
                char* env_port   = getenv("FM2K_LOCAL_PORT");
                char* env_remote = getenv("FM2K_REMOTE_ADDR");
                g_local_port = env_port ? (uint16_t)atoi(env_port) : 7002;
                if (env_remote) {
                    strncpy(g_remote_addr, env_remote, sizeof(g_remote_addr) - 1);
                } else {
                    snprintf(g_remote_addr, sizeof(g_remote_addr), "127.0.0.1:7000");
                }
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Spectator config: port=%u host=%s", g_local_port, g_remote_addr);
                if (!Netplay_InitAsSpectator(g_local_port, g_remote_addr)) {
                    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                                 "Spectator init failed — falling back to offline");
                    g_spectator_mode = false;
                    g_offline_mode   = true;
                }
            } else if (!g_offline_mode && !g_stress_mode) {
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
            } else if (g_stress_mode) {
                // Stress-test mode: no network, no socket, no control channel.
                // GekkoStressSession runs single-instance with both players local.
                // The battle-mode handler in Hook_UpdateGameState will call
                // Netplay_StartStressBattle() directly when game enters battle,
                // bypassing the remote-peer sync barrier.
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Stress mode - skipping socket init, GekkoStressSession will start at battle entry");
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

        // Locale spoof teardown (idempotent if not installed).
        UninstallLocaleSpoof();

        // Shutdown in reverse order
        ShutdownHooks();

        // Tear down GDI+ for the auto-capture writer (no-op when
        // capture wasn't active this run).
        FM2KCapture::Shutdown();

        // Cleanup shared memory
        CleanupSharedMemory();

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
