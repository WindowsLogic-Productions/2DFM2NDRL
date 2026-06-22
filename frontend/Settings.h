// frontend/Settings.h — flat-file settings.ini helpers shared by the new
// fm2k::shell render path and (later) the legacy launcher UI. Mirrors
// the static helpers at FM2K_LauncherUI.cpp:2049-2156 — kept additive
// for M1 (legacy keeps its copy; we'll consolidate in M5). Format is
// `key=value\n`, one key per line, `#` or `;` for comment lines.
//
// Path resolves to %APPDATA%\FM2K_Rollback\settings.ini, same file the
// legacy locale + notify + SOCD + random-stage code already writes.
#pragma once

#include <string>
#include <vector>

namespace fm2k::shell {

// %APPDATA%\FM2K_Rollback\settings.ini — creates the parent dir if
// missing. Returns empty string when APPDATA isn't set (CI / weird
// shells); callers should treat empty as "skip persistence."
std::string SettingsPath();

bool        ReadBool   (const std::string& path, const char* key, bool        dflt);
int         ReadInt    (const std::string& path, const char* key, int         dflt);
float       ReadFloat  (const std::string& path, const char* key, float       dflt);
std::string ReadString (const std::string& path, const char* key, const char* dflt);

void WriteBool  (const std::string& path, const char* key, bool                value);
void WriteInt   (const std::string& path, const char* key, int                 value);
void WriteFloat (const std::string& path, const char* key, float               value);
void WriteString(const std::string& path, const char* key, const std::string&  value);

// Comma-split / join helpers for list-valued keys (e.g. subscribed_rooms
// = "kof98,3s,kof02"). Trim whitespace around each item.
std::vector<std::string> SplitCsv (const std::string& csv);
std::string              JoinCsv  (const std::vector<std::string>& items);

}  // namespace fm2k::shell
