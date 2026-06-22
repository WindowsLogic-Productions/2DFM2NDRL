// frontend/Settings.cpp — see header. Implementation mirrors
// FM2K_LauncherUI.cpp:2049-2156 with one addition: ReadString /
// WriteString for the subscribed_rooms CSV. Brute-force read-modify-
// write is fine — settings.ini is tiny and writes are user-driven.
#include "Settings.h"

#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace fm2k::shell {

namespace {

// Read all kv pairs (preserves user comments by skipping them; we
// rewrite without them, which matches the legacy behavior — the
// settings.ini only ever contains key=value lines we wrote).
std::vector<std::pair<std::string, std::string>> LoadAllPairs(const std::string& path) {
    std::vector<std::pair<std::string, std::string>> kv;
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f) return kv;
    char line[512];
    while (std::fgets(line, sizeof(line), f)) {
        std::string s = line;
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r' ||
                              s.back() == ' '  || s.back() == '\t')) {
            s.pop_back();
        }
        if (s.empty() || s[0] == '#' || s[0] == ';') continue;
        const size_t eq = s.find('=');
        if (eq == std::string::npos) continue;
        kv.emplace_back(s.substr(0, eq), s.substr(eq + 1));
    }
    std::fclose(f);
    return kv;
}

void StoreAllPairs(const std::string& path,
                   const std::vector<std::pair<std::string, std::string>>& kv) {
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) return;
    for (const auto& p : kv) {
        std::fprintf(f, "%s=%s\n", p.first.c_str(), p.second.c_str());
    }
    std::fclose(f);
}

void Trim(std::string& s) {
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    if (i) s.erase(0, i);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
}

}  // namespace

std::string SettingsPath() {
    const char* a = std::getenv("APPDATA");
    if (!a || !*a) return "";
    std::string dir = std::string(a) + "\\FM2K_Rollback";
    CreateDirectoryA(dir.c_str(), nullptr);
    return dir + "\\settings.ini";
}

bool ReadBool(const std::string& path, const char* key, bool dflt) {
    auto kv = LoadAllPairs(path);
    for (const auto& p : kv) {
        std::string k = p.first; Trim(k);
        if (k != key) continue;
        std::string v = p.second; Trim(v);
        return (v == "1" || v == "true" || v == "yes" || v == "on");
    }
    return dflt;
}

int ReadInt(const std::string& path, const char* key, int dflt) {
    auto kv = LoadAllPairs(path);
    for (const auto& p : kv) {
        std::string k = p.first; Trim(k);
        if (k != key) continue;
        std::string v = p.second; Trim(v);
        char* endp = nullptr;
        long parsed = std::strtol(v.c_str(), &endp, 10);
        if (endp && endp != v.c_str()) return (int)parsed;
        return dflt;
    }
    return dflt;
}

float ReadFloat(const std::string& path, const char* key, float dflt) {
    auto kv = LoadAllPairs(path);
    for (const auto& p : kv) {
        std::string k = p.first; Trim(k);
        if (k != key) continue;
        std::string v = p.second; Trim(v);
        char* endp = nullptr;
        float parsed = std::strtof(v.c_str(), &endp);
        if (endp && endp != v.c_str()) return parsed;
        return dflt;
    }
    return dflt;
}

std::string ReadString(const std::string& path, const char* key, const char* dflt) {
    auto kv = LoadAllPairs(path);
    for (const auto& p : kv) {
        std::string k = p.first; Trim(k);
        if (k != key) continue;
        std::string v = p.second; Trim(v);
        return v;
    }
    return dflt ? std::string(dflt) : std::string();
}

void WriteBool(const std::string& path, const char* key, bool value) {
    auto kv = LoadAllPairs(path);
    bool found = false;
    for (auto& p : kv) {
        if (p.first == key) { p.second = value ? "1" : "0"; found = true; }
    }
    if (!found) kv.emplace_back(key, value ? "1" : "0");
    StoreAllPairs(path, kv);
}

void WriteInt(const std::string& path, const char* key, int value) {
    char buf[32]; std::snprintf(buf, sizeof(buf), "%d", value);
    auto kv = LoadAllPairs(path);
    bool found = false;
    for (auto& p : kv) {
        if (p.first == key) { p.second = buf; found = true; }
    }
    if (!found) kv.emplace_back(key, buf);
    StoreAllPairs(path, kv);
}

void WriteFloat(const std::string& path, const char* key, float value) {
    // %g keeps the round-trip readable (no trailing zeros) while still
    // surviving strtof. 9 sig digits covers float precision exactly.
    char buf[32]; std::snprintf(buf, sizeof(buf), "%.9g", value);
    auto kv = LoadAllPairs(path);
    bool found = false;
    for (auto& p : kv) {
        if (p.first == key) { p.second = buf; found = true; }
    }
    if (!found) kv.emplace_back(key, buf);
    StoreAllPairs(path, kv);
}

void WriteString(const std::string& path, const char* key, const std::string& value) {
    auto kv = LoadAllPairs(path);
    bool found = false;
    for (auto& p : kv) {
        if (p.first == key) { p.second = value; found = true; }
    }
    if (!found) kv.emplace_back(key, value);
    StoreAllPairs(path, kv);
}

std::vector<std::string> SplitCsv(const std::string& csv) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= csv.size()) {
        size_t comma = csv.find(',', start);
        std::string item = (comma == std::string::npos)
                         ? csv.substr(start)
                         : csv.substr(start, comma - start);
        Trim(item);
        if (!item.empty()) out.push_back(std::move(item));
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return out;
}

std::string JoinCsv(const std::vector<std::string>& items) {
    std::string out;
    for (size_t i = 0; i < items.size(); ++i) {
        if (i) out += ',';
        out += items[i];
    }
    return out;
}

}  // namespace fm2k::shell
