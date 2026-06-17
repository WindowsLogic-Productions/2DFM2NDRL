#pragma once
// game_discovery.cpp shared cache type + helpers, externed so the cache-IO TU
// (game_discovery_cache.cpp) and the core scan/discovery code share them. Pure
// linkage move from the original Utils namespace -- GetCacheFilePath/StatFile
// are defined in the core (config/stat sections); CanonicalizePath/
// LoadGameCacheMap are defined in the cache-IO TU.
#include "FM2K_Integration.h"   // FM2K::Engine, FM2K::FM2KGameInfo
#include "FM2K_KgtParser.h"     // fm2k::KgtSummary
#include <cstdint>
#include <string>
#include <unordered_map>

namespace Utils {

struct GameCacheEntry {
    std::string exe_path;
    std::string dll_path;
    // exe stat — used for invalidation
    uint64_t    size       = 0;
    int64_t     mtime      = 0;
    // exe identification — populated on cache hit, skips re-hash + sniff
    uint64_t    exe_xxh64  = 0;
    FM2K::Engine engine    = FM2K::Engine::FM2K;
    std::string clean_label;
    std::string packer_label;
    bool        is_clean   = false;
    // kgt stat — separate from exe so kgt edits invalidate independently.
    // kgt_present=false for FM95 .player-only games (no .kgt file).
    bool        kgt_present = false;
    uint64_t    kgt_size    = 0;
    int64_t     kgt_mtime   = 0;
    // Parsed kgt summary — replaces fm2k::ParseKgtSummary on hit
    fm2k::KgtSummary kgt;
};

// Helpers shared between the scan core and the cache-IO TU (were file-static).
std::string GetCacheFilePath();                                  // core defines
bool        StatFile(const std::string& path, uint64_t& size, int64_t& mtime);  // core
std::string CanonicalizePath(const std::string& p);              // cache TU defines
std::unordered_map<std::string, GameCacheEntry> LoadGameCacheMap();  // cache TU defines

}  // namespace Utils
