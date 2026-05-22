// Clean DLL entry point with persistent network connection
#include "globals.h"
#include "hooks.h"
#include "netplay.h"
#include "netplay_state.h"
#include "control_channel.h"
#include "shared_mem.h"
#include "../ui/screenshot.h"
#include "../locale/locale_spoof.h"
#include "../util/pii_scrub.h"

// Forward-declare just the new patch function. We can't include
// game_patches.h here because dllmain.cpp keeps static duplicates of
// the older patch functions and the extern declarations would conflict.
extern void PatchVsRoundCase200T4FalsePositive();
extern void NeuterFullscreenTogglesForCncDdraw();

// Per-game patches that delegate to css_autoconfirm.cpp's hook (already
// detoured on game_state_manager — sharing the same MinHook entry).
#include "../hooks/css_autoconfirm.h"
#include "../hooks/round_events.h"
#include "../hooks/per_game_patches.h"
#include "../netplay/upload_queue.h"
#include "../netplay/spectator_node.h"  // SpectatorNode_GetSessionId
#include "version_local.h"  // fm2k::kAppBranch / kAppVersion / kAppRevision
#include <SDL3/SDL_log.h>
#include <windows.h>
#include <mmsystem.h>
#include <psapi.h>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <ctime>
#include <cctype>
#include <string>

// Quill — async logging backend. SDL_Log call sites stay unchanged; our
// SDLCustomLogOutput function (LogOutputFunction below) routes the formatted
// message into a quill Logger which the backend thread writes to disk
// asynchronously. Fixes the "fprintf + fflush per log line" stall that was
// noticeable on the spectator under SPEC-FP / ROLLBACK-stats cadence.
#include "quill/Backend.h"
#include "quill/Frontend.h"
#include "quill/LogMacros.h"
#include "quill/Logger.h"
#include "quill/sinks/FileSink.h"
#include "quill/core/PatternFormatterOptions.h"

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
// FILE LOGGING (quill async backend)
// =============================================================================
//
// Pre-quill: each SDL_Log call → LogOutputFunction → fprintf(g_log_file,
// ...) + fflush(). Synchronous file I/O on the sim thread; under heavy
// log volume (~3 lines/sec SPEC-FP plus ROLLBACK stats plus SoundRollback
// chatter) the per-line WriteFile + fflush adds up to visible jitter.
//
// Now: SDL_Log → LogOutputFunction formats the line → LOG_INFO into a
// quill Logger. Quill's backend thread drains the queue and writes to
// disk async on its own thread, so the sim never blocks on disk.
//
// We use a custom PatternFormatter ("%(message)") that emits the raw
// formatted message verbatim — no quill-prefixed timestamp/level dup,
// since LogOutputFunction already builds the "[hh:mm:ss.mmm] [P1] [INFO]"
// prefix in the same shape the user/CI grep tools expect.
static quill::Logger* g_quill_logger = nullptr;

// Fallback FILE* for the case where quill init fails (rare — only seen on
// some 32-bit injected DLL paths where the backend thread can't spawn).
// LogOutputFunction prefers g_quill_logger when non-null; falls through
// to this synchronous-but-reliable file write otherwise. Bug v0.2.32:
// silent quill init failures left users with no log file at all on
// game launch. Belt-and-suspenders fallback added v0.2.33.
static FILE* g_log_file_fallback = nullptr;

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

    // PII scrub on the message body BEFORE we splice it into the
    // output line. We don't scrub the whole formatted string because
    // the prefix (`[hh:mm:ss.mmm] [P1] [INFO]`) has no user data and
    // running regex on a smaller buffer is cheaper.
    char scrubbed_msg[2048];
    fm2k::pii::ScrubInto(message ? message : "",
                         scrubbed_msg, sizeof(scrubbed_msg));

    // Format: [HH:MM:SS.mmm] [PRIORITY] message
    char formatted[2048];
    snprintf(formatted, sizeof(formatted), "[%02d:%02d:%02d.%03d] [P%d] [%s] %s\n",
             st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
             g_player_index + 1, priority_str, scrubbed_msg);

    // File-only by default — fputs to stdout is synchronous and lags the
    // sim under heavy log volume (SoundRollback / ROLLBACK stats /
    // BATTLE STATUS / SPEC-FP cadence). Spectator instances historically
    // mirrored to console for live protocol tracing; gated now behind
    // FM2K_SPECTATOR_DEBUG=1 so release-mode spectators stay quiet (per-
    // session log file is still on disk for post-hoc inspection).
    if (g_spectator_mode) {
        static int s_console_mirror_env = -1;
        if (s_console_mirror_env < 0) {
            const char* v = std::getenv("FM2K_SPECTATOR_DEBUG");
            s_console_mirror_env =
                (v && v[0] == '1' && v[1] == '\0') ? 1 : 0;
        }
        if (s_console_mirror_env == 1) {
            fputs(formatted, stdout);
            fflush(stdout);
        }
    }

    // Quill async path: one snprintf'd line per call, hands off to the
    // backend thread for actual disk I/O. Pattern is "%(message)" so the
    // line lands in the file verbatim — same byte stream as the prior
    // fprintf path, just async. Strip the trailing \n we added since
    // quill always appends one of its own.
    if (g_quill_logger) {
        size_t len = std::strlen(formatted);
        if (len > 0 && formatted[len - 1] == '\n') {
            formatted[len - 1] = '\0';
        }

        // Diagnostic backtrace routing.
        //
        // SDL_LOG_CATEGORY_CUSTOM (and beyond) marks high-frequency
        // diagnostic lines that we want CAPTURED but not always WRITTEN.
        // Currently used by [SPEC-FP] / [HOST-FP] / [SPEC-TRACE] /
        // [HOST-TRACE] / pb_queue=N (~3 lines/sec each). Quill's
        // backtrace ring (init_backtrace below) buffers the last 4096
        // such lines in memory; LOG_ERROR through any channel auto-
        // flushes the ring to disk just before the error line, giving
        // us "diag-armed forever, zero disk IO until something breaks."
        //
        // FM2K_SPECTATOR_DEBUG=1 routes diag straight to disk like a
        // normal log line — keep the existing opt-in path for users
        // who want continuous capture during a debug session.
        //
        // Routing for non-CUSTOM (regular APPLICATION) categories: WARN
        // and below go to LOG_INFO (file); ERROR/CRITICAL go to LOG_ERROR
        // (file + auto-flush backtrace).
        if (category >= SDL_LOG_CATEGORY_CUSTOM) {
            static int s_spec_diag_to_file = -1;
            if (s_spec_diag_to_file < 0) {
                const char* v = std::getenv("FM2K_SPECTATOR_DEBUG");
                s_spec_diag_to_file =
                    (v && v[0] == '1' && v[1] == '\0') ? 1 : 0;
            }
            if (s_spec_diag_to_file == 1) {
                LOG_INFO(g_quill_logger, "{}", formatted);
            } else {
                LOG_BACKTRACE(g_quill_logger, "{}", formatted);
            }
        } else if (priority >= SDL_LOG_PRIORITY_ERROR) {
            // ERROR / CRITICAL — flushes the backtrace ring as a side
            // effect of init_backtrace's flush_level being set to Error.
            LOG_ERROR(g_quill_logger, "{}", formatted);
        } else {
            LOG_INFO(g_quill_logger, "{}", formatted);
        }
    } else if (g_log_file_fallback) {
        // Quill init failed — write directly. Re-add the trailing newline
        // since the LOG_BACKTRACE path strips it but fprintf needs it back.
        size_t len = std::strlen(formatted);
        bool had_newline = (len > 0 && formatted[len - 1] == '\n');
        fputs(formatted, g_log_file_fallback);
        if (!had_newline) fputc('\n', g_log_file_fallback);
        fflush(g_log_file_fallback);
    }
}

static void InitFileLogging() {
    // Capture USERNAME etc. before the first SDL_Log call hits the
    // scrubber. Idempotent — the launcher's logger init also calls this.
    fm2k::pii::Init();

    char base_name[64];
    snprintf(base_name, sizeof(base_name), "FM2K_P%d_Debug.log", g_player_index + 1);
    char filename[MAX_PATH];
    if (!Fm2k_BuildLogPath(filename, sizeof(filename), base_name)) {
        // Fallback to cwd if the helper failed (extremely unlikely — only
        // CreateDirectoryA would have to fail with something other than
        // ERROR_ALREADY_EXISTS, e.g. permission denied).
        snprintf(filename, sizeof(filename), "%s", base_name);
    }

    // v0.2.33: quill init removed from the hook DLL.
    //
    // Why: InitFileLogging runs from DLL_PROCESS_ATTACH, which Windows
    // serializes under the loader lock. quill::Backend::start spawns a
    // std::thread for its writer; CreateThread blocks until DllMain
    // returns, deadlocking the loader. Symptom in v0.2.32: AllocConsole
    // (a few lines above) succeeds → "FM2K P*N* Console" window appears →
    // hook hangs in Backend::start → game's WinMain never resumes → no
    // game window, no log file. try/catch can't help: it's a hang, not a
    // throw.
    //
    // The launcher (which inits quill from main(), not DllMain) is fine.
    // The hook just uses synchronous fopen below — same as v0.2.31 and
    // earlier. Quill stays vendored; if we want it back in the hook a
    // future commit can defer init to a worker spawned AFTER DllMain
    // returns (e.g. on first frame-hook callback).
    bool quill_ok = false;
    g_quill_logger = nullptr;

    if (!quill_ok) {
        g_log_file_fallback = fopen(filename, "w");
        if (g_log_file_fallback) {
            SYSTEMTIME st;
            GetLocalTime(&st);
            fprintf(g_log_file_fallback,
                    "=== FM2K Hook Debug Log - Player %d ===\n",
                    g_player_index + 1);
            fprintf(g_log_file_fallback,
                    "Session started: %04d-%02d-%02d %02d:%02d:%02d\n",
                    st.wYear, st.wMonth, st.wDay,
                    st.wHour, st.wMinute, st.wSecond);
            fprintf(g_log_file_fallback,
                    "==========================================\n");
            fflush(g_log_file_fallback);
        }
    }

    // Wire SDL's logger to dispatch through our function so each
    // SDL_LogInfo / Warn / Error call site (~150 across the hook) reaches
    // the quill backend without changing call-site code.
    SDL_SetLogOutputFunction(LogOutputFunction, nullptr);
}

// Crash dumper — last-chance handler that records faulting address +
// exception code + a hex stack-backtrace to the same log file the rest
// of the hook writes to. No DbgHelp dependency: we capture raw frame
// addresses (RtlCaptureStackBackTrace) and dump the module name + RVA
// for each so you can post-process against the IDA / .map / .pdb later.
//
// Why this exists: alt-tab during init or modal title-drag mid-boot
// occasionally crashes the game right after the main pump resumes.
// Without a stack we can only guess at root cause (D3D9 device-lost,
// rollback catchup explosion, ImGui state mismatch — all plausible).
// This handler turns the next incident into a concrete trace.
static LONG WINAPI CrashHandler(EXCEPTION_POINTERS* ep) {
    FILE* f = g_log_file_fallback;  // shared with normal logging
    if (!f) {
        // Last-resort fallback — open a dedicated crash file in case the
        // main log handle is lost. Same logs/ dir.
        char path[MAX_PATH];
        char base[64];
        std::snprintf(base, sizeof(base),
                      "FM2K_P%d_Crash.log", g_player_index + 1);
        if (Fm2k_BuildLogPath(path, sizeof(path), base)) {
            f = fopen(path, "w");
        }
    }
    if (!f) return EXCEPTION_CONTINUE_SEARCH;

    SYSTEMTIME st;
    GetLocalTime(&st);
    DWORD code = ep && ep->ExceptionRecord
                 ? ep->ExceptionRecord->ExceptionCode : 0;
    void* addr = ep && ep->ExceptionRecord
                 ? ep->ExceptionRecord->ExceptionAddress : nullptr;

    fprintf(f,
        "\n=== CRASH at %02d:%02d:%02d.%03d (P%d) ===\n"
        "ExceptionCode    = 0x%08lX\n"
        "ExceptionAddress = %p\n",
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
        g_player_index + 1,
        (unsigned long)code, addr);

    if (ep && ep->ContextRecord) {
        const CONTEXT* c = ep->ContextRecord;
        fprintf(f,
            "EIP=0x%08lX EAX=0x%08lX EBX=0x%08lX ECX=0x%08lX EDX=0x%08lX\n"
            "ESI=0x%08lX EDI=0x%08lX EBP=0x%08lX ESP=0x%08lX\n",
            (unsigned long)c->Eip, (unsigned long)c->Eax,
            (unsigned long)c->Ebx, (unsigned long)c->Ecx,
            (unsigned long)c->Edx, (unsigned long)c->Esi,
            (unsigned long)c->Edi, (unsigned long)c->Ebp,
            (unsigned long)c->Esp);

        // ExceptionInformation[0] = 0 read, 1 write, 8 DEP.
        // ExceptionInformation[1] = the inaccessible address. Critical
        // for AVs — tells us "tried to {read,write} <addr>".
        if (code == EXCEPTION_ACCESS_VIOLATION &&
            ep->ExceptionRecord->NumberParameters >= 2) {
            ULONG_PTR rw   = ep->ExceptionRecord->ExceptionInformation[0];
            ULONG_PTR vbad = ep->ExceptionRecord->ExceptionInformation[1];
            fprintf(f, "AV: %s 0x%08lX\n",
                    rw == 0 ? "read" : rw == 1 ? "write" : "exec",
                    (unsigned long)vbad);
        }
    }

    // Backtrace: RtlCaptureStackBackTrace gives us up to 62 frames on
    // 32-bit Windows. Walk and resolve each to module+RVA via
    // GetModuleHandleEx with FROM_ADDRESS (no DbgHelp needed).
    void* frames[64] = {};
    USHORT n = RtlCaptureStackBackTrace(0, 64, frames, nullptr);
    fprintf(f, "Backtrace (%u frames):\n", (unsigned)n);
    for (USHORT i = 0; i < n; ++i) {
        HMODULE mod = nullptr;
        char modname[MAX_PATH] = {0};
        ULONG_PTR rva = 0;
        if (GetModuleHandleExA(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                (LPCSTR)frames[i], &mod) && mod) {
            char fullpath[MAX_PATH] = {0};
            if (GetModuleFileNameA(mod, fullpath, sizeof(fullpath))) {
                const char* p = strrchr(fullpath, '\\');
                std::snprintf(modname, sizeof(modname),
                              "%s", p ? p + 1 : fullpath);
            }
            rva = (ULONG_PTR)frames[i] - (ULONG_PTR)mod;
        }
        fprintf(f, "  [%02u] %p  %s+0x%08lX\n",
                (unsigned)i, frames[i],
                modname[0] ? modname : "?",
                (unsigned long)rva);
    }
    fprintf(f, "=== END CRASH ===\n\n");
    fflush(f);

    // Drop an upload manifest so the launcher posts the crash artifact
    // to the hub on next launch (the launcher polls upload_queue/
    // every tick — see PollUploadQueue). We're inside an unhandled
    // exception filter so the heap may be unstable; the std::vector
    // + std::string allocations Enqueue does are small (~520 bytes
    // worst case) and use the same allocator the existing fprintf
    // calls above already touched without crashing, so the marginal
    // risk is low. If it ever blows up in the wild we can switch to
    // an inline fprintf-only manifest writer here.
    {
        char cwd[MAX_PATH] = {};
        GetCurrentDirectoryA(sizeof(cwd), cwd);
        char debug_path[MAX_PATH], crash_path[MAX_PATH];
        std::snprintf(debug_path, sizeof(debug_path),
                      "%s\\logs\\FM2K_P%d_Debug.log",
                      cwd, g_player_index + 1);
        std::snprintf(crash_path, sizeof(crash_path),
                      "%s\\logs\\FM2K_P%d_Crash.log",
                      cwd, g_player_index + 1);

        char exe_path[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, exe_path, sizeof(exe_path));
        const char* basename = exe_path;
        for (char* p = exe_path; *p; ++p) {
            if (*p == '\\' || *p == '/') basename = p + 1;
        }
        char game_id[128] = {};
        std::strncpy(game_id, basename, sizeof(game_id) - 1);
        if (char* dot = std::strrchr(game_id, '.')) *dot = '\0';

        fm2k::upload_queue::Manifest m;
        m.kind = "crash";
        m.frame = -1;  // unknown — crashes can fire from any phase
        m.session_id = SpectatorNode_GetSessionId();
        m.player_index = g_player_index;
        m.game_id = game_id;
        // Reference both candidate log paths. The launcher's processor
        // silently skips paths that don't exist, so listing both lets
        // it pick up whichever one CrashHandler actually wrote to
        // (fallback path used Crash.log; primary path appended to
        // Debug.log via the shared g_log_file_fallback handle).
        m.file_paths.emplace_back(debug_path);
        m.file_paths.emplace_back(crash_path);
        fm2k::upload_queue::Enqueue(m);
    }

    return EXCEPTION_CONTINUE_SEARCH;  // let Windows' default handler
                                       // pop the WER dialog as usual
}

// Vectored exception handler for known vanilla-game render bugs that we
// can't (yet) prevent at the source.
//
// Currently catches: AV inside sprite_rendering_engine @ 0x40CC30 reading
// from `[g_effect_character_data_base + 0x114] + frame_idx*16 - 0x10`.
// Repro: alt-tab / drag title bar of one launcher instance during the CSS
// demo phase; the OTHER instance occasionally trips this AV. Stack
// register dump showed EDI=3 (case 3 = effect render path), EBX pointing
// at g_effect_character_data_base, EDX = a near-page-aligned heap-ish
// pointer that didn't extend 3 more bytes for the `mov ax, [edx+3]` read.
// Likely a vanilla edge case where an effect's sprite-table pointer is
// either uninitialized or off-by-one against the loaded sprite stream.
//
// Recovery strategy: jump EIP to a known full-epilogue position inside
// the function (0x40D7A8: `pop edi; pop esi; pop ebp; pop ebx; add esp,
// 0x6C; ret`). At the crash address the function has only executed its
// prologue + a handful of loads — no extra stack pushes — so the epilogue
// unwinds cleanly to the caller. Effect renders for that one frame are
// skipped; game keeps running.
//
// Throttle: log first 3 recoveries, then 1-per-1000 thereafter, to avoid
// blasting the log if the crash repeats every render frame.
static LONG WINAPI VectoredRenderGuard(EXCEPTION_POINTERS* ep) {
    if (!ep || !ep->ExceptionRecord || !ep->ContextRecord) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    constexpr DWORD kSpriteEngineStart    = 0x0040CC30;
    constexpr DWORD kSpriteEngineEnd      = 0x0040E3E8;  // start + 0x17B8
    constexpr DWORD kSpriteEngineEpilogue = 0x0040D7A8;
    constexpr DWORD kKnownCrashSite       = 0x0040CCAE;  // mov ax, [edx+3]

    CONTEXT* c = ep->ContextRecord;
    if (c->Eip < kSpriteEngineStart || c->Eip >= kSpriteEngineEnd) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    // Only recover at the known crash site for now — broad jump-to-epilogue
    // from arbitrary EIPs inside the function risks unwinding past
    // mid-call stack pushes (later in the function). Tightens the
    // recovery to "the exact frame-decode AV we've observed" and lets
    // any new crash pattern surface as a real fault we can dump.
    if (c->Eip != kKnownCrashSite) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    static std::atomic<uint32_t> s_count{0};
    const uint32_t n = s_count.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 3 || (n % 1000) == 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "VectoredRenderGuard: recovered AV at 0x%08lX in sprite_rendering_engine "
            "(occurrence #%u, EDX=0x%08lX) — skipping render via epilogue jump",
            (unsigned long)c->Eip, (unsigned)n, (unsigned long)c->Edx);
    }
    c->Eip = kSpriteEngineEpilogue;
    return EXCEPTION_CONTINUE_EXECUTION;
}

static void InstallCrashHandler() {
    // Vectored handlers run BEFORE SEH-style filters and BEFORE the
    // unhandled-exception filter, so they get first crack at recovering
    // known-bad code paths. SetUnhandledExceptionFilter remains as the
    // last-resort dumper for everything we don't recover.
    AddVectoredExceptionHandler(/*FirstHandler=*/1, VectoredRenderGuard);
    SetUnhandledExceptionFilter(CrashHandler);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "CrashHandler: installed (vectored render-guard + last-resort dumper)");
}

static void ShutdownFileLogging() {
    if (g_quill_logger) {
        LOG_INFO(g_quill_logger, "{}", "=== Session ended ===");
        // Don't stop the backend thread here — quill's Backend::start
        // installs a std::atexit handler that drains + stops cleanly on
        // normal process exit. Stopping early could swallow pending log
        // lines from late shutdown paths (gekko_destroy etc).
        g_quill_logger = nullptr;
    }
    if (g_log_file_fallback) {
        fprintf(g_log_file_fallback, "=== Session ended ===\n");
        fclose(g_log_file_fallback);
        g_log_file_fallback = nullptr;
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

// Runtime fix for the GameSpeed (Editor.TestPlay.GameSpeed INI key) setting
// breaking character / effect / VS-intro scripts at any value other than 10.
//
// Root cause (init_round_state_and_apply_gamespeed @ 0x406450):
//
//   v3 = (gs <= 10) ? (5*gs + 50) : (10*gs);   // gs=10 -> 100, gs=5 -> 75
//   g_gamespeed_move_scalar  (0x541F78) = 0x10000 / v3
//   g_gamespeed_accel_scalar (0x445700) = 3932160 / (v3*v3)
//   g_gamespeed_pic_step     (0x445704) = v3
//
// op_MOVE in the script interpreter (character_state_machine @ 0x411bf0)
// uses MOVE/ACCEL scalars to scale velocity per frame — fine. But op_PIC
// (case 0x0C) ADDS `v3 * keepTime` to a per-object wait timer that gets
// DECREMENTED by a HARDCODED 100 per frame at the top of the dispatch
// loop. At gs=10 the two cancel exactly; at any other gs they don't:
//
//   keepTime  gs=10 frames  gs=5 frames
//      1          2             1        (2x faster)
//      2          3             2        (1.5x)
//      3          4             3        (1.33x)
//
// Small waits collapse disproportionately and desync against unscaled
// systems (hitstop, hurtbox lifetimes, BG_FX timers, g_game_timer).
// Vanpri-style scripts with tight OP_PIC chains hit the dispatch loop's
// 300-iter safety break and leave objects stuck.
//
// Fix: NOP the `mov [g_gamespeed_pic_step], ecx` write at 0x40649A and
// pin g_gamespeed_pic_step = 100. MOVE/ACCEL scalars still vary with gs
// (so the game still feels faster at lower gs) but the script-time clock
// stays exact.
//
// Opt-out: FM2K_KEEP_GAMESPEED_PIC=1 leaves vanilla behavior for re-test.
static void FixGameSpeedDesync() {
    if (const char* e = std::getenv("FM2K_KEEP_GAMESPEED_PIC"); e && std::strcmp(e, "1") == 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "PATCH: FM2K_KEEP_GAMESPEED_PIC=1 — leaving vanilla "
            "gs-pic-timer behavior (gs != 10 will desync scripts).");
        return;
    }

    // NOP the 6-byte write `mov ds:[0x445704], ecx` at 0x40649A
    // (89 0D 04 57 44 00 -> 90 90 90 90 90 90).
    uint8_t* pic_write = (uint8_t*)0x40649A;
    DWORD old_protect;
    if (VirtualProtect(pic_write, 6, PAGE_EXECUTE_READWRITE, &old_protect)) {
        std::memset(pic_write, 0x90, 6);
        VirtualProtect(pic_write, 6, old_protect, &old_protect);
    }

    // Pin g_gamespeed_pic_step = 100 so OP_PIC behaves as if gs=10 from
    // frame 0 onward (init_round_state_and_apply_gamespeed no longer
    // overwrites it, but we still need a defined initial value).
    uint32_t* pic_step = (uint32_t*)0x445704;
    if (VirtualProtect(pic_step, 4, PAGE_READWRITE, &old_protect)) {
        *pic_step = 100;
        VirtualProtect(pic_step, 4, old_protect, &old_protect);
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "Patch: GameSpeed PIC desync fix applied "
        "(pic_step pinned to 100; MOVE/ACCEL still scale with gs)");
}

// Configure per-game patches that live as runtime flags inside other
// hook modules (rather than byte patches). Reads env vars set by the
// launcher's ApplyGamePatchEnvVars before CreateProcess and forwards
// them to the owning module.
static void ApplyPerGameRuntimePatches() {
    // Team-mode CSS duplicate-slot lock — handled inside
    // css_autoconfirm.cpp's existing game_state_manager detour.
    if (const char* e = std::getenv("FM2K_TEAM_CSS_DUPE_LOCK");
        e && std::strcmp(e, "1") == 0) {
        CssAutoConfirm_SetTeamDupeLock(true);
    }

    // KOF-style HP/meter retention — handled inside round_events.cpp's
    // vs_round_function detour.
    if (const char* e = std::getenv("FM2K_TEAM_KOF_RETENTION");
        e && std::strcmp(e, "1") == 0) {
        RoundEvents_SetKofRetention(true);
    }

    // Team size override + stubs (vs_cpu / cpu_vs_cpu / training /
    // option_mode_selector). The damage multiplier MinHook is installed
    // separately from InitializeHooks (it needs MH_Initialize to have
    // run already).
    PerGamePatches_ApplyRuntime();
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
            //
            // Console window is gated on FM2K_DEV_MODE=1 or
            // FM2K_SPECTATOR_DEBUG=1 — release-mode users don't want a
            // stray "FM2K P*N* Console" window next to their game. Log
            // file is always written either way.
            {
                const char* dev = std::getenv("FM2K_DEV_MODE");
                const char* spec_dbg = std::getenv("FM2K_SPECTATOR_DEBUG");
                bool want_console =
                    (dev && dev[0] == '1' && dev[1] == '\0') ||
                    (spec_dbg && spec_dbg[0] == '1' && spec_dbg[1] == '\0');
                if (want_console) {
                    FreeConsole();
                    AllocConsole();
                    FILE* fDummy;
                    freopen_s(&fDummy, "CONOUT$", "w", stdout);
                    freopen_s(&fDummy, "CONOUT$", "w", stderr);
                    freopen_s(&fDummy, "CONIN$", "r", stdin);
                    char title[64];
                    std::snprintf(title, sizeof(title),
                                  "FM2K P%d Console", g_player_index + 1);
                    SetConsoleTitleA(title);
                }
            }
            InitFileLogging();
            InstallCrashHandler();
            SDL_SetLogPriorities(SDL_LOG_PRIORITY_INFO);
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "=== FM2K Hook Starting (Player %d) ===", g_player_index + 1);
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "FM2KHook build: %s %s rev %s (%s)",
                fm2k::kAppBranch, fm2k::kAppVersion,
                fm2k::kAppRevision, fm2k::kAppBuildTime);

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
                FixGameSpeedDesync();
                ApplyPerGameRuntimePatches();
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

            // Spectator mode: passive viewer subscribing to a remote host
            // OR offline-replay file player. Both share the same hook init
            // path (SpectatorNode + pb_queue drain), but the wire/log
            // messaging differs — replay has no network, no peer, no
            // JOIN_REQ. Distinguish via FM2K_REPLAY_FILE.
            char* env_spectator = getenv("FM2K_SPECTATOR_MODE");
            g_spectator_mode = (env_spectator && strcmp(env_spectator, "1") == 0);
            const char* env_replay = getenv("FM2K_REPLAY_FILE");
            const bool is_offline_replay =
                g_spectator_mode && env_replay && env_replay[0];
            if (g_spectator_mode) {
                g_offline_mode = false;
                g_stress_mode  = false;
                if (is_offline_replay) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                "Replay mode ENABLED — playing %s (no network, no peer)",
                                env_replay);
                } else {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                "Spectator mode ENABLED — will SPEC_JOIN_REQ host on startup");
                }
            }

            if (g_spectator_mode) {
                // Spectator path: socket + control callback + SpectatorNode.
                // Replay mode short-circuits inside Netplay_InitAsSpectator
                // (no network setup), so the port/host config is unused but
                // we still pass valid placeholders to keep the call shape.
                char* env_port   = getenv("FM2K_LOCAL_PORT");
                char* env_remote = getenv("FM2K_REMOTE_ADDR");
                g_local_port = env_port ? (uint16_t)atoi(env_port) : 7002;
                if (env_remote) {
                    strncpy(g_remote_addr, env_remote, sizeof(g_remote_addr) - 1);
                } else {
                    snprintf(g_remote_addr, sizeof(g_remote_addr), "127.0.0.1:7000");
                }
                if (!is_offline_replay) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                "Spectator config: port=%u host=%s",
                                g_local_port, g_remote_addr);
                }
                if (!Netplay_InitAsSpectator(g_local_port, g_remote_addr)) {
                    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                                 "%s init failed — falling back to offline",
                                 is_offline_replay ? "Replay" : "Spectator");
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
