// frontend/ShellState.h — UI navigation + transient state for the
// fm2k::shell render path. Distinct from the existing LauncherState
// (process-lifecycle: GameSelection/Connecting/InGame/Disconnected),
// which stays untouched.
//
// First-run flow: Splash → Login → Setup → Hub, gated by
// setup_version. Returning users skip straight from Splash to Hub.
#pragma once

#include <string>
#include <vector>

namespace fm2k::shell {

enum class ShellRoute {
    // M6 first-run flow + steady-state Hub. Deprecated route values
    // (BootCrawl/LogoIntro/SetupController/SetupIdentity/SetupSubscribe)
    // were removed in M6.5 — the M-plan settled on these 5.
    Splash,           // M6.0 — single splash screen with auto-advance
    Login,            // M6.1 — Discord OAuth pairing inline
    Setup,            // M6.2 — unified wizard; setup_step picks substep
    Completion,       // M6.4 — brief "all set" interstitial after Setup
    Hub,              // steady state
};

enum class HubView {
    Lobby,
    Browse,
    Replays,
    Rankings,
    Stats,
    Events,
    Profile,
    Config,
    Map,
};

struct ShellState {
    // Always boot through Splash. After auto-advance the route branches
    // on `setup_version`: 0 → Login → Setup wizard, ≥1 → Hub.
    ShellRoute route        = ShellRoute::Splash;
    // Initial Hub view = Browse per user feedback (post-setup users
    // should see the games library first, not an empty lobby chat
    // log). Once they pick a room the lobby becomes the natural
    // landing place but the first impression is the catalog.
    HubView    hub_view     = HubView::Browse;
    double     route_entered_at = 0.0;   // ImGui::GetTime() at last transition
    int        transition_seq   = 0;     // bumped on route change for tween IDs

    // First-run gate. Persisted in settings.ini under `setup_version`.
    // Bump this constant when a new mandatory setup step lands so
    // existing users get re-prompted with the new step.
    static constexpr int kCurrentSetupVersion = 1;
    int        setup_version = 0;

    // Read on startup from settings.ini key `subscribed_rooms` (CSV of
    // game_ids); falls back to "all installed games" if empty.
    std::vector<std::string> subscribed_rooms;
    int                      active_room_index = 0;   // -1 = none

    // Favorited rooms — fightcade-style. Independent of subscription
    // (you can fav a room without joining it). Persisted in
    // settings.ini under `favorite_rooms` (CSV of game_ids). Shown
    // with a star badge on Browse cards + a star next to the room id
    // in the Lobby strip.
    std::vector<std::string> favorite_rooms;

    // M6.1 LoginV2 — nick-picker UI state, populated from cached
    // discord_auth on first entry to the Login route. login_custom_nick
    // is a fixed buffer for ImGui::InputText.
    bool                     login_use_discord_name = true;
    char                     login_custom_nick[64]  = {0};
    bool                     login_state_loaded     = false;

    // M6.2 SetupV2 — current wizard step (0=Controls, 1=GameDirs,
    // 2=Net, 3=Finish; ordering per user feedback that controller
    // mapping should come before games-folder picking). Sidebar in
    // RenderSetupV2 lets the user click any step to jump.
    int                      setup_step = 0;
    char                     setup_new_path_buf[260] = {0};

    // HubView::Lobby has two modes:
    //   * in_room == false  → home dashboard (search + popular +
    //     populated lobbies + categories + hidden gems + events)
    //   * in_room == true   → traditional in-room view (hero + RULES
    //     + RESOURCES + CHAT) for the active_room_index room.
    // Clicking a populated lobby card / a LeftRail chip flips into
    // in_room. The "← HOME" affordance flips back. Per-session, not
    // persisted — fresh launch always lands on the home dashboard.
    bool                     in_room                  = false;
    char                     home_search_buf[64]      = {0};

    // Hidden games — game_ids excluded from default Browse / Populated
    // / Hidden-Gems listings. Mostly NSFW or community-flagged titles
    // (e.g. some Studio Climax releases). Persisted to settings.ini
    // under `hidden_rooms` (CSV). The toggle below reveals them.
    std::vector<std::string> hidden_rooms;
    bool                     show_hidden_rooms        = false;

    bool initialized = false;
};

// Returns true when the launcher should walk the Login → Setup wizard
// before landing in Hub.
inline bool ShouldRunWizard(const ShellState& s) {
    return s.setup_version < ShellState::kCurrentSetupVersion;
}

// Mark setup complete (writes to settings.ini and bumps in-memory state).
void MarkSetupComplete(ShellState& s);

// Singleton accessor. Constructed lazily on first call.
ShellState& State();

// Read-side helpers: load subscribed_rooms from settings.ini if the
// caller hasn't populated it yet. Idempotent — call from Render entry.
void LoadFromSettings(ShellState& s);

// Toggle favorite for `game_id`: adds/removes from s.favorite_rooms
// and persists the new CSV to settings.ini. Returns the new state.
bool ToggleFavorite(ShellState& s, const std::string& game_id);
bool IsFavorite(const ShellState& s, const std::string& game_id);

// Hidden-rooms equivalent — a per-user blacklist for NSFW or community-
// flagged titles. ToggleRoomHidden persists the new CSV to settings.ini.
bool IsRoomHidden(const ShellState& s, const std::string& game_id);
bool ToggleRoomHidden(ShellState& s, const std::string& game_id);

}  // namespace fm2k::shell
