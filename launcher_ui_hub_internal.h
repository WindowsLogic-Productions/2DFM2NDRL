#pragma once
// Hub-family internal helpers shared across the split launcher_ui_hub*.cpp TUs
// (hub / hub_panel / hub_events / hub_match). Definitions live in
// launcher_ui_hub.cpp (top-level `static` was removed for external linkage).
// Kept off the public FM2K_Integration.h surface like the rest of the hub code.
#include <string>
#include <vector>
#include <filesystem>
#include <cstdint>
#include "FM2K_Integration.h"  // FM2K::FM2KGameInfo

// Pre-match UDP punch toward the peer (opens NAT before the game binds).
bool HubPreflightPunch(uint16_t local_port,
                       const std::string& peer_ip,
                       uint16_t peer_port,
                       const std::string& match_token_hex,
                       int timeout_ms);
// Read the spawned game's hook log and pull the "GameHash: manifest" block.
std::string ExtractGameHashManifest(const std::filesystem::path& exe_path,
                                    int player_index);
// Find the discovered-games index whose exe stem matches a hub room id.
int FindInstalledGameForRoom(const std::vector<FM2K::FM2KGameInfo>& games,
                             const std::string& room_id);
