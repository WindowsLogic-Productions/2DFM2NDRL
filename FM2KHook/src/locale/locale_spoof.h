#pragma once

// Locale-spoofing for Japanese-only games (CPW.exe, FM95-era titles, some FM2K
// games that ship with Shift-JIS strings/fonts). Reimplements the minimum
// useful subset of Locale-Emulator (https://github.com/xupefei/Locale-Emulator)
// without requiring the user to launch through LE manually.
//
// Activation: install hooks at DLL_PROCESS_ATTACH if the launcher passed
// FM2K_JP_LOCALE=1 in the environment, OR if this is the FM95 build (always-on
// for FM95 since CPW won't even render Shift-JIS title without it).
//
// Hooks installed: GetACP, GetOEMCP, GetUserDefaultLCID, GetSystemDefaultLCID,
// GetUserDefaultUILanguage, GetSystemDefaultUILanguage, GetCPInfo,
// IsValidCodePage, GetThreadLocale, GetLocaleInfoA/W, EnumSystemLocalesA.
// All return Japanese (CP 932 / LCID 0x411) regardless of system settings.
//
// Returns true if hooks installed successfully (or already installed).
// Hook timing: must run BEFORE the host CRT initializes its locale cache.
// Since FM2KHook injects pre-ResumeThread, DllMain is the right call site.
bool InstallLocaleSpoof();

// Idempotent uninstall, called from DLL_PROCESS_DETACH.
void UninstallLocaleSpoof();
