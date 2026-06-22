// frontend/ShellState.cpp — see header.
#include "ShellState.h"
#include "Settings.h"

#include <algorithm>

namespace fm2k::shell {

ShellState& State() {
    static ShellState s;
    return s;
}

void LoadFromSettings(ShellState& s) {
    if (s.initialized) return;
    s.initialized = true;

    const std::string path = SettingsPath();
    if (path.empty()) return;

    s.setup_version = ReadInt(path, "setup_version", 0);

    const std::string csv = ReadString(path, "subscribed_rooms", "");
    s.subscribed_rooms = SplitCsv(csv);
    // Active room defaults to the first subscribed entry. If the list
    // is empty, AppShell falls back to whatever LauncherUI::games()
    // returns (no subscription is fine for offline use).
    s.active_room_index = s.subscribed_rooms.empty() ? -1 : 0;

    s.favorite_rooms = SplitCsv(ReadString(path, "favorite_rooms", ""));

    // Hidden games — the user can populate this list via settings.ini
    // (`hidden_rooms = studio_climax_x,studio_climax_y`) until we ship
    // a right-click "hide game" menu. v1 default is empty; the home
    // dashboard reveals hidden entries when show_hidden_rooms toggle
    // is on (also in settings.ini for sticky preference).
    s.hidden_rooms       = SplitCsv(ReadString(path, "hidden_rooms", ""));
    s.show_hidden_rooms  = ReadBool(path, "show_hidden_rooms", false);
}

bool IsRoomHidden(const ShellState& s, const std::string& game_id) {
    for (const auto& h : s.hidden_rooms) if (h == game_id) return true;
    return false;
}

bool ToggleRoomHidden(ShellState& s, const std::string& game_id) {
    bool now_hidden;
    auto it = std::find(s.hidden_rooms.begin(), s.hidden_rooms.end(), game_id);
    if (it == s.hidden_rooms.end()) {
        s.hidden_rooms.push_back(game_id);
        now_hidden = true;
    } else {
        s.hidden_rooms.erase(it);
        now_hidden = false;
    }
    const std::string path = SettingsPath();
    if (!path.empty()) {
        WriteString(path, "hidden_rooms", JoinCsv(s.hidden_rooms));
    }
    return now_hidden;
}

bool IsFavorite(const ShellState& s, const std::string& game_id) {
    for (const auto& f : s.favorite_rooms) {
        if (f == game_id) return true;
    }
    return false;
}

bool ToggleFavorite(ShellState& s, const std::string& game_id) {
    bool now_fav;
    auto it = std::find(s.favorite_rooms.begin(), s.favorite_rooms.end(), game_id);
    if (it == s.favorite_rooms.end()) {
        s.favorite_rooms.push_back(game_id);
        now_fav = true;
    } else {
        s.favorite_rooms.erase(it);
        now_fav = false;
    }
    const std::string path = SettingsPath();
    if (!path.empty()) {
        WriteString(path, "favorite_rooms", JoinCsv(s.favorite_rooms));
    }
    return now_fav;
}

void MarkSetupComplete(ShellState& s) {
    s.setup_version = ShellState::kCurrentSetupVersion;
    const std::string path = SettingsPath();
    if (!path.empty()) {
        WriteInt(path, "setup_version", s.setup_version);
    }
}

}  // namespace fm2k::shell
