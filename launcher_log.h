#pragma once

// fm2k::launcher_log -- always-on disk log for the LAUNCHER process itself,
// written as launcher.log next to FM2K_RollbackLauncher.exe.
//
// Why this exists: the launcher previously kept logs only in the in-memory
// ImGui console + the chained SDL console logger, and release builds redirect
// stdout/stderr to NUL (see SDL_AppInit). So when the launcher "opens, shows
// the window, then closes" there was nothing on disk to ask a tester for.
// This sink fixes that -- every SDL_Log line lands in launcher.log, and an
// unhandled-exception filter records the faulting module + offset so a hard
// crash leaves a usable breadcrumb in the same file.
//
// Design notes:
//   - Raw Win32 HANDLE + WriteFile (not std::ofstream): WriteFile goes straight
//     to the OS cache, so a process crash does NOT lose buffered lines -- no
//     per-line flush needed. That property is what makes it a viable CRASH log.
//   - Best-effort: a read-only install dir (or any open failure) silently
//     disables the sink. Logging must never be able to crash the launcher.
//   - PII-scrubbed: lines run through fm2k::pii::ScrubInto before hitting disk,
//     same redaction the in-UI console uses. Call fm2k::pii::Init() before Init.
//   - Rotates the previous run to launcher.prev.log on each Init().

#include <SDL3/SDL_log.h>

namespace fm2k::launcher_log {

// Open <exe_dir>\launcher.log (rotating any prior run to launcher.prev.log).
// Idempotent. Best-effort: silently no-ops if the path can't be opened.
void Init();

// Close the file. Safe if Init never ran / failed.
void Shutdown();

// SDL_SetLogOutputFunction sink. Scrubs PII then appends one timestamped line.
// Install in SDL_AppInit so the whole startup is captured; LauncherUI::Init's
// SDLCustomLogOutput chains to this (SDL_GetLogOutputFunction captures it as
// its "original"), so UI-era lines reach the file too.
void SdlLogOutput(void* userdata, int category, SDL_LogPriority priority,
                  const char* message);

// Install the process-wide failure handlers, each of which writes a one-line
// breadcrumb to the log before the process dies. Covers the THREE distinct ways
// the launcher can vanish, so a "shows then closes" report always leaves a
// reason on disk:
//   [CRASH]     -- SetUnhandledExceptionFilter: SEH fault (access violation),
//                  records faulting module+offset, then chains to WER.
//   [TERMINATE] -- std::set_terminate: an uncaught C++ exception (the SEH
//                  filter's blind spot), records the exception's what().
//   [CRT]       -- invalid-parameter / pure-virtual CRT fast-fail paths.
// If a "closes" log ends with NONE of these (and no clean-shutdown line), the
// process was killed from outside (antivirus / SmartScreen / Task Manager).
// Install once, early.
void InstallCrashHandler();

}  // namespace fm2k::launcher_log
