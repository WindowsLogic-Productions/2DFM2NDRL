#include "FM2K_Integration.h"
#include "FM2K_HubClient.h"
#include "FM2K_DiscordAuth.h"
#include "FM2K_Locale.h"
#include "FM2K_Updater.h"
#include "version_local.h"
#include "FM2KHook/src/ui/input_binder.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <wininet.h>
#include <shellapi.h>  // Shell_NotifyIcon for challenge toast
#include <iostream>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <vector>
#include <filesystem>
#include "vendored/imgui/imgui.h"
#include "imgui_internal.h"
#include "vendored/GekkoNet/GekkoLib/include/gekkonet.h"
#include <chrono>
#include <ctime>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <memory>
#include <unordered_map>

// Local-only state owned by LauncherUI for the Hub panel. Defined here
// rather than in the header to keep FM2K_HubClient.h out of the public
// integration surface. unique_ptr<HubState> destructor needs the full
// type, which it has thanks to this definition + the LauncherUI dtor
// living in this file (line 82 onwards).
struct LauncherUI::HubState {
    fm2k::HubClient client;
    std::string my_id;
    std::string my_nick;
    std::string current_room_id;

    std::vector<fm2k::HubRoom> rooms;                     // discovered rooms
    std::unordered_map<std::string, fm2k::HubUser> users; // users in current room
    std::string pending_challenge_from_id;
    std::string pending_challenge_from_nick;
    std::string status_line;
    bool show_challenge_modal = false;

    // Outbound challenge state — populated when WE click Challenge on
    // somebody, cleared when the hub tells us the outcome (declined,
    // cancelled, failed, or match_start). Drives the "Waiting for X..."
    // modal so the challenger gets feedback instead of a silent UI.
    std::string outgoing_challenge_to_id;
    std::string outgoing_challenge_to_nick;
    bool        show_outgoing_challenge_modal = false;
};

// Case-insensitive match of `room_id` against installed games. A
// match is either an exact stem hit (room "SCWU" -> "SCWU.exe") or
// the room id followed by a non-letter on the stem
// (room "WonderfulWorld" -> "WonderfulWorld_ver_0946.exe", with '_'
// being non-alpha). The non-letter gate avoids overmatching on
// unrelated games whose stems happen to start with the same word
// ("Strip" -> "StripFighter5CE" vs "StripFighter_Zero" both pass
// when 'F' is non-alpha — but "Strip" wouldn't be a real room id).
//
// Phase-2 master game list will replace this heuristic with a
// canonical-id → exe-aliases table. Until then this gets us through
// versioned exes without a manual selection step.
// Launcher-side preflight: bidirectional 0xCD CTRL_PUNCH on the same
// UDP port the spawned game's hook will bind. Confirms peer reachability
// AND opens the NAT pinhole before launch — the hook's own punch is then
// redundant in the happy path but stays as a safety net. Closes the socket
// on return so the game DLL can re-bind via SO_REUSEADDR.
//
// Returns true if at least one authentic peer punch was observed.
// Synchronous and bounded by `timeout_ms`; UI freezes briefly while it
// runs (≤ ~50 ms on loopback, <1 s typical LAN/Internet).
static bool HubPreflightPunch(uint16_t local_port,
                              const std::string& peer_ip,
                              uint16_t peer_port,
                              const std::string& match_token_hex,
                              int timeout_ms)
{
    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);

    SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == INVALID_SOCKET) return false;

    BOOL reuse = TRUE;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in laddr{};
    laddr.sin_family = AF_INET;
    laddr.sin_port   = htons(local_port);
    laddr.sin_addr.s_addr = INADDR_ANY;
    if (bind(s, reinterpret_cast<sockaddr*>(&laddr), sizeof(laddr)) != 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "Preflight: bind() to %d failed err=%d", (int)local_port,
            WSAGetLastError());
        closesocket(s);
        return false;
    }

    DWORD recv_timeout = 100;  // 100ms recvfrom poll
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&recv_timeout), sizeof(recv_timeout));

    sockaddr_in paddr{};
    paddr.sin_family = AF_INET;
    paddr.sin_port   = htons(peer_port);
    if (inet_pton(AF_INET, peer_ip.c_str(), &paddr.sin_addr) != 1) {
        closesocket(s);
        return false;
    }

    // Decode 32-hex match token to 16 binary bytes (matches hub.py + nat_traversal).
    uint8_t token[16] = {};
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return 10 + c - 'a';
        if (c >= 'A' && c <= 'F') return 10 + c - 'A';
        return -1;
    };
    for (size_t i = 0; i + 1 < match_token_hex.size() && i / 2 < 16; i += 2) {
        int hi = nib(match_token_hex[i]);
        int lo = nib(match_token_hex[i + 1]);
        if (hi < 0 || lo < 0) break;
        token[i / 2] = static_cast<uint8_t>((hi << 4) | lo);
    }

    // CTRL_PUNCH packet: 0xCD 0x10 [16-byte token] — matches
    // FM2KHook/src/netplay/nat_traversal.cpp wire format.
    uint8_t pkt[2 + 16];
    pkt[0] = 0xCD;
    pkt[1] = 0x10;
    std::memcpy(pkt + 2, token, 16);

    const auto start    = std::chrono::steady_clock::now();
    const auto deadline = start + std::chrono::milliseconds(timeout_ms);
    auto next_send      = start;
    int  sends_done     = 0;
    bool peer_seen      = false;

    while (std::chrono::steady_clock::now() < deadline) {
        auto now = std::chrono::steady_clock::now();
        if (!peer_seen && now >= next_send && sends_done < 30) {
            sendto(s, reinterpret_cast<const char*>(pkt), sizeof(pkt), 0,
                   reinterpret_cast<const sockaddr*>(&paddr), sizeof(paddr));
            sends_done++;
            next_send = now + std::chrono::milliseconds(10);
        }

        uint8_t buf[1024];
        sockaddr_in from{};
        int from_len = sizeof(from);
        int n = recvfrom(s, reinterpret_cast<char*>(buf), sizeof(buf), 0,
                         reinterpret_cast<sockaddr*>(&from), &from_len);
        if (n >= 18 && buf[0] == 0xCD && buf[1] == 0x10 &&
            std::memcmp(buf + 2, token, 16) == 0) {
            peer_seen = true;
            // Send a few more punches so the peer also confirms us
            // before we drop the socket. NAT mapping persists through
            // close on cone NATs, but the peer needs one good packet
            // arriving from us to flip its own preflight to "done".
            for (int i = 0; i < 3; ++i) {
                sendto(s, reinterpret_cast<const char*>(pkt), sizeof(pkt), 0,
                       reinterpret_cast<const sockaddr*>(&paddr), sizeof(paddr));
                Sleep(5);
            }
            break;
        }
        // recvfrom returned WSAETIMEDOUT or other err — loop and try
        // again until deadline.
    }

    closesocket(s);

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "Preflight: %s in %lldms (%d sends, peer %s:%u, port=%d)",
        peer_seen ? "PEER REACHED" : "TIMED OUT",
        (long long)elapsed, sends_done, peer_ip.c_str(),
        (unsigned)peer_port, (int)local_port);
    return peer_seen;
}

static int FindInstalledGameForRoom(const std::vector<FM2K::FM2KGameInfo>& games,
                                    const std::string& room_id) {
    if (room_id.empty()) return -1;
    auto lower = [](std::string s) {
        for (auto& c : s) c = (char)std::tolower((unsigned char)c);
        return s;
    };
    const std::string target = lower(room_id);
    for (size_t i = 0; i < games.size(); ++i) {
        std::filesystem::path exe(games[i].exe_path);
        const std::string stem = lower(exe.stem().string());
        if (stem == target) return (int)i;
        if (stem.size() > target.size() && stem.compare(0, target.size(), target) == 0) {
            unsigned char next = static_cast<unsigned char>(stem[target.size()]);
            if (!std::isalpha(next)) return (int)i;
        }
    }
    return -1;
}

// LauncherUI Implementation
LauncherUI::LauncherUI() 
    : games_{}
    , network_config_{} // Zero-initialize network config
    , frames_ahead_(0.0f)
    , launcher_state_(LauncherState::GameSelection)
    , renderer_(nullptr)
    , window_(nullptr)
    , scanning_games_(false)
    , games_root_path_("")
    , selected_game_index_(-1)
    , current_theme_(UITheme::Dark)
    , scroll_to_bottom_(true)
    , original_log_function_(nullptr)
    , original_log_userdata_(nullptr)
{
    // Developer mode: opt-in via env var. End users see a simplified
    // panel with just Online (Hub) / Offline / Replay; developers see
    // the full battery of bisect checkboxes, stress, dual-client, etc.
    if (const char* env_dev = std::getenv("FM2K_DEV_MODE");
        env_dev && std::strcmp(env_dev, "1") == 0) {
        developer_mode_ = true;
    }

    hub_state_ = std::make_unique<HubState>();

    // Initialize callbacks to null
    on_game_selected = nullptr;
    on_offline_session_start = nullptr;
    on_online_session_start = nullptr;
    on_stress_session_start = nullptr;
    on_session_stop = nullptr;
    on_exit = nullptr;
    on_games_folder_set = nullptr;
    on_debug_save_state = nullptr;
    on_debug_load_state = nullptr;
    on_debug_force_rollback = nullptr;
    on_frame_step_pause = nullptr;
    on_frame_step_single = nullptr;
    on_frame_step_multi = nullptr;
    on_debug_save_to_slot = nullptr;
    on_debug_load_from_slot = nullptr;
    on_debug_auto_save_config = nullptr;
    on_get_slot_status = nullptr;
    on_get_auto_save_config = nullptr;
    on_get_enhanced_actions = nullptr;
    on_set_production_mode = nullptr;
    on_set_input_recording = nullptr;
    on_set_minimal_gamestate_testing = nullptr;
    // on_set_save_profile removed - now using optimized FastGameState system
    
    // Initialize multi-client testing callbacks
    on_launch_local_client1 = nullptr;
    on_launch_local_client2 = nullptr;
    on_launch_local_spectator = nullptr;
    on_launch_local_spectator2 = nullptr;
    on_terminate_all_clients = nullptr;
    on_get_client_status = nullptr;
    // Network simulation callbacks removed - not connected to LocalNetworkAdapter
    on_get_rollback_stats = nullptr;

    log_buffer_mutex_ = SDL_CreateMutex();
}

LauncherUI::~LauncherUI() {
    if (log_buffer_mutex_) {
        SDL_DestroyMutex(log_buffer_mutex_);
        log_buffer_mutex_ = nullptr;
    }
    Shutdown();
}

bool LauncherUI::Initialize(SDL_Window* window, SDL_Renderer* renderer) {
    if (!window || !renderer) {
        std::cerr << "Invalid SDL window or renderer" << std::endl;
        return false;
    }
    renderer_ = renderer;
    window_ = window;
    
    // NUCLEAR: Exact copy of official SDL3 renderer example initialization
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
    
    // Setup Dear ImGui style
    ImGui::StyleColorsDark();
    
    // Setup scaling - THIS IS CRITICAL FOR FONT STACK
    float main_scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(main_scale);
    style.FontScaleDpi = main_scale;

    // Two-font atlas: a Latin-first font for ASCII / Latin-1 (so backslash
    // stays a backslash and Spanish accented chars render natively), then
    // MS Gothic / Meiryo merged on top for Japanese coverage.
    //
    // Why this matters: Japanese fonts follow JIS X 0201, which maps
    // codepoint 0x5C to the yen sign (¥) instead of backslash. If we load
    // a JP font first with `GetGlyphRangesJapanese()` (which includes
    // ASCII), every backslash in the UI renders as ¥ — visible in file
    // paths, escape characters in tooltips, etc. By loading Segoe UI (or
    // any Latin font) first to claim ASCII slots, then merging the JP
    // font with MergeMode=true, ImGui keeps the Latin font's glyph for
    // any codepoint already in the atlas and only pulls JP glyphs from
    // MS Gothic. Backslash stays a backslash, hiragana/kanji come from JP.
    {
        // Latin font candidates in priority order. Segoe UI is the modern
        // Windows UI font (Vista+); Tahoma is a universal fallback that
        // ships on every Windows install.
        const char* latin_font_paths[] = {
            "C:\\Windows\\Fonts\\segoeui.ttf",
            "C:\\Windows\\Fonts\\tahoma.ttf",
            "C:\\Windows\\Fonts\\arial.ttf",
        };
        ImFontConfig latin_config;
        latin_config.OversampleH = 2;
        latin_config.OversampleV = 2;
        latin_config.PixelSnapH = false;
        ImFont* latin_font = nullptr;
        for (const char* p : latin_font_paths) {
            latin_font = io.Fonts->AddFontFromFileTTF(p, 16.0f, &latin_config,
                                                     io.Fonts->GetGlyphRangesDefault());
            if (latin_font) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Loaded Latin UI font: %s", p);
                break;
            }
        }
        if (!latin_font) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "No Latin system font loaded — falling back to "
                        "ImGui default. Backslash and accented characters "
                        "may render with the bundled bitmap font.");
            io.Fonts->AddFontDefault();
        }

        // Japanese fonts merged on top. MergeMode=true means glyphs already
        // claimed by the Latin font (ASCII / Latin-1 supplement) stay with
        // the Latin font; only codepoints not yet in the atlas (kana,
        // kanji, half-width katakana, etc) get pulled from MS Gothic.
        ImFontConfig jp_config;
        jp_config.MergeMode    = true;
        jp_config.OversampleH  = 2;
        jp_config.OversampleV  = 2;
        jp_config.PixelSnapH   = true;
        const char* jp_font_paths[] = {
            "C:\\Windows\\Fonts\\msgothic.ttc",
            "C:\\Windows\\Fonts\\meiryo.ttc",
            "C:\\Windows\\Fonts\\msgothic.ttf",
            "C:\\Windows\\Fonts\\YuGothM.ttc",
        };
        bool jp_loaded = false;
        for (const char* p : jp_font_paths) {
            if (io.Fonts->AddFontFromFileTTF(p, 16.0f, &jp_config,
                                             io.Fonts->GetGlyphRangesJapanese())) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Merged Japanese font: %s", p);
                jp_loaded = true;
                break;
            }
        }
        if (!jp_loaded) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "No Japanese-capable system font found — Japanese "
                        "text will render as '?'. Install East Asian "
                        "language pack to fix.");
        }
    }

    // Locale: load translation tables and pick the active language. Must
    // happen AFTER the font is configured (so ImGui has glyphs ready when
    // the first frame renders) but BEFORE any T() call.
    fm2k::Locale::Init();

    // Setup Platform/Renderer backends
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);
    
    // Setup our logging to capture SDL logs
    SDL_GetLogOutputFunction(&original_log_function_, &original_log_userdata_);
    SDL_SetLogOutputFunction(SDLCustomLogOutput, this);

    SDL_Log("Launcher UI Initialized");
    
    return true;
}

void LauncherUI::Shutdown() {
    // Tear down the updater's background worker so we don't leak the
    // thread on exit (and so a mid-flight download is cancelled cleanly
    // rather than racing with shutdown).
    fm2k::updater::Shutdown();

    // Restore original logger
    SDL_SetLogOutputFunction(original_log_function_, original_log_userdata_);

    // Cleanup ImGui
    if (ImGui::GetCurrentContext()) {
        // Make sure we finish any pending viewport operations
        if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
            ImGui::UpdatePlatformWindows();
            ImGui::RenderPlatformWindowsDefault();
        }
        
        ImGui_ImplSDLRenderer3_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
    }
    
    std::cout << "LauncherUI shutdown" << std::endl;
}

void LauncherUI::NewFrame() {
    // Start the Dear ImGui frame
    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
}

void LauncherUI::Render() {
    // First-run nudge: if no cached Discord session and we haven't
    // shown the prompt yet this run, auto-open the sign-in window.
    // Hub auth is mandatory for online play during testing; users
    // landing on the launcher should see the path forward right away
    // rather than discovering it via "auth_required" after a failed
    // Connect.
    {
        static bool s_did_first_run_nudge = false;
        if (!s_did_first_run_nudge) {
            s_did_first_run_nudge = true;
            const auto a = fm2k::discord_auth::LoadCached();
            if (!a.valid) {
                show_discord_auth_ = true;
            }
        }
    }

    // Render menu bar at application level first
    RenderMenuBar();
    
    // Create a dockspace to allow for flexible panel arrangement
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);

    ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove;
    window_flags |= ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;
    window_flags |= ImGuiWindowFlags_NoBackground;
    
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("DockSpace", nullptr, window_flags);
    ImGui::PopStyleVar();
    ImGui::PopStyleVar(2);

    ImGuiID dockspace_id = ImGui::GetID("MainDockSpace");
    ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_None);

    // First-launch default layout. ImGui persists user-edited layout into
    // imgui.ini on quit, so this only fires on a fresh install (or after
    // the user deletes imgui.ini). DockBuilder gates on whether the node
    // already has children — if any prior layout exists, we leave it
    // alone so users keep their customizations across versions.
    {
        static bool s_layout_built = false;
        if (!s_layout_built) {
            s_layout_built = true;
            ImGuiDockNode* root = ImGui::DockBuilderGetNode(dockspace_id);
            if (!root || (root->Windows.Size == 0 && !root->IsSplitNode())) {
                ImGui::DockBuilderRemoveNode(dockspace_id);
                ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
                ImGui::DockBuilderSetNodeSize(dockspace_id, viewport->WorkSize);
                // Single tab group containing both the Hub and Games &
                // Configuration. They live in the same dock so the user
                // sees them as tabs by default; drag-out to undock if
                // they want side-by-side.
                ImGui::DockBuilderDockWindow("Hub",                    dockspace_id);
                ImGui::DockBuilderDockWindow("Games & Configuration",  dockspace_id);
                ImGui::DockBuilderDockWindow("Debug & Diagnostics",    dockspace_id);
                ImGui::DockBuilderFinish(dockspace_id);
            }
        }
    }

    ImGuiWindowFlags panel_flags = ImGuiWindowFlags_NoCollapse;

    if (ImGui::Begin("Games & Configuration", nullptr, panel_flags)) {
        RenderGameSelection();
        ImGui::Separator();
        // Network-config panel is dev-mode only — end users use the Hub
        // panel for matchmaking and don't manually configure ports/IPs.
        if (developer_mode_) {
            RenderNetworkConfig();
            ImGui::Separator();
        }
        RenderSessionControls();
    }
    ImGui::End();

    if (ImGui::Begin("Hub", nullptr, panel_flags)) {
        RenderHubPanel();
    }
    ImGui::End();

    if (developer_mode_) {
        if (ImGui::Begin("Debug & Diagnostics", nullptr, panel_flags)) {
            RenderDebugTools();
        }
        ImGui::End();
    }
    
    ImGui::End(); // End DockSpace

    // Render connection status popups
    RenderConnectionStatus();
}

void LauncherUI::RenderMenuBar() {
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu(T("menu_file"))) {
            if (ImGui::MenuItem(T("btn_select_games_folder"))) {
                // ... folder selection logic ...
            }
            if (ImGui::MenuItem(T("menu_exit"), "Alt+F4")) {
                if (on_exit) on_exit();
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(T("menu_session"))) {
            if (launcher_state_ == LauncherState::InGame || launcher_state_ == LauncherState::Connecting) {
                if (ImGui::MenuItem(T("hub_disconnect"))) {
                    if (on_session_stop) on_session_stop();
                }
            } else {
                ImGui::MenuItem(T("hub_disconnect"), nullptr, false, false); // Disabled
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(T("menu_view"))) {
            if (ImGui::MenuItem(T("menu_developer_mode"), nullptr, developer_mode_)) {
                developer_mode_ = !developer_mode_;
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(T("menu_settings"))) {
            if (ImGui::MenuItem(T("input_bindings_p1"), nullptr, show_input_binder_p1_)) {
                show_input_binder_p1_ = !show_input_binder_p1_;
                if (show_input_binder_p1_ && !input_binder_initialized_) {
                    FM2KInputBinder::Init();
                    input_binder_initialized_ = true;
                }
            }
            if (ImGui::MenuItem(T("input_bindings_p2"), nullptr, show_input_binder_p2_)) {
                show_input_binder_p2_ = !show_input_binder_p2_;
                if (show_input_binder_p2_ && !input_binder_initialized_) {
                    FM2KInputBinder::Init();
                    input_binder_initialized_ = true;
                }
            }
            if (ImGui::MenuItem(T("panel_host_config"), nullptr, show_host_config_)) {
                show_host_config_ = !show_host_config_;
            }
            if (ImGui::MenuItem(T("menu_hub_server"), nullptr, show_hub_server_)) {
                show_hub_server_ = !show_hub_server_;
            }
            if (ImGui::MenuItem(T("hub_signin_ellipsis"), nullptr, show_discord_auth_)) {
                show_discord_auth_ = !show_discord_auth_;
            }
            ImGui::Separator();
            // Audio mutes — write to %APPDATA%\FM2K_Rollback\audio.ini.
            // The hook DLL re-reads it every ~1s from inside the audio
            // dispatcher, so the toggle takes effect within a second
            // without needing the game to restart.
            if (!mute_state_loaded_) {
                mute_state_loaded_ = true;
                LoadAudioMuteState();
            }
            if (ImGui::MenuItem(T("audio_mute_music"), nullptr, mute_bgm_)) {
                mute_bgm_ = !mute_bgm_;
                SaveAudioMuteState();
            }
            if (ImGui::MenuItem(T("audio_mute_se"), nullptr, mute_se_)) {
                mute_se_ = !mute_se_;
                SaveAudioMuteState();
            }
            ImGui::Separator();
            // Notification toggles. Lazy-loaded once on first menu render
            // so the read of settings.ini doesn't happen until the user
            // actually opens the Settings menu.
            if (!notify_state_loaded_) {
                LoadNotifyState();
                notify_state_loaded_ = true;
            }
            if (ImGui::BeginMenu(T("menu_notifications"))) {
                if (ImGui::MenuItem(T("notify_taskbar_flash"), nullptr, notify_flash_)) {
                    notify_flash_ = !notify_flash_;
                    SaveNotifyState();
                }
                if (ImGui::MenuItem(T("notify_sound"), nullptr, notify_sound_)) {
                    notify_sound_ = !notify_sound_;
                    SaveNotifyState();
                }
                if (ImGui::MenuItem(T("notify_toast"), nullptr, notify_toast_)) {
                    notify_toast_ = !notify_toast_;
                    SaveNotifyState();
                }
                ImGui::EndMenu();
            }
            ImGui::Separator();
            if (ImGui::MenuItem(T("menu_check_for_updates"))) {
                fm2k::updater::CheckForUpdates();
            }
            ImGui::TextDisabled(T("label_version_rev"), fm2k::kAppVersion, fm2k::kAppRevision);
            ImGui::EndMenu();
        }

        // Language menu — top-level so users don't have to dig into
        // Settings to switch. Each entry labels itself in its own native
        // script so anyone can recognize their language regardless of what
        // the launcher is currently set to. Toggling persists the choice to
        // %APPDATA%\FM2K_Rollback\settings.ini and applies on the next
        // frame (no restart needed — the font atlas has every glyph range
        // loaded once at boot).
        if (ImGui::BeginMenu(T("menu_language"))) {
            const fm2k::Lang current = fm2k::Locale::Current();
            for (fm2k::Lang lang : fm2k::Locale::All()) {
                bool selected = (lang == current);
                if (ImGui::MenuItem(fm2k::Locale::DisplayNameForLang(lang),
                                    nullptr, selected)) {
                    fm2k::Locale::Set(lang);
                }
            }
            ImGui::EndMenu();
        }

        // Lazy-load auth state on first menu-bar render. File is only
        // touched when the auth window saves/clears, so the read is
        // cheap and we don't need to refresh per-frame.
        if (!discord_state_loaded_) {
            discord_state_loaded_ = true;
            const auto a = fm2k::discord_auth::LoadCached();
            discord_signed_in_ = a.valid;
            discord_nick_      = a.nick;
        }

        // Kick off the version check exactly once on first menu-bar
        // render. Async; pill below shows the result whenever it lands.
        static bool s_did_update_check = false;
        if (!s_did_update_check) {
            s_did_update_check = true;
            fm2k::updater::CheckForUpdates();
        }

        // Build BOTH pills (update + Discord) and right-align them
        // together so they don't drift around as state changes.
        const auto upd = fm2k::updater::Get();

        char update_pill[80] = {};
        bool show_update_pill = false;
        ImVec4 update_col{};
        switch (upd.state) {
            case fm2k::updater::State::UpdateAvailable:
                std::snprintf(update_pill, sizeof(update_pill),
                              "  Update %s -> %s  ",
                              fm2k::kAppVersion, upd.remote_version.c_str());
                update_col      = ImVec4(0.40f, 0.65f, 0.95f, 1.0f);
                show_update_pill = true;
                break;
            case fm2k::updater::State::Downloading: {
                int pct = (upd.total_bytes > 0)
                    ? (int)(((uint64_t)upd.downloaded_bytes * 100) / upd.total_bytes)
                    : 0;
                std::snprintf(update_pill, sizeof(update_pill),
                              "  Downloading %d%%  ", pct);
                update_col      = ImVec4(0.40f, 0.65f, 0.95f, 1.0f);
                show_update_pill = true;
                break;
            }
            case fm2k::updater::State::Ready:
                std::snprintf(update_pill, sizeof(update_pill),
                              "  Apply %s — Restart  ", upd.remote_version.c_str());
                update_col      = ImVec4(0.40f, 0.85f, 0.50f, 1.0f);
                show_update_pill = true;
                break;
            case fm2k::updater::State::Failed:
                // Surface the failure quietly — clickable so the user
                // can re-trigger via the menu, but not blinking. Most
                // common failure is "fm2ktest repo doesn't exist yet";
                // logging will say so too.
                std::snprintf(update_pill, sizeof(update_pill),
                              "  Update check failed  ");
                update_col      = ImVec4(0.95f, 0.40f, 0.40f, 0.85f);
                show_update_pill = true;
                break;
            default:
                show_update_pill = false;
                break;
        }

        char discord_pill[64];
        if (discord_signed_in_) {
            std::snprintf(discord_pill, sizeof(discord_pill), "  Discord: %s  ",
                          discord_nick_.empty() ? "signed in" : discord_nick_.c_str());
        } else {
            std::snprintf(discord_pill, sizeof(discord_pill), "  Sign in with Discord  ");
        }

        const float discord_w = ImGui::CalcTextSize(discord_pill).x +
                                ImGui::GetStyle().ItemSpacing.x * 2.0f;
        const float update_w  = show_update_pill
            ? ImGui::CalcTextSize(update_pill).x +
              ImGui::GetStyle().ItemSpacing.x * 2.0f
            : 0.0f;
        const float total_w   = discord_w + update_w;
        const float bar_w     = ImGui::GetContentRegionAvail().x;
        if (total_w < bar_w) {
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (bar_w - total_w));
        }

        // Update pill (left of the Discord one) — clickable, advances
        // through the state machine: UpdateAvailable → start download
        // → Ready → spawn FM2KUpdater.exe and exit.
        if (show_update_pill) {
            ImGui::PushStyleColor(ImGuiCol_Text, update_col);
            if (ImGui::MenuItem(update_pill)) {
                switch (upd.state) {
                    case fm2k::updater::State::UpdateAvailable:
                        fm2k::updater::StartDownload();
                        break;
                    case fm2k::updater::State::Ready:
                        fm2k::updater::ApplyUpdateAndExit();
                        break;
                    case fm2k::updater::State::Failed:
                        fm2k::updater::CheckForUpdates();
                        break;
                    default:
                        break;
                }
            }
            // Tooltip carries the actual error / status detail. Pill
            // text is intentionally short so it fits in the menu bar;
            // hovering reveals the diagnostic.
            if (ImGui::IsItemHovered()) {
                if (upd.state == fm2k::updater::State::Failed) {
                    ImGui::SetTooltip("%s\nClick to retry.",
                        upd.error_detail.empty()
                            ? "Update check failed."
                            : upd.error_detail.c_str());
                } else if (upd.state == fm2k::updater::State::UpdateAvailable) {
                    ImGui::SetTooltip("Click to download v%s.",
                        upd.remote_version.c_str());
                } else if (upd.state == fm2k::updater::State::Ready) {
                    ImGui::SetTooltip("Click to apply v%s — launcher will restart.",
                        upd.remote_version.c_str());
                } else if (upd.state == fm2k::updater::State::Downloading) {
                    if (upd.total_bytes > 0) {
                        ImGui::SetTooltip("Downloading %u / %u bytes.",
                            (unsigned)upd.downloaded_bytes,
                            (unsigned)upd.total_bytes);
                    } else {
                        ImGui::SetTooltip("Downloading %u bytes.",
                            (unsigned)upd.downloaded_bytes);
                    }
                }
            }
            ImGui::PopStyleColor();
        }

        // Discord pill (right edge).
        ImVec4 col;
        if (discord_signed_in_) {
            col = ImVec4(0.3f, 0.85f, 0.45f, 1.0f);  // green = good to go
        } else {
            const float t = (float)ImGui::GetTime();
            const float a = 0.55f + 0.35f * (float)std::sin(t * 3.0f);
            col = ImVec4(0.95f, 0.65f, 0.20f, a);
        }
        ImGui::PushStyleColor(ImGuiCol_Text, col);
        if (ImGui::MenuItem(discord_pill)) {
            show_discord_auth_ = !show_discord_auth_;
        }
        ImGui::PopStyleColor();
        ImGui::EndMainMenuBar();
    }

    // Input-binder windows. RenderWindow returns true on any change → autosave.
    if (show_input_binder_p1_) {
        if (FM2KInputBinder::RenderWindow(0, &show_input_binder_p1_)) {
            FM2KInputBinder::Save();
        }
    }
    if (show_input_binder_p2_) {
        if (FM2KInputBinder::RenderWindow(1, &show_input_binder_p2_)) {
            FM2KInputBinder::Save();
        }
    }
    if (show_host_config_) {
        RenderHostConfigWindow();
    }
    if (show_hub_server_) {
        RenderHubServerWindow();
    }
    if (show_discord_auth_) {
        RenderDiscordAuthWindow();
    }
}

// Settings → Hub Server… window. Lets the user point the launcher at a
// custom hub (their own hub.py instance, a friend's box, etc.) without
// cluttering the main Hub panel for casual users who just want the
// default 2dfm.sytes.net.
void LauncherUI::RenderHubServerWindow() {
    if (!hub_host_initialized_) {
        hub_host_initialized_ = true;
        const char* env_h = std::getenv("FM2K_HUB_HOST");
        const char* def   = (env_h && env_h[0]) ? env_h : "2dfm.sytes.net";
        std::snprintf(hub_host_, sizeof(hub_host_), "%s", def);
    }
    if (!ImGui::Begin("Hub Server", &show_hub_server_, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        return;
    }
    ImGui::PushItemWidth(280);
    ImGui::InputText(T("netcfg_host"), hub_host_, sizeof(hub_host_));
    ImGui::PopItemWidth();
    ImGui::TextWrapped(
        "Hub server hostname or IP. Default 2dfm.sytes.net for public play. "
        "Use 127.0.0.1 (or localhost) when running your own hub.py on the same "
        "machine — NAT routers rarely hairpin so the public DNS won't loop back. "
        "Takes effect on next Connect.");
    ImGui::End();
}

// Audio-mute persistence. Same file the hook DLL reads from inside
// Hook_DispatchScriptSoundCommand. We deliberately use a tiny flat
// key=value format so a textedit-the-ini fallback works for users
// without a launcher rebuild.
static std::string AudioIniPath() {
    const char* a = std::getenv("APPDATA");
    if (!a || !*a) return "";
    std::string dir = std::string(a) + "\\FM2K_Rollback";
    CreateDirectoryA(dir.c_str(), nullptr);
    return dir + "\\audio.ini";
}

// Dev-flag persistence. Same flat key=value format as audio.ini. Currently
// just stores `eb_diag=` so the [EB] palette/shake diagnostic toggle
// survives launcher restarts.
static std::string DevFlagsIniPath() {
    const char* a = std::getenv("APPDATA");
    if (!a || !*a) return "";
    std::string dir = std::string(a) + "\\FM2K_Rollback";
    CreateDirectoryA(dir.c_str(), nullptr);
    return dir + "\\dev_flags.ini";
}

static bool LoadDevFlag(const char* key, bool default_val) {
    const std::string path = DevFlagsIniPath();
    if (path.empty()) return default_val;
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f) return default_val;
    char line[128];
    bool result = default_val;
    while (std::fgets(line, sizeof(line), f)) {
        std::string s = line;
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r' ||
                              s.back() == ' '  || s.back() == '\t')) s.pop_back();
        const size_t eq = s.find('=');
        if (eq == std::string::npos) continue;
        if (s.substr(0, eq) != key) continue;
        const std::string v = s.substr(eq + 1);
        result = (v == "1" || v == "true" || v == "yes" || v == "on");
    }
    std::fclose(f);
    return result;
}

static void SaveDevFlag(const char* key, bool value) {
    const std::string path = DevFlagsIniPath();
    if (path.empty()) return;
    // Read all existing keys, replace this one, write back. Tiny file —
    // a few keys at most — so brute-force rewrite is fine.
    std::vector<std::pair<std::string, std::string>> kv;
    if (FILE* f = std::fopen(path.c_str(), "r")) {
        char line[128];
        while (std::fgets(line, sizeof(line), f)) {
            std::string s = line;
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r' ||
                                  s.back() == ' '  || s.back() == '\t')) s.pop_back();
            if (s.empty() || s[0] == '#' || s[0] == ';') continue;
            const size_t eq = s.find('=');
            if (eq == std::string::npos) continue;
            kv.emplace_back(s.substr(0, eq), s.substr(eq + 1));
        }
        std::fclose(f);
    }
    bool found = false;
    for (auto& p : kv) if (p.first == key) { p.second = value ? "1" : "0"; found = true; }
    if (!found) kv.emplace_back(key, value ? "1" : "0");
    if (FILE* f = std::fopen(path.c_str(), "w")) {
        for (const auto& p : kv) std::fprintf(f, "%s=%s\n", p.first.c_str(), p.second.c_str());
        std::fclose(f);
    }
}

void LauncherUI::LoadAudioMuteState() {
    const std::string path = AudioIniPath();
    if (path.empty()) return;
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f) return;
    char line[128];
    while (std::fgets(line, sizeof(line), f)) {
        std::string s = line;
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r' ||
                              s.back() == ' '  || s.back() == '\t')) s.pop_back();
        if (s.empty() || s[0] == '#' || s[0] == ';') continue;
        const size_t eq = s.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = s.substr(0, eq);
        const std::string v = s.substr(eq + 1);
        const bool truthy = (v == "1" || v == "true" || v == "yes" || v == "on");
        if      (k == "bgm_muted") mute_bgm_ = truthy;
        else if (k == "se_muted")  mute_se_  = truthy;
    }
    std::fclose(f);
}

void LauncherUI::SaveAudioMuteState() {
    const std::string path = AudioIniPath();
    if (path.empty()) return;
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) return;
    std::fprintf(f, "; FM2K Rollback audio mute state\n");
    std::fprintf(f, "; rewritten on each toggle from the launcher's Settings menu.\n");
    std::fprintf(f, "; the hook DLL re-reads this file ~once per second from inside\n");
    std::fprintf(f, "; the audio dispatcher, so changes propagate without restarting.\n");
    std::fprintf(f, "bgm_muted=%d\n", mute_bgm_ ? 1 : 0);
    std::fprintf(f, "se_muted=%d\n",  mute_se_  ? 1 : 0);
    std::fclose(f);
}

// ---------------------------------------------------------------------------
// Notification settings + delivery
// ---------------------------------------------------------------------------
// Persists three independent toggles to the launcher's settings.ini next to
// the Locale module's `language` key. Defaults are all-on so users never miss
// a challenge while tabbed out — they can dial it back per-channel from
// Settings → Notifications.

static std::string NotifySettingsPath() {
    const char* a = std::getenv("APPDATA");
    if (!a || !*a) return "";
    std::string dir = std::string(a) + "\\FM2K_Rollback";
    CreateDirectoryA(dir.c_str(), nullptr);
    return dir + "\\settings.ini";
}

static bool ReadBoolSetting(const std::string& path, const char* key, bool dflt) {
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f) return dflt;
    char line[256];
    bool out = dflt;
    while (std::fgets(line, sizeof(line), f)) {
        char* p = line;
        while (*p == ' ' || *p == '\t') ++p;
        if (*p == '#' || *p == ';' || *p == '\0' || *p == '\n') continue;
        char* eq = std::strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char* k = p;
        size_t klen = std::strlen(k);
        while (klen > 0 && (k[klen-1] == ' ' || k[klen-1] == '\t')) k[--klen] = '\0';
        if (std::strcmp(k, key) != 0) continue;
        char* v = eq + 1;
        while (*v == ' ' || *v == '\t') ++v;
        size_t vlen = std::strlen(v);
        while (vlen > 0 && (v[vlen-1] == '\n' || v[vlen-1] == '\r' ||
                            v[vlen-1] == ' '  || v[vlen-1] == '\t')) v[--vlen] = '\0';
        out = (std::strcmp(v, "1") == 0 || std::strcmp(v, "true") == 0
            || std::strcmp(v, "yes") == 0 || std::strcmp(v, "on") == 0);
        break;
    }
    std::fclose(f);
    return out;
}

static void WriteBoolSetting(const std::string& path, const char* key, bool value) {
    // Read all keys, replace ours, rewrite. Tiny file, tiny number of keys —
    // brute force is fine and keeps the format stable.
    std::vector<std::pair<std::string, std::string>> kv;
    if (FILE* f = std::fopen(path.c_str(), "r")) {
        char line[256];
        while (std::fgets(line, sizeof(line), f)) {
            std::string s = line;
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r' ||
                                  s.back() == ' '  || s.back() == '\t')) s.pop_back();
            if (s.empty() || s[0] == '#' || s[0] == ';') continue;
            const size_t eq = s.find('=');
            if (eq == std::string::npos) continue;
            kv.emplace_back(s.substr(0, eq), s.substr(eq + 1));
        }
        std::fclose(f);
    }
    bool found = false;
    for (auto& p : kv) if (p.first == key) { p.second = (value ? "1" : "0"); found = true; }
    if (!found) kv.emplace_back(key, value ? "1" : "0");
    if (FILE* f = std::fopen(path.c_str(), "w")) {
        for (const auto& p : kv) std::fprintf(f, "%s=%s\n", p.first.c_str(), p.second.c_str());
        std::fclose(f);
    }
}

void LauncherUI::LoadNotifyState() {
    const std::string path = NotifySettingsPath();
    if (path.empty()) return;
    notify_flash_ = ReadBoolSetting(path, "notify_flash", true);
    notify_sound_ = ReadBoolSetting(path, "notify_sound", true);
    notify_toast_ = ReadBoolSetting(path, "notify_toast", true);
}

void LauncherUI::SaveNotifyState() {
    const std::string path = NotifySettingsPath();
    if (path.empty()) return;
    WriteBoolSetting(path, "notify_flash", notify_flash_);
    WriteBoolSetting(path, "notify_sound", notify_sound_);
    WriteBoolSetting(path, "notify_toast", notify_toast_);
}

void LauncherUI::FireChallengeNotification(const std::string& from_nick) {
    // Resolve the launcher's HWND once. SDL3 stores it on the window's
    // properties under SDL_PROP_WINDOW_WIN32_HWND_POINTER. If we can't get
    // it (e.g., SDL backend changed), every Win32-flavored notification
    // silently no-ops — sound still works.
    HWND hwnd = nullptr;
    if (window_) {
        hwnd = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(window_),
                                            SDL_PROP_WINDOW_WIN32_HWND_POINTER,
                                            nullptr);
    }

    // 1) Taskbar flash. Only if the launcher isn't currently the foreground
    // window — flashing while focused is annoying. FLASHW_ALL flashes both
    // window caption AND taskbar button. FLASHW_TIMERNOFG keeps flashing
    // until the user focuses the window. Cancels automatically on focus.
    if (notify_flash_ && hwnd && hwnd != GetForegroundWindow()) {
        FLASHWINFO fi = { sizeof(fi), hwnd,
                          FLASHW_ALL | FLASHW_TIMERNOFG, 0, 0 };
        FlashWindowEx(&fi);
    }

    // 2) Sound: MessageBeep is the cheapest "make a noise" path on Windows.
    // No assets to ship; the Windows default-event sound is what users
    // already recognize as a notification chirp. MB_ICONINFORMATION maps
    // to SystemAsterisk — a short, non-jarring ding.
    if (notify_sound_) {
        MessageBeep(MB_ICONINFORMATION);
    }

    // 3) Windows toast / balloon notification via Shell_NotifyIconW
    // (wide-string variant). The W variant is critical so non-ASCII nicks
    // (Armonté, テスト, español) render correctly — Shell_NotifyIconA
    // would interpret UTF-8 bytes through the system codepage (CP1252 on
    // most US installs), turning "é" (`C3 A9`) into "Ã©" garbage.
    //
    // Single-balloon protocol:
    //   NIM_ADD    with NIF_ICON | NIF_TIP only      — register, no toast
    //   NIM_MODIFY with NIF_INFO + content fields    — fires exactly 1 toast
    //   NIM_DELETE                                   — cleanup
    // Earlier impl set NIF_INFO on both ADD and MODIFY which fired TWO
    // balloons (one per call) — fixed by splitting the flag set.
    if (notify_toast_ && hwnd) {
        char body_utf8[256];
        std::snprintf(body_utf8, sizeof(body_utf8),
                      T("modal_incoming_challenge_body"), from_nick.c_str());

        NOTIFYICONDATAW nid{};
        nid.cbSize  = sizeof(nid);
        nid.hWnd    = hwnd;
        nid.uID     = 1;
        nid.hIcon   = LoadIcon(nullptr, IDI_APPLICATION);

        auto utf8_to_wide = [](const char* in, wchar_t* out, int out_len) {
            if (!in || !out || out_len <= 0) return;
            int n = MultiByteToWideChar(CP_UTF8, 0, in, -1, out, out_len);
            if (n <= 0) out[0] = L'\0';
        };
        utf8_to_wide("FM2K Rollback Launcher", nid.szTip,
                     (int)(sizeof(nid.szTip)/sizeof(WCHAR)));

        // Step 1: register the icon (no NIF_INFO yet → no balloon).
        nid.uFlags = NIF_ICON | NIF_TIP;
        Shell_NotifyIconW(NIM_ADD, &nid);

        // Step 2: modify with the balloon info (this fires the one toast).
        nid.uFlags     = NIF_INFO | NIF_ICON | NIF_TIP;
        nid.dwInfoFlags = NIIF_INFO;
        utf8_to_wide(T("modal_incoming_challenge_title"), nid.szInfoTitle,
                     (int)(sizeof(nid.szInfoTitle)/sizeof(WCHAR)));
        utf8_to_wide(body_utf8, nid.szInfo,
                     (int)(sizeof(nid.szInfo)/sizeof(WCHAR)));
        Shell_NotifyIconW(NIM_MODIFY, &nid);

        // Step 3: cleanup. Windows captures the balloon info before
        // releasing the icon slot, so the toast still appears in Action
        // Center even after this Delete returns.
        Shell_NotifyIconW(NIM_DELETE, &nid);
    }
}

// Settings → Sign in with Discord… window. Drives the OAuth pairing
// flow in FM2K_DiscordAuth: kicks off /pair/begin, opens the browser,
// polls /pair/<code> until success/fail. The hub_token is cached in
// %APPDATA%\FM2K_Rollback\discord_auth.json and read by RenderHubPanel
// at Connect time. Patron-only access — Tester ($5+) tier required
// during testing, mapped via Patreon→Discord role automation.
void LauncherUI::RenderDiscordAuthWindow() {
    using namespace fm2k::discord_auth;
    static std::unique_ptr<Pairing> s_pairing;
    static std::string              s_status;
    static fm2k::discord_auth::CachedAuth s_cached = LoadCached();

    if (!ImGui::Begin(T("hub_signin"), &show_discord_auth_,
                      ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        return;
    }

    if (s_cached.valid) {
        ImGui::TextColored(ImVec4(0.3f, 0.8f, 0.4f, 1.0f),
                           "Signed in as %s", s_cached.nick.c_str());
        ImGui::TextDisabled(T("label_discord_id"), s_cached.discord_user_id.c_str());
        if (ImGui::Button(T("hub_signout"))) {
            ClearCached();
            s_cached = CachedAuth{};
            // Tell the menu-bar pill to flip back to the orange
            // "Sign in with Discord" state on the next render.
            discord_signed_in_ = false;
            discord_nick_.clear();
        }
        ImGui::SameLine();
    }

    const bool busy = s_pairing && s_pairing->status() == Pairing::Status::Pending;
    if (busy) ImGui::BeginDisabled();
    if (ImGui::Button(s_cached.valid ? T("hub_resignin") : T("hub_signin"))) {
        // Build hub HTTP base URL from the configured Hub Server host.
        // Note: separate port from the WebSocket. The hub's HUB_HTTP_PORT
        // (default 7700) handles the OAuth callback; WS stays on 7711.
        std::string base = "http://";
        base += hub_host_[0] ? hub_host_ : "2dfm.sytes.net";
        base += ":7700";
        s_pairing.reset(Begin(base));
        s_status = "Browser opened. Click Authorize on Discord and come back.";
    }
    if (busy) ImGui::EndDisabled();

    if (s_pairing) {
        switch (s_pairing->status()) {
            case Pairing::Status::Pending:
                ImGui::TextWrapped("%s", s_status.c_str());
                break;
            case Pairing::Status::Ok: {
                auto a = s_pairing->result();
                if (SaveCached(a)) s_cached = a;
                s_status = "Signed in as " + a.nick + ". You can connect to the hub now.";
                s_pairing.reset();
                // Refresh the menu-bar pill so it flips green this frame.
                discord_signed_in_ = true;
                discord_nick_      = a.nick;
                // Auto-close after a brief moment so the user sees the
                // success message but isn't stuck on a modal-feeling
                // window. Closing here is immediate; the
                // confirmation lives on the menu-bar pill which now
                // shows the green "Discord: <nick>" state.
                show_discord_auth_ = false;
                break;
            }
            case Pairing::Status::Expired:
                ImGui::TextColored(ImVec4(0.85f, 0.6f, 0.3f, 1.0f),
                                   "%s", s_pairing->error_detail().c_str());
                if (ImGui::Button(T("btn_dismiss"))) s_pairing.reset();
                break;
            case Pairing::Status::Error:
                ImGui::TextColored(ImVec4(0.95f, 0.32f, 0.32f, 1.0f),
                                   "%s", s_pairing->error_detail().c_str());
                if (ImGui::Button(T("btn_dismiss"))) s_pairing.reset();
                break;
        }
    }

    ImGui::Separator();
    ImGui::TextWrapped(
        "Tester ($5+) tier on Patreon required during testing. Pledge on "
        "Patreon, link your Discord on the Patreon Connections page, then "
        "click Sign in here. Patreon assigns the Discord role automatically; "
        "the hub checks your roles when you sign in.");
    ImGui::End();
}

// Host-side match settings UI. SOCD mode + stage selection for now;
// round count / time limit / game speed are forward-compat fields whose
// addresses aren't mapped per-game yet. Settings are saved to fm2k_host.ini
// next to the launcher and pushed over the control channel via the hook
// DLL's Netplay_BroadcastHostConfig.
void LauncherUI::RenderHostConfigWindow() {
    if (!ImGui::Begin("Host Config", &show_host_config_, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        return;
    }

    ImGui::TextWrapped(
        "These settings apply to YOUR game and (when you host) get pushed "
        "to the connected client + spectators automatically.");
    ImGui::Separator();

    static const char* kSocdLabels[6] = {
        "0 — Default        (R wins L+R, U wins U+D)",
        "1 — Hitbox SOCD    (L+R neutral, U wins U+D)  [tournament default]",
        "2 — U/D Cancel     (R wins L+R, U+D neutral)",
        "3 — Both Cancel    (L+R neutral, U+D neutral)",
        "4 — Up Bias        (R wins L+R, U wins U+D)",
        "5 — Hitbox + UpBias",
    };
    ImGui::Text("%s", T("label_socd_mode"));
    if (ImGui::Combo("##socd", &host_config_socd_mode_, kSocdLabels, 6)) {
        host_config_dirty_ = true;
    }

    ImGui::Spacing();
    ImGui::Text("%s", T("label_selected_stage"));
    int stage_int = (int)host_config_stage_;
    if (ImGui::InputInt("##stage", &stage_int, 1, 1)) {
        if (stage_int < 0) stage_int = -1;
        host_config_stage_ = (uint32_t)stage_int;
        host_config_dirty_ = true;
    }

    ImGui::Spacing();
    ImGui::TextDisabled(
        "Round count / time limit / game speed: not yet mapped to game memory.");
    ImGui::TextDisabled(
        "For now both peers must use the same install + game.ini.");

    ImGui::Separator();
    if (host_config_dirty_) {
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f),
            "Unsaved changes — apply before hosting.");
    }
    if (ImGui::Button(T("btn_apply_host_config"))) {
        FILE* f = fopen("fm2k_host.ini", "w");
        if (f) {
            fprintf(f, "[Host]\nSocdMode=%d\nSelectedStage=%u\n",
                    host_config_socd_mode_, host_config_stage_);
            fclose(f);
        }
        // Set env vars so the freshly-spawned hook DLL picks these up at
        // its first SOCD-mode read (Hook_GetSOCDMode caches on first call).
        char socd_buf[8];
        snprintf(socd_buf, sizeof(socd_buf), "%d", host_config_socd_mode_);
        _putenv_s("FM2K_SOCD_MODE", socd_buf);
        host_config_dirty_ = false;
    }

    ImGui::End();
}

void LauncherUI::RenderGameSelection() {
    ImGui::Text("%s", T("panel_games_folder"));
    static char path_buf[512] = {0};
    static bool initialized = false;
    
    // Only initialize the buffer once, not every frame
    if (!initialized) {
        SDL_strlcpy(path_buf, games_root_path_.c_str(), sizeof(path_buf));
        initialized = true;
    }
    
    // Create a focus scope for the input group
    ImGui::PushID("GamesFolder");
    
    // Check if input text has changed
    bool path_changed = ImGui::InputText("##GamesFolder", path_buf, sizeof(path_buf));
    
    ImGui::SameLine();
    if (ImGui::Button(T("btn_set")) || (path_changed && ImGui::IsKeyPressed(ImGuiKey_Enter))) {
        if (on_games_folder_set) {
            // Update our internal path to match what user typed
            games_root_path_ = path_buf;
            on_games_folder_set(path_buf);
        }
    }
    ImGui::PopID();
    
    ImGui::Separator();
    ImGui::Text("%s", T("panel_available_games"));
    ImGui::Separator();

    if (scanning_games_) {
        ImGui::Text("%s", T("status_scanning_for_games"));
    } else if (games_.empty()) {
        ImGui::Text("%s", T("status_no_games_found"));
        ImGui::Text("%s", T("status_invalid_games_folder"));
    } else {
        // Simple list without child window to avoid focus scope conflicts
        for (size_t i = 0; i < games_.size(); ++i) {
            const auto& game = games_[i];
            if (!game.is_host) {
                continue; // Skip invalid entries
            }
            
            bool is_selected = (static_cast<int>(i) == selected_game_index_);
            
            // Use PushID with integer to avoid string pointer issues
            ImGui::PushID(static_cast<int>(i));
            
            if (ImGui::Selectable(game.GetExeName().c_str(), is_selected)) {
                selected_game_index_ = static_cast<int>(i);
                if (on_game_selected) {
                    on_game_selected(game);
                }
                // Route the input binder to this game's per-game profile
                // (creates fm2k_inputs_<basename>.ini lookup; reads
                // default if no override exists). Strip .exe suffix.
                std::filesystem::path p(game.exe_path);
                std::string stem = p.stem().string();
                FM2KInputBinder::SetGameProfile(stem.c_str());
                if (input_binder_initialized_) {
                    FM2KInputBinder::Load();
                }
            }
            
            if (is_selected) {
                ImGui::SetItemDefaultFocus();
            }
            
            // Tooltips restored - font stack issue is fixed
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("EXE: %s\nKGT: %s", game.exe_path.c_str(), game.dll_path.c_str());
            }
            
            ImGui::PopID();
        }
    }
}

void LauncherUI::RenderNetworkConfig() {
    if (ImGui::CollapsingHeader(T("panel_network_config"), ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Indent();

        // Session Type (Host/Join)
        static int session_type = 0; // 0: Host, 1: Join
        ImGui::RadioButton(T("netcfg_host"), &session_type, 0); ImGui::SameLine();
        ImGui::RadioButton(T("netcfg_join"), &session_type, 1);

        network_config_.is_host = (session_type == 0);

        if (network_config_.is_host) {
            // HOST: Show ip:port and copy to clipboard on click
            // Get actual local IP from first non-loopback adapter
            // Get external IP via HTTP (same approach as CCCaster).
            // Queries checkip.amazonaws.com which returns just the IP as text.
            static char local_ip[64] = "Resolving...";
            static bool ip_resolved = false;
            if (!ip_resolved) {
                ip_resolved = true;
                HINTERNET hInternet = InternetOpenA("FM2K", INTERNET_OPEN_TYPE_DIRECT, NULL, NULL, 0);
                if (hInternet) {
                    // Try multiple services like CCCaster does
                    const char* services[] = {
                        "http://checkip.amazonaws.com",
                        "http://ipv4.icanhazip.com",
                        "http://ifcfg.net",
                    };
                    for (const char* url : services) {
                        HINTERNET hUrl = InternetOpenUrlA(hInternet, url, NULL, 0,
                            INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_RELOAD, 0);
                        if (hUrl) {
                            char buf[256] = {};
                            DWORD bytesRead = 0;
                            if (InternetReadFile(hUrl, buf, sizeof(buf) - 1, &bytesRead) && bytesRead >= 7) {
                                buf[bytesRead] = '\0';
                                // Trim whitespace/newlines
                                char* p = buf;
                                while (*p == ' ' || *p == '\r' || *p == '\n') p++;
                                char* end = p + strlen(p) - 1;
                                while (end > p && (*end == ' ' || *end == '\r' || *end == '\n')) *end-- = '\0';
                                strncpy(local_ip, p, sizeof(local_ip) - 1);
                                InternetCloseHandle(hUrl);
                                break;
                            }
                            InternetCloseHandle(hUrl);
                        }
                    }
                    InternetCloseHandle(hInternet);
                }
                if (strcmp(local_ip, "Resolving...") == 0) {
                    strncpy(local_ip, "Could not resolve", sizeof(local_ip));
                }
            }
            char address_with_port[128];
            snprintf(address_with_port, sizeof(address_with_port), "%s:%d", local_ip, network_config_.local_port);

            ImGui::Text("%s", T("label_your_address"));
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "%s", address_with_port);
            ImGui::SameLine();
            if (ImGui::Button(T("btn_copy"))) {
                SDL_SetClipboardText(address_with_port);
            }

            // Port (editable for host)
            ImGui::SetNextItemWidth(100);
            ImGui::InputInt("Port", &network_config_.local_port, 0, 0, ImGuiInputTextFlags_CharsDecimal);
        } else {
            // JOIN: Single paste field for ip:port
            static char paste_buf[128] = "";
            ImGui::SetNextItemWidth(200);
            if (ImGui::InputTextWithHint("##join_addr", "Paste host ip:port here", paste_buf, sizeof(paste_buf))) {
                network_config_.remote_address = paste_buf;
            }
            ImGui::SameLine();
            if (ImGui::Button(T("btn_paste"))) {
                const char* clipboard = SDL_GetClipboardText();
                if (clipboard && clipboard[0]) {
                    strncpy(paste_buf, clipboard, sizeof(paste_buf) - 1);
                    paste_buf[sizeof(paste_buf) - 1] = '\0';
                    network_config_.remote_address = paste_buf;
                }
            }

            // Local port (editable for client — required to avoid collisions when both peers run on localhost)
            ImGui::SetNextItemWidth(100);
            ImGui::InputInt("Local Port", &network_config_.local_port, 0, 0, ImGuiInputTextFlags_CharsDecimal);
        }

        // Input Delay
        ImGui::SetNextItemWidth(100);
        ImGui::SliderInt("Input Delay (frames)", &network_config_.input_delay, 0, 10);

        ImGui::Unindent();
    }
}

void LauncherUI::RenderConnectionStatus() {
    // ImGui popup IDs are hashed from the title string. Keep the ID stable
    // across language switches by appending a `##` suffix — text after `##`
    // is treated as ID-only and never displayed. This way the visible
    // title localizes but the popup keeps its identity if someone changes
    // language while a popup is open.
    if (launcher_state_ == LauncherState::Connecting) {
        ImGui::OpenPopup("##connecting_modal");
    }

    if (ImGui::BeginPopupModal("##connecting_modal", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("%s", T("modal_connecting_body"));
        if (launcher_state_ != LauncherState::Connecting) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (launcher_state_ == LauncherState::Disconnected) {
        ImGui::OpenPopup("##disconnected_modal");
    }

    if (ImGui::BeginPopupModal("##disconnected_modal", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("%s", T("hub_status_connection_lost"));
        if (ImGui::Button(T("btn_ok"))) {
            if (on_session_stop) on_session_stop();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void LauncherUI::RenderInGameUI() {
    // Only show during active gameplay
    if (launcher_state_ != LauncherState::InGame) {
        return;
    }
    
    // Network diagnostics are now shown in the debug tools panel
    // This function is no longer needed but kept for backwards compatibility
}

// Note: ShowGameValidationStatus removed ? UI simplified
/* void LauncherUI::ShowGameValidationStatus(const FM2K::FM2KGameInfo& game) {} */

void LauncherUI::ShowNetworkDiagnostics() {}

bool LauncherUI::ValidateNetworkConfig() {
    // Check if remote address is valid format
    if (network_config_.remote_address.empty()) {
        return false;
    }
    
    // Check if port is in valid range
    if (network_config_.local_port < 1024 || network_config_.local_port > 65535) {
        return false;
    }
    
    // Basic IP:port format check
    size_t colon_pos = network_config_.remote_address.find(':');
    if (colon_pos == std::string::npos) {
        return false;
    }
    
    return true;
}

// Data binding methods
void LauncherUI::SetGames(const std::vector<FM2K::FM2KGameInfo>& games) {
    games_ = games;
}

void LauncherUI::SetNetworkConfig(const NetworkConfig& config) {
    network_config_ = config;
}


void LauncherUI::SetLauncherState(LauncherState state) {
    launcher_state_ = state;
}

void LauncherUI::SetScanning(bool scanning) {
    scanning_games_ = scanning;
}

void LauncherUI::SetGamesRootPath(const std::string& path) {
    games_root_path_ = path;
}

void LauncherUI::SetFramesAhead(float frames_ahead) {
    frames_ahead_ = frames_ahead;
}

// Simplified theme - always use Dark
void LauncherUI::SetTheme(UITheme theme) {
    current_theme_ = UITheme::Dark;
    ImGui::StyleColorsDark();
}

void LauncherUI::RenderSessionControls() {
    if (ImGui::CollapsingHeader(T("panel_session_controls"), ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Indent();

        // Disable buttons if no game is selected
        bool game_selected = selected_game_index_ >= 0 && selected_game_index_ < games_.size();
        if (!game_selected) {
            ImGui::BeginDisabled();
        }

        // Boot/auto-mash defaults — applied to every offline launch, both
        // dev and end-user paths. End users never see these toggles.
        static int  s_boot_strategy     = 0;     // 0=safe, 1=fast
        static bool s_auto_title_skip   = true;
        static bool s_bypass_trampoline = false;
        static bool s_force_t4_patch    = false;
        static bool s_skip_vs_mode_patch= false;
        static bool s_t4_probe          = false;
        // Persisted across launcher restarts via %APPDATA%\FM2K_Rollback\dev_flags.ini.
        // First-frame init reads the saved value; toggling the checkbox writes back.
        static bool s_eb_diag = []() {
            bool v = LoadDevFlag("eb_diag", false);
            // Apply immediately on launcher start so any auto-launched session
            // (offline / online / hub) inherits the env var.
            ::SetEnvironmentVariableA("FM2K_EB_DIAG", v ? "1" : nullptr);
            return v;
        }();

        // ---------- USER-FACING SECTION ----------
        ImGui::Text("%s", T("label_play"));
        ImGui::Separator();

        if (ImGui::Button(T("btn_play_online"), ImVec2(-1, 0))) {
            // Move the user to the Hub panel and, if they've already
            // got a game selected and a hub connection, drop them into
            // a per-game lobby on demand. Hub creates rooms lazily on
            // join_room — no master room list needed; the room id is
            // the exe stem so two players on the same game converge.
            ImGui::SetWindowFocus("Hub");
            const bool game_selected =
                selected_game_index_ >= 0 &&
                selected_game_index_ < (int)games_.size();
            if (game_selected && hub_state_ && hub_state_->client.IsConnected()) {
                std::filesystem::path exe(games_[selected_game_index_].exe_path);
                const std::string game_id = exe.stem().string();
                if (hub_state_->current_room_id != game_id) {
                    hub_state_->client.JoinRoom(game_id, game_id);
                }
            }
        }
        ImGui::SetItemTooltip(
            "Switch to the Hub panel. If a game is selected and you're "
            "connected, joins (or creates) a lobby for that game.");

        if (ImGui::Button(T("btn_play_offline"), ImVec2(-1, 0))) {
            ::SetEnvironmentVariableA("FM2K_BOOT_TO_CSS_DIRECT",
                                      s_boot_strategy == 1 ? "1" : nullptr);
            ::SetEnvironmentVariableA("FM2K_AUTO_TITLE_SKIP",
                                      s_auto_title_skip ? nullptr : "0");
            ::SetEnvironmentVariableA("FM2K_BYPASS_TRAMPOLINE",
                                      s_bypass_trampoline ? "1" : nullptr);
            ::SetEnvironmentVariableA("FM2K_FORCE_T4_PATCH",
                                      s_force_t4_patch ? "1" : nullptr);
            ::SetEnvironmentVariableA("FM2K_SKIP_VS_MODE_PATCH",
                                      s_skip_vs_mode_patch ? "1" : nullptr);
            ::SetEnvironmentVariableA("FM2K_T4_PROBE",
                                      s_t4_probe ? "1" : nullptr);
            ::SetEnvironmentVariableA("FM2K_EB_DIAG",
                                      s_eb_diag ? "1" : nullptr);
            if (on_offline_session_start) {
                on_offline_session_start();
            }
        }
        ImGui::SetItemTooltip("%s", T("btn_play_offline_tooltip"));

        // ---------- DEVELOPER SECTION ----------
        if (developer_mode_) {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "%s", T("dev_section_header"));
            ImGui::Separator();

            ImGui::Text("%s", T("dev_boot_strategy"));
            ImGui::RadioButton(T("dev_boot_safe"), &s_boot_strategy, 0);
            ImGui::SetItemTooltip(
                "Boots to title_screen_manager (skips intro cutscene). The "
                "hook auto-mashes button A with cursor pre-set to VS Player. "
                "Works on every game — adds ~10 frames to boot.");
            ImGui::RadioButton(T("dev_boot_fast"), &s_boot_strategy, 1);
            ImGui::SetItemTooltip(
                "Skips title screen entirely. WORKS on WW. BREAKS StudioS "
                "Fighters / Strip Fighter Zero — characters self-damage on "
                "frame 0. Only enable per-game once verified safe.");

            ImGui::Checkbox(T("dev_auto_title_skip"), &s_auto_title_skip);
            ImGui::SetItemTooltip(
                "Default ON. Disable to walk title screen manually with your "
                "own inputs.");

            ImGui::Spacing();
            ImGui::Text("%s", T("dev_diagnostics"));
            ImGui::Checkbox(T("dev_bypass_trampoline"), &s_bypass_trampoline);
            ImGui::SetItemTooltip(
                "Routes Hook_RunGameLoop to vanilla. Other hooks still fire. "
                "Offline only — netplay/spectator require the trampoline.");

            ImGui::Checkbox(T("dev_skip_vs_mode_patch"), &s_skip_vs_mode_patch);
            ImGui::SetItemTooltip("%s", T("dev_skip_vs_mode_tooltip"));

            ImGui::Checkbox(T("dev_force_t4_patch"), &s_force_t4_patch);
            ImGui::SetItemTooltip(
                "Re-enables the case-200 t4-walk neuter patch (0x408EC5).");

            ImGui::Checkbox(T("dev_t4_probe"), &s_t4_probe);
            ImGui::SetItemTooltip("%s", T("dev_t4_probe_tooltip"));

            if (ImGui::Checkbox(T("dev_eb_diag"), &s_eb_diag)) {
                // Apply immediately so EVERY launch path (offline, online,
                // hub, dual-clients, spectator) inherits the env var.
                // Persist to dev_flags.ini so the toggle survives launcher
                // restarts — otherwise the static-bool default loses your
                // setting every time you close the launcher.
                ::SetEnvironmentVariableA("FM2K_EB_DIAG",
                                          s_eb_diag ? "1" : nullptr);
                SaveDevFlag("eb_diag", s_eb_diag);
            }
            ImGui::SetItemTooltip(
                "Logs shake-effect timer values at PRE-SAVE / PRE-RENDER / "
                "POST-RENDER / POST-RESTORE around the trampoline render "
                "boundary. Use to track [EB] palette-flash and screen-shake "
                "duration loss. Output goes to FM2K_eb_diag_pid<PID>.log "
                "in the game folder (NOT the main launcher log). Repro: "
                "pkmncc Bewear 624B, slither wing 6A landing, URORFG Loader "
                "5B / walking, Breloom 6a6a6b. Persists across launcher "
                "restarts.");

            ImGui::Spacing();
            if (ImGui::Button(T("dev_online_legacy"), ImVec2(-1, 0))) {
                if (on_online_session_start) {
                    on_online_session_start(network_config_);
                }
            }
            ImGui::SetItemTooltip("%s", T("dev_online_legacy_tip"));

            if (ImGui::Button(T("dev_stress_test"), ImVec2(-1, 0))) {
                if (on_stress_session_start) {
                    on_stress_session_start();
                }
            }
            ImGui::SetItemTooltip(
                "GekkoStressSession with a single instance. Forces rollback every 10 frames "
                "and compares save hashes — any DESYNC = local determinism bug.");

            ImGui::Spacing();
            ImGui::Text("%s", T("dev_local_testing"));
            ImGui::Separator();
        
        // Get client status for dual client button
        uint32_t client1_pid = 0, client2_pid = 0;
        bool clients_running = false;
        if (on_get_client_status) {
            clients_running = on_get_client_status(client1_pid, client2_pid);
        }
        
        if (clients_running) {
            ImGui::BeginDisabled();
        }
        
        if (ImGui::Button(T("dev_launch_dual"), ImVec2(-1, 0))) {
            if (on_launch_local_client1 && on_launch_local_client2 && game_selected) {
                const auto& selected_game = games_[selected_game_index_];

                // Both clients launch from the same folder. The hook's
                // BypassMultiInstanceCheck patch disables FM2K's own
                // FindWindow("KGT2KGAME") guard, and shared memory is
                // PID-namespaced. Mutable file collisions (.ini, save data)
                // are tolerable for testing; if they become a problem,
                // revisit with per-instance shadow folders.
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Starting dual clients...");
                bool success1 = on_launch_local_client1(selected_game.exe_path);
                if (success1) {
                    bool success2 = on_launch_local_client2(selected_game.exe_path);
                    if (!success2) {
                        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to launch Client 2");
                    }
                }
            }
        }
        
        if (clients_running) {
            ImGui::EndDisabled();
        }

        ImGui::SetItemTooltip("%s", T("dev_launch_dual_tip"));

        // "Launch Spectator" — spawns a third local instance that subscribes
        // to client1 (host on 7000) for replay-streamed spectating. Only
        // makes sense after Launch Dual Clients has the host running.
        bool can_spectate = on_launch_local_spectator && game_selected && client1_pid != 0;
        if (!can_spectate) ImGui::BeginDisabled();
        if (ImGui::Button(T("dev_launch_spectator"), ImVec2(-1, 0))) {
            if (can_spectate) {
                const auto& selected_game = games_[selected_game_index_];
                bool ok = on_launch_local_spectator(selected_game.exe_path);
                if (!ok) {
                    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to launch spectator");
                }
            }
        }
        if (!can_spectate) ImGui::EndDisabled();
        ImGui::SetItemTooltip("%s", T("dev_launch_spectator_tip"));

        // "Launch Spectator 2 (chain)" — daisy-chain test. Subscribes to
        // spectator 1 (port 7002) instead of the host. Validates that
        // spectator 1 correctly relays its received frames to its own
        // subscribers. Disabled until both dual clients + spectator 1 are
        // running.
        bool can_spectate2 = on_launch_local_spectator2 && game_selected && client1_pid != 0;
        if (!can_spectate2) ImGui::BeginDisabled();
        if (ImGui::Button(T("dev_launch_spectator2"), ImVec2(-1, 0))) {
            if (can_spectate2) {
                const auto& selected_game = games_[selected_game_index_];
                bool ok = on_launch_local_spectator2(selected_game.exe_path);
                if (!ok) {
                    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to launch spectator 2");
                }
            }
        }
        if (!can_spectate2) ImGui::EndDisabled();
        ImGui::SetItemTooltip("%s", T("dev_launch_spectator2_tip"));

        if (clients_running) {
            ImGui::Text(T("clients_running"), client1_pid, client2_pid);
            if (ImGui::Button(T("dev_terminate_clients"), ImVec2(-1, 0))) {
                if (on_terminate_all_clients) {
                    on_terminate_all_clients();
                }
            }
        }
        }  // end if (developer_mode_)

        ImGui::Spacing();
        ImGui::Separator();

        if (ImGui::Button(T("btn_stop_session"), ImVec2(-1, 0))) {
            if (on_session_stop) {
                on_session_stop();
            }
        }
        ImGui::SetItemTooltip("%s", T("btn_stop_session_tooltip"));

        if (!game_selected) {
            ImGui::EndDisabled();
        }

        ImGui::Unindent();
    }
}

void LauncherUI::NotifyHubMatchEnded() {
    if (!hub_state_) return;
    if (!hub_state_->client.IsConnected()) return;
    hub_state_->client.MatchEnded();
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Hub: signaled match_ended (game terminated)");
}

void LauncherUI::RenderHubPanel() {
    auto& hs = *hub_state_;

    // Drain hub events into local state once per frame.
    hs.client.Poll([&](const fm2k::HubEvent& ev) {
        using K = fm2k::HubEvent::Kind;
        switch (ev.kind) {
            case K::Connected:
                hs.my_id = ev.user_id;
                hs.rooms = ev.rooms;
                hs.status_line = "connected";
                // Tell the hub our planned UDP listen so it can relay
                // it to a peer in match_start. Both launchers register
                // their already-configured network_config_.local_port.
                // For LAN/internet, replace "127.0.0.1" with the hub-
                // observed reflexive IP (Phase 2 — STUN responder).
                hs.client.SendUdpAddr("127.0.0.1", network_config_.local_port);
                break;
            case K::Disconnected:
                hs.users.clear();
                hs.current_room_id.clear();
                hs.my_id.clear();
                hs.status_line = ev.error.empty() ? "disconnected" : ("disconnected: " + ev.error);
                break;
            case K::RoomList:
                hs.rooms = ev.rooms;
                break;
            case K::RoomJoined: {
                if (!ev.rooms.empty()) hs.current_room_id = ev.rooms.front().id;
                hs.users.clear();
                for (auto& u : ev.users) hs.users[u.id] = u;
                // Auto-select the installed game matching this room and
                // ALSO fire on_game_selected so the launcher's
                // FM2KLauncher::selected_game_ record is populated —
                // not just our local UI mirror selected_game_index_.
                // Without this, StartOnlineSession bails on
                // selected_game_.exe_path.empty() even though the UI
                // showed a selection.
                int idx = FindInstalledGameForRoom(games_, hs.current_room_id);
                if (idx >= 0) {
                    selected_game_index_ = idx;
                    if (on_game_selected) on_game_selected(games_[idx]);
                    hs.status_line = "auto-selected installed game: "
                        + std::filesystem::path(games_[idx].exe_path).stem().string();
                } else {
                    hs.status_line = "joined room '" + hs.current_room_id +
                        "' — game not in your library, install it before challenging";
                }
                break;
            }
            case K::RoomLeft:
                hs.current_room_id.clear();
                hs.users.clear();
                break;
            case K::UserJoined:
                if (ev.room_id == hs.current_room_id) hs.users[ev.user.id] = ev.user;
                break;
            case K::UserLeft:
                if (ev.room_id == hs.current_room_id) hs.users.erase(ev.user_id);
                break;
            case K::UserStatus:
                hs.users[ev.user.id] = ev.user;
                break;
            case K::UserRtt:
                if (auto it = hs.users.find(ev.user_id); it != hs.users.end()) {
                    it->second.rtt_ms = ev.rtt_ms;
                }
                break;
            case K::ChallengeReceived:
                hs.pending_challenge_from_id   = ev.challenge.from_id;
                hs.pending_challenge_from_nick = ev.challenge.from_nick;
                hs.show_challenge_modal = true;
                FireChallengeNotification(ev.challenge.from_nick);
                break;
            case K::ChallengeFailed:
                hs.status_line = "challenge failed: " + ev.error;
                hs.show_outgoing_challenge_modal = false;
                hs.outgoing_challenge_to_id.clear();
                hs.outgoing_challenge_to_nick.clear();
                break;
            case K::ChallengeCancelled:
                // Server tells US our outbound challenge was cancelled
                // (e.g., target went offline) OR an inbound challenge
                // was cancelled by the sender. Same handling for both:
                // close any open modal that referenced it.
                hs.show_challenge_modal = false;
                hs.pending_challenge_from_id.clear();
                hs.show_outgoing_challenge_modal = false;
                hs.outgoing_challenge_to_id.clear();
                hs.outgoing_challenge_to_nick.clear();
                hs.status_line = "challenge cancelled";
                break;
            case K::ChallengeDeclined:
                hs.status_line = "challenge declined by " +
                    (hs.outgoing_challenge_to_nick.empty()
                        ? std::string("opponent")
                        : hs.outgoing_challenge_to_nick);
                hs.show_outgoing_challenge_modal = false;
                hs.outgoing_challenge_to_id.clear();
                hs.outgoing_challenge_to_nick.clear();
                break;
            case K::MatchStart: {
                // Match is on — drop the waiting modal.
                hs.show_outgoing_challenge_modal = false;
                hs.outgoing_challenge_to_id.clear();
                hs.outgoing_challenge_to_nick.clear();
                hs.status_line = "match_start: " + ev.match.role +
                    " peer=" + ev.match.peer.nick +
                    " udp=" + ev.match.peer_udp_ip + ":" +
                    std::to_string(ev.match.peer_udp_port);
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Hub: %s", hs.status_line.c_str());

                // Three preconditions must hold to actually launch:
                //   (1) peer reported a non-zero UDP port
                //   (2) we have the room's game installed
                //   (3) the launcher exposes on_online_session_start
                // Failing any of these, tell the hub the "match" is over
                // immediately so both peers go back to idle — otherwise
                // the lobby reads "in_match" forever and they can't
                // re-challenge or pick a new game.
                int idx = FindInstalledGameForRoom(games_, hs.current_room_id);
                bool ok = (ev.match.peer_udp_port > 0)
                       && (idx >= 0)
                       && (on_online_session_start != nullptr);

                if (ok) {
                    // Preflight punch — purely informational. We send
                    // a quick burst of UDP probes to the peer to wake
                    // up the NAT mappings so the in-game GekkoNet
                    // handshake has a head start, then ALWAYS proceed
                    // to spawn the game. The previous "abort if probe
                    // doesn't reply" gate killed legitimate matches
                    // because home-router NATs frequently take longer
                    // than 2 seconds to punch (or never punch directly
                    // and need relay), and the loopback fallback only
                    // works for same-box tests. The in-game NAT layer
                    // (nat_traversal.cpp) handles STUN, multiple punch
                    // rounds, and relay engagement properly — let it
                    // do its job instead of failing fast here.
                    //
                    // We still TRY the probe so cone-NAT pairs benefit
                    // from a few packets in-flight before launch, and
                    // we still detect the same-box loopback case so the
                    // game gets FM2K_REMOTE_ADDR=127.0.0.1 for that
                    // setup specifically.
                    hs.status_line = "preflight: punching peer (best-effort)...";
                    std::string peer_ip   = ev.match.peer_udp_ip;
                    int         peer_port = ev.match.peer_udp_port;
                    const bool public_reachable = HubPreflightPunch(
                        static_cast<uint16_t>(network_config_.local_port),
                        peer_ip,
                        static_cast<uint16_t>(peer_port),
                        ev.match.token,
                        2000);
                    if (!public_reachable) {
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Hub: public probe timed out — trying 127.0.0.1 "
                            "in case this is a same-box test");
                        if (HubPreflightPunch(
                                static_cast<uint16_t>(network_config_.local_port),
                                "127.0.0.1",
                                static_cast<uint16_t>(peer_port),
                                ev.match.token,
                                1000)) {
                            peer_ip = "127.0.0.1";
                            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                "Hub: loopback responded — same-box match, "
                                "using 127.0.0.1 as remote");
                        } else {
                            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                                "Hub: probe didn't get a reply on either path. "
                                "Spawning game anyway — in-game NAT traversal "
                                "(STUN + punch + relay) will retry on its own. "
                                "If you stall in 'Connecting...' for >10s, your "
                                "NAT probably needs the relay path.");
                        }
                    } else {
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Hub: public probe succeeded — direct path looks good");
                    }
                    hs.status_line = "match starting...";

                    // Make sure the launcher's selected_game_ record is
                    // up to date even if RoomJoined fired before games
                    // discovery completed.
                    if (on_game_selected) on_game_selected(games_[idx]);

                    // Plumb hub coordinates through the spawned game's
                    // env so FM2KHook's nat_traversal can fire a STUN
                    // probe and authenticated punch on Netplay_Init.
                    // Inherited via CreateProcess in
                    // FM2KGameInstance::Launch.
                    //
                    // Use the same FM2K_HUB_HOST override as the WS
                    // Connect above. nat_traversal does its own DNS
                    // resolve on this string — it's not required to be
                    // a literal IP.
                    const char* hub_host_env = std::getenv("FM2K_HUB_HOST");
                    const std::string hub_udp =
                        std::string(hub_host_env && hub_host_env[0]
                                    ? hub_host_env : "2dfm.sytes.net")
                        + ":7711";
                    ::SetEnvironmentVariableA("FM2K_HUB_UDP_ADDR",   hub_udp.c_str());
                    ::SetEnvironmentVariableA("FM2K_HUB_USER_ID",    hs.my_id.c_str());
                    ::SetEnvironmentVariableA("FM2K_HUB_MATCH_TOKEN", ev.match.token.c_str());
                    if (!ev.match.relay_ip.empty() && ev.match.relay_port > 0) {
                        std::string relay_addr = ev.match.relay_ip + ":" +
                                                 std::to_string(ev.match.relay_port);
                        ::SetEnvironmentVariableA("FM2K_HUB_RELAY_ADDR",    relay_addr.c_str());
                        ::SetEnvironmentVariableA("FM2K_HUB_RELAY_SESSION",
                                                  ev.match.relay_session_id.c_str());
                    } else {
                        ::SetEnvironmentVariableA("FM2K_HUB_RELAY_ADDR",    nullptr);
                        ::SetEnvironmentVariableA("FM2K_HUB_RELAY_SESSION", nullptr);
                    }

                    NetworkConfig cfg = network_config_;
                    cfg.session_mode = SessionMode::ONLINE;
                    cfg.is_host = (ev.match.role == "host");
                    // Use the peer addr the preflight actually
                    // succeeded on (may have been swapped to
                    // 127.0.0.1 above). The spawned game's
                    // ControlChannel needs the same path the
                    // pinhole opened on.
                    cfg.remote_address =
                        peer_ip + ":" + std::to_string(peer_port);
                    on_online_session_start(cfg);
                } else {
                    const char* reason =
                        (ev.match.peer_udp_port == 0) ? "peer never sent udp_addr" :
                        (idx < 0)                     ? "game not in your library" :
                                                        "launcher missing online callback";
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Hub: match_start aborted (%s) — sending match_ended", reason);
                    hs.status_line = std::string("match aborted: ") + reason;
                    hs.client.MatchEnded();
                }
                break;
            }
            case K::PeerDisconnected:
                hs.status_line = "peer disconnected";
                break;
            case K::SpectateGranted: {
                hs.status_line = "spectate: " + ev.spectate.target_nick +
                                 " vs " + ev.spectate.opponent_nick +
                                 " @ " + ev.spectate.host_ip + ":" +
                                 std::to_string(ev.spectate.host_port);
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Hub: %s", hs.status_line.c_str());
                if (on_spectate_match) {
                    on_spectate_match(ev.spectate.host_ip, ev.spectate.host_port);
                }
                break;
            }
            case K::SpectateDenied:
                hs.status_line = "spectate denied: " + ev.error;
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Hub: %s", hs.status_line.c_str());
                break;
            case K::Error:
                hs.status_line = "error: " + ev.error;
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "Hub: %s", hs.status_line.c_str());
                // Auth-required error: pop the Discord sign-in window so
                // the user knows what to do. Matches "auth_required"
                // reason from hub/hub.py.
                if (ev.error.find("auth_required") != std::string::npos) {
                    show_discord_auth_ = true;
                }
                break;
        }
    });

    // ---- UI ----
    ImGui::SeparatorText(T("hub_section_header"));

    // Nick input — 128-byte buffer covers 32 visible codepoints even at
    // 4 bytes per UTF-8 char (CJK / emoji). Hub caps incoming nicks to 32
    // codepoints + sanitizes control chars (see hub.py). Local buffer is
    // generous so the input field doesn't truncate mid-character.
    static char s_nick[128] = "";
    static bool s_use_discord_name = true;
    static std::string s_discord_global_name;
    // Pre-fill on first hub-panel render from the persisted auth cache so
    // (a) users who set a custom nick see it again, (b) the "Use Discord
    // name" checkbox tracks their last choice, and (c) we have the
    // authoritative Discord global_name available for when they flip the
    // checkbox back on.
    static bool s_nick_initialized = false;
    if (!s_nick_initialized) {
        const auto cached = fm2k::discord_auth::LoadCached();
        if (!cached.nick.empty()) {
            std::snprintf(s_nick, sizeof(s_nick), "%s", cached.nick.c_str());
        }
        s_use_discord_name    = cached.use_discord_name;
        s_discord_global_name = cached.discord_global_name;
        s_nick_initialized = true;
    }
    // Hub host string lives on the LauncherUI (member hub_host_) and is
    // edited from Settings → Hub Server… The Hub panel is read-only here.
    if (!hub_host_initialized_) {
        hub_host_initialized_ = true;
        const char* env_h = std::getenv("FM2K_HUB_HOST");
        const char* def   = (env_h && env_h[0]) ? env_h : "2dfm.sytes.net";
        std::snprintf(hub_host_, sizeof(hub_host_), "%s", def);
    }
    // Delay override panel. Default is "computed" (CCCaster-style auto-
    // pick at match session creation from the worst measured RTT — see
    // Netplay_StartBattleSession). Combo lets the user pin a manual
    // value if they want to eat a fixed delay rather than rely on the
    // computed pick. Manual override persists across matches via the
    // FM2K_LOCAL_DELAY env var; "computed" clears the var so the hook
    // falls back to the auto path.
    static int s_delay_override = 0;  // 0 = computed, 1..16 = manual frames
    {
        // Manual delay range: 1..16 (was 1..8). Bumped because some
        // intercontinental matches need delay >8 to ride out the worst-
        // case RTT spikes without rollback churn. 16 frames at 100 Hz
        // = 160 ms — past that the input lag is bad enough that nobody
        // wants to play anyway, so capping there is fine.
        const char* delay_items[] = {
            "computed",
            "1",  "2",  "3",  "4",  "5",  "6",  "7",  "8",
            "9", "10", "11", "12", "13", "14", "15", "16",
        };
        ImGui::PushItemWidth(-120);
        ImGui::Combo("Delay", &s_delay_override, delay_items,
                     IM_ARRAYSIZE(delay_items));
        ImGui::PopItemWidth();
        ImGui::SetItemTooltip(
            "Input delay (frames at 100 Hz). \"computed\" applies a "
            "CCCaster-style pick at match start: ceil(worst_one_way_ms "
            "/ 10) + 1, clamped [2, 15] — covers the worst spike since "
            "the prior match. Pin 1..16 to override and ride a fixed "
            "delay instead. 16 = 160 ms, basically the upper limit of "
            "playable delay-only netcode.");
        if (s_delay_override > 0) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "%d", s_delay_override);
            ::SetEnvironmentVariableA("FM2K_LOCAL_DELAY", buf);
        } else {
            ::SetEnvironmentVariableA("FM2K_LOCAL_DELAY", nullptr);
        }
    }

    if (!hs.client.IsConnected()) {
        // "Use Discord name" checkbox — when checked, the nick input is
        // grayed and shows the user's Discord global_name (read-only).
        // When unchecked, the user can edit their custom nick. Toggling
        // doesn't destroy the custom nick — it just switches WHICH value
        // gets sent on Connect. Persists immediately.
        if (ImGui::Checkbox(T("hub_use_discord_name"), &s_use_discord_name)) {
            auto cached_save = fm2k::discord_auth::LoadCached();
            if (cached_save.valid) {
                cached_save.use_discord_name = s_use_discord_name;
                fm2k::discord_auth::SaveCached(cached_save);
            }
        }
        // Display buffer: shows what'll be sent on Connect. When the
        // checkbox is on, that's discord_global_name (read-only). When
        // off, it's the editable custom nick (s_nick). Two separate
        // buffers under the hood so flipping the checkbox doesn't
        // clobber either source-of-truth.
        char display_buf[128];
        if (s_use_discord_name) {
            std::snprintf(display_buf, sizeof(display_buf), "%s",
                          s_discord_global_name.c_str());
        } else {
            std::snprintf(display_buf, sizeof(display_buf), "%s", s_nick);
        }
        ImGui::PushItemWidth(-120);
        if (s_use_discord_name) ImGui::BeginDisabled();
        if (ImGui::InputText(T("hub_nick"), display_buf, sizeof(display_buf))
            && !s_use_discord_name) {
            std::snprintf(s_nick, sizeof(s_nick), "%s", display_buf);
        }
        if (s_use_discord_name) ImGui::EndDisabled();
        ImGui::PopItemWidth();
        // Show the configured hub host as read-only context. Edit from
        // Settings → Hub Server…
        ImGui::TextDisabled(T("hub_server"), hub_host_[0] ? hub_host_ : "2dfm.sytes.net");
        ImGui::SameLine();
        if (ImGui::SmallButton("change")) {
            show_hub_server_ = true;
        }
        const auto cached_auth_check = fm2k::discord_auth::LoadCached();
        // nick_ok: when "Use Discord name" is on, validity depends on whether
        // we know what their Discord name actually is (populated post-OAuth).
        // When off, just whether they typed something.
        const bool nick_ok = s_use_discord_name
            ? !s_discord_global_name.empty()
            : (s_nick[0] != '\0');
        const bool signed_in  = cached_auth_check.valid;
        const bool can_connect = nick_ok && signed_in;
        const char* button_label =
            !signed_in ? "(sign in with Discord first)" :
            !nick_ok   ? "(set a nick first)" : "Connect";
        if (!can_connect) ImGui::BeginDisabled();
        if (ImGui::Button(button_label, ImVec2(-1, 0))) {
            // Pick the right nick to send: Discord global_name when the
            // checkbox is on, custom nick otherwise. Custom nick still
            // persists across the connect (so toggling the checkbox back
            // off restores the user's last custom value).
            const std::string outgoing_nick =
                s_use_discord_name ? s_discord_global_name : std::string(s_nick);
            hs.my_nick = outgoing_nick;
            // Persist nick + checkbox state to discord_auth.json.
            auto cached_save = fm2k::discord_auth::LoadCached();
            if (cached_save.valid) {
                bool dirty = false;
                if (cached_save.nick != s_nick) {
                    cached_save.nick = s_nick;
                    dirty = true;
                }
                if (cached_save.use_discord_name != s_use_discord_name) {
                    cached_save.use_discord_name = s_use_discord_name;
                    dirty = true;
                }
                if (dirty) fm2k::discord_auth::SaveCached(cached_save);
            }
            // Auto-pick a free UDP port: bind a socket to port 0
            // (OS-assigned ephemeral), read back the chosen port via
            // getsockname, close. Same-machine multi-launcher tests
            // get distinct ports automatically; users never need to
            // think about it. Cross-machine: any free port works.
            //
            // WSAStartup is required before socket() on Windows. It's
            // idempotent — internal refcount, fine to call repeatedly.
            // Without it socket() fails with WSANOTINITIALISED and the
            // fallback picks 7000, which then collides between two
            // launchers on the same box.
            int picked = 7000;
            WSADATA wsa{};
            WSAStartup(MAKEWORD(2, 2), &wsa);
            SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
            if (s == INVALID_SOCKET) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Hub: auto-pick socket() failed (err=%d) — falling back to 7000",
                    WSAGetLastError());
            } else {
                sockaddr_in addr{};
                addr.sin_family = AF_INET;
                addr.sin_port = 0;
                addr.sin_addr.s_addr = INADDR_ANY;
                if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
                    sockaddr_in bound{};
                    int len = sizeof(bound);
                    if (getsockname(s, reinterpret_cast<sockaddr*>(&bound), &len) == 0) {
                        picked = ntohs(bound.sin_port);
                    }
                } else {
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Hub: auto-pick bind() failed (err=%d)", WSAGetLastError());
                }
                closesocket(s);
            }
            network_config_.local_port = picked;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Hub: auto-picked UDP port %d for this session", picked);
            // Hub address from the Host field above. The same string
            // gets persisted into the FM2K_HUB_HOST env so the spawned
            // game's nat_traversal STUN probe / relay endpoint uses
            // the same host.
            const std::string hub_host = (hub_host_[0] != '\0') ? hub_host_ : "2dfm.sytes.net";
            ::SetEnvironmentVariableA("FM2K_HUB_HOST", hub_host.c_str());
            // Pull the cached Discord hub_token. Hub will reject the
            // hello with `auth_required` if missing/expired and the
            // launcher will surface the error in status_line.
            const auto cached = fm2k::discord_auth::LoadCached();
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Hub: connecting to %s:7711 (WS) auth=%s",
                        hub_host.c_str(),
                        cached.valid ? "present" : "missing");
            hs.client.Connect(hub_host, 7711, "/", hs.my_nick, cached.hub_token);
            hs.status_line = "connecting to " + hub_host + " ...";
        }
        if (!can_connect) ImGui::EndDisabled();
    } else {
        ImGui::TextColored(ImVec4(0.3f, 0.8f, 0.4f, 1.0f),
                           "Connected as %s", hs.my_nick.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton(T("hub_disconnect"))) {
            hs.client.Disconnect();
        }
    }
    if (!hs.status_line.empty()) {
        ImGui::TextDisabled("%s", hs.status_line.c_str());
    }
    if (!hs.client.IsConnected()) return;

    // ---- Rooms ----
    ImGui::SeparatorText(T("hub_rooms_header"));
    if (hs.rooms.empty()) {
        ImGui::TextDisabled("%s", T("hub_no_rooms"));
    }
    if (ImGui::BeginTable("##rooms", 4,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn(T("col_game"));
        ImGui::TableSetupColumn(T("col_players"),  ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn(T("col_installed"), ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("",                 ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableHeadersRow();
        // Sort by player count descending so the busiest rooms surface
        // first. Stable secondary sort by room name (alpha) so empty/quiet
        // rooms have a deterministic order between renders. Sort a copy so
        // we don't mutate hs.rooms (which the hub broadcast handler also
        // touches asynchronously — sorting in-place would race).
        std::vector<fm2k::HubRoom> sorted_rooms = hs.rooms;
        std::sort(sorted_rooms.begin(), sorted_rooms.end(),
            [](const fm2k::HubRoom& a, const fm2k::HubRoom& b) {
                if (a.user_count != b.user_count) return a.user_count > b.user_count;
                return a.name < b.name;
            });
        for (auto& r : sorted_rooms) {
            int installed_idx = FindInstalledGameForRoom(games_, r.id);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(r.name.c_str());
            ImGui::TableSetColumnIndex(1); ImGui::Text("%d", r.user_count);
            ImGui::TableSetColumnIndex(2);
            if (installed_idx >= 0) {
                ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.4f, 1.0f), "%s", T("label_yes"));
            } else {
                ImGui::TextColored(ImVec4(0.85f, 0.5f, 0.4f, 1.0f), "%s", T("label_no"));
            }
            ImGui::TableSetColumnIndex(3);
            ImGui::PushID(r.id.c_str());
            if (r.id == hs.current_room_id) {
                ImGui::TextColored(ImVec4(0.3f, 0.8f, 0.4f, 1.0f), "%s", T("label_joined"));
            } else if (ImGui::SmallButton(T("btn_join"))) {
                hs.client.JoinRoom(r.id, r.name);
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    // Quick-create room from the selected game (until a master list ships).
    bool game_selected = selected_game_index_ >= 0 &&
                         selected_game_index_ < (int)games_.size();
    if (game_selected && hs.current_room_id.empty()) {
        ImGui::Spacing();
        const auto& g = games_[selected_game_index_];
        // Use exe path stem as the room/game id so two clients with the
        // same exe land in the same room. Master list will replace this
        // with a stable canonical id.
        std::filesystem::path exe(g.exe_path);
        std::string game_id = exe.stem().string();
        std::string label = "Join room for: " + game_id;
        if (ImGui::Button(label.c_str(), ImVec2(-1, 0))) {
            hs.client.JoinRoom(game_id, game_id);
        }
    }

    // ---- Active Matches ----
    // Walk the user list, group in_match pairs (each pair appears once,
    // owned by the user with the lexicographically smaller id so we don't
    // double-render). Click "Spectate" to ask the hub for the host's UDP
    // addr; on grant a local FM2K spectator instance launches pointing at
    // it and joins the host's GekkoSpectateSession via SpectatorNode JOIN_REQ.
    if (!hs.users.empty()) {
        std::vector<std::pair<const fm2k::HubUser*, const fm2k::HubUser*>> active_pairs;
        for (auto& [uid, u] : hs.users) {
            if (u.status != "in_match") continue;
            if (u.opponent_id.empty())  continue;
            if (uid >= u.opponent_id)   continue;  // dedupe — only the lower-id half
            auto it = hs.users.find(u.opponent_id);
            if (it == hs.users.end())   continue;
            if (it->second.status != "in_match") continue;
            active_pairs.emplace_back(&u, &it->second);
        }

        ImGui::SeparatorText(T("hub_active_matches_header"));
        if (active_pairs.empty()) {
            ImGui::TextDisabled("%s", T("hub_no_active_matches"));
        } else if (ImGui::BeginTable("##active_matches", 3,
                       ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
            ImGui::TableSetupColumn(T("col_match"));
            ImGui::TableSetupColumn(T("col_room"), ImGuiTableColumnFlags_WidthFixed, 110.0f);
            ImGui::TableSetupColumn("",            ImGuiTableColumnFlags_WidthFixed, 100.0f);
            ImGui::TableHeadersRow();

            for (auto& [a, b] : active_pairs) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text(T("hub_active_match"), a->nick.c_str(), b->nick.c_str());

                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(a->room_id.empty() ? T("label_dash") : a->room_id.c_str());

                ImGui::TableSetColumnIndex(2);
                ImGui::PushID(a->id.c_str());
                // Spectate is currently broken (sim-determinism leak under
                // active investigation — host vs spectator diverge after
                // ~bf=8000 with ALL inputs/RNG/scripts identical, only one
                // pos field drifts; root-cause is a hook that touches
                // sim state on host but not on spectator). Show the
                // button greyed so users see the feature is intentional
                // but disabled, not missing.
                ImGui::BeginDisabled(true);
                ImGui::SmallButton(T("btn_spectate"));
                ImGui::EndDisabled();
                ImGui::SetItemTooltip("%s", T("btn_spectate_disabled_tooltip"));
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }

    // ---- Users in current room ----
    // Build a localized "Players in <room>" header with snprintf so the
    // translation string can position the room name wherever the language
    // wants it (English: "Players in %s", JP: "%s のプレイヤー").
    char players_header[160];
    if (hs.current_room_id.empty()) {
        std::snprintf(players_header, sizeof(players_header), "%s",
                      T("hub_players_header"));
    } else {
        std::snprintf(players_header, sizeof(players_header),
                      T("hub_players_in_room"), hs.current_room_id.c_str());
    }
    ImGui::SeparatorText(players_header);
    if (hs.current_room_id.empty()) {
        ImGui::TextDisabled("%s", T("hub_no_room_selected"));
    } else if (hs.users.empty()) {
        ImGui::TextDisabled("%s", T("hub_room_empty"));
    } else {
        if (ImGui::BeginTable("##users", 4,
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
            ImGui::TableSetupColumn(T("col_nick"));
            ImGui::TableSetupColumn(T("col_status"), ImGuiTableColumnFlags_WidthFixed, 110.0f);
            ImGui::TableSetupColumn(T("col_ping"),   ImGuiTableColumnFlags_WidthFixed, 60.0f);
            ImGui::TableSetupColumn("",              ImGuiTableColumnFlags_WidthFixed, 100.0f);
            ImGui::TableHeadersRow();

            // Tier → color mapping. Tester ($5) gets blue (0x2C7BDB,
            // matching Patreon's hub branding); Special Thanks ($10) gets
            // gold (0xFFBF03); monte (operator) gets red (0xE53935, Material
            // red 600 — distinct from gold without being garish). Anything
            // else (legacy hub, missing field) renders in the default text
            // color so stale clients don't turn invisible.
            const ImVec4 kTierTester(0x2C / 255.0f, 0x7B / 255.0f, 0xDB / 255.0f, 1.0f);
            const ImVec4 kTierThanks(0xFF / 255.0f, 0xBF / 255.0f, 0x03 / 255.0f, 1.0f);
            const ImVec4 kTierMonte (0xE5 / 255.0f, 0x39 / 255.0f, 0x35 / 255.0f, 1.0f);
            for (auto& [uid, u] : hs.users) {
                // Self is shown in the list (top row, naturally — most
                // hubs put your row at the top so you can see your own
                // tier color + status without scrolling). The Challenge
                // button is hidden for your own row below since
                // self-challenges are nonsensical.
                const bool is_self = (uid == hs.my_id);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                if (u.tier == "monte") {
                    ImGui::TextColored(kTierMonte, "%s", u.nick.c_str());
                } else if (u.tier == "thanks") {
                    ImGui::TextColored(kTierThanks, "%s", u.nick.c_str());
                } else if (u.tier == "tester") {
                    ImGui::TextColored(kTierTester, "%s", u.nick.c_str());
                } else {
                    ImGui::TextUnformatted(u.nick.c_str());
                }

                ImGui::TableSetColumnIndex(1);
                ImVec4 c(0.6f, 0.6f, 0.6f, 1.0f);
                // Localize status label too. The protocol value (u.status)
                // stays untranslated — that's an internal protocol token,
                // not user-facing text. Map it to a translation key.
                const char* status_label = u.status.c_str();
                if (u.status == "idle")             { c = ImVec4(0.3f, 0.9f, 0.4f, 1.0f); status_label = T("status_idle"); }
                else if (u.status == "in_match")    { c = ImVec4(0.95f, 0.7f, 0.2f, 1.0f); status_label = T("status_in_match"); }
                else if (u.status == "challenging") { c = ImVec4(0.6f, 0.7f, 1.0f, 1.0f); status_label = T("status_challenging"); }
                ImGui::TextColored(c, "%s", status_label);

                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%dms", u.rtt_ms);

                ImGui::TableSetColumnIndex(3);
                ImGui::PushID(uid.c_str());
                // Self-row shows nothing in the action column — challenging
                // yourself isn't a thing. Other rows get the Challenge button
                // gated on idle status.
                if (!is_self) {
                    bool can_challenge = (u.status == "idle");
                    if (!can_challenge) ImGui::BeginDisabled();
                    if (ImGui::SmallButton(T("btn_challenge"))) {
                        hs.client.Challenge(uid);
                        // Populate outbound state so the next frame
                        // renders the "Waiting for X..." modal. Cleared
                        // by the hub-event handler on any outcome.
                        hs.outgoing_challenge_to_id   = uid;
                        hs.outgoing_challenge_to_nick = u.nick;
                        hs.show_outgoing_challenge_modal = true;
                        hs.status_line = "challenged " + u.nick + " — waiting for response";
                    }
                    if (!can_challenge) ImGui::EndDisabled();
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }

    // ---- Incoming-challenge modal ----
    // Stable `##incoming_challenge` popup ID so a language switch mid-popup
    // doesn't break ImGui's hashed identity (see RenderConnectionStatus).
    if (hs.show_challenge_modal) {
        ImGui::OpenPopup("##incoming_challenge");
        hs.show_challenge_modal = false;
    }
    if (ImGui::BeginPopupModal("##incoming_challenge", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text(T("modal_incoming_challenge_body"), hs.pending_challenge_from_nick.c_str());
        ImGui::Spacing();
        if (ImGui::Button(T("btn_accept"), ImVec2(120, 0))) {
            hs.client.AcceptChallenge(hs.pending_challenge_from_id);
            hs.pending_challenge_from_id.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(T("btn_decline"), ImVec2(120, 0))) {
            hs.client.DeclineChallenge(hs.pending_challenge_from_id);
            hs.pending_challenge_from_id.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // ---- Outgoing-challenge modal ----
    // Renders for the challenger after they click Challenge so they
    // get visible feedback that the request went out. The hub-event
    // handler clears show_outgoing_challenge_modal on any terminal
    // outcome (declined / failed / cancelled / match_start).
    if (hs.show_outgoing_challenge_modal) {
        ImGui::OpenPopup("##outgoing_challenge");
    }
    if (ImGui::BeginPopupModal("##outgoing_challenge", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::Text(T("modal_outgoing_challenge_body"),
                    hs.outgoing_challenge_to_nick.empty()
                        ? "opponent"
                        : hs.outgoing_challenge_to_nick.c_str());
        // Animated pip so the user sees the modal is alive (ImGui's
        // own spinner widget doesn't exist; a string of dots cycling
        // is enough for the "we're waiting" signal).
        const int dots = (int)(ImGui::GetTime() * 2.0) % 4;
        char dot_str[5] = {0};
        for (int i = 0; i < dots; ++i) dot_str[i] = '.';
        ImGui::SameLine();
        ImGui::TextDisabled("%s", dot_str);
        ImGui::Spacing();
        if (ImGui::Button(T("btn_cancel"), ImVec2(120, 0))) {
            hs.client.CancelChallenge(hs.outgoing_challenge_to_id);
            hs.show_outgoing_challenge_modal = false;
            hs.outgoing_challenge_to_id.clear();
            hs.outgoing_challenge_to_nick.clear();
            hs.status_line = "challenge cancelled";
            ImGui::CloseCurrentPopup();
        }
        // Auto-close popup if the event handler dropped the flag (e.g.
        // we got match_start). BeginPopupModal returns true only while
        // the popup is open, so just stop reopening it: the next frame
        // sees show_outgoing_challenge_modal=false and skips OpenPopup.
        if (!hs.show_outgoing_challenge_modal) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void LauncherUI::RenderDebugTools() {
    if (ImGui::BeginTabBar("DebugTabs", ImGuiTabBarFlags_None)) {

        if (ImGui::BeginTabItem("Multi-Client")) {
            RenderMultiClientTools();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Network")) {
            RenderNetworkTools();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Log")) {
            RenderConsoleLog();
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }
}

void LauncherUI::RenderConsoleLog() {
    SDL_LockMutex(log_buffer_mutex_);

    if (ImGui::Button(T("btn_clear"))) {
        ClearLog();
    }

    ImGui::Separator();

    ImGui::BeginChild("LogScrollingRegion", ImVec2(0, -1), false, ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::InputTextMultiline("##console", (char*)log_buffer_.c_str(), log_buffer_.size(),
        ImVec2(-FLT_MIN, -FLT_MIN), ImGuiInputTextFlags_ReadOnly);

    if (scroll_to_bottom_) {
        ImGui::SetScrollHereY(1.0f);
        scroll_to_bottom_ = false;
    }

    ImGui::EndChild();
    SDL_UnlockMutex(log_buffer_mutex_);
}

void LauncherUI::RenderMultiClientTools() {
    ImGui::Text("%s", T("dev_local_multi"));
    ImGui::Separator();

    // Client Launch Controls
    if (ImGui::CollapsingHeader(T("panel_launch_control"), ImGuiTreeNodeFlags_DefaultOpen)) {
        // Get client status
        uint32_t client1_pid = 0, client2_pid = 0;
        bool status_available = false;
        if (on_get_client_status) {
            status_available = on_get_client_status(client1_pid, client2_pid);
        }
        
        // Selected game display
        if (!games_.empty() && selected_game_index_ >= 0 && selected_game_index_ < (int)games_.size()) {
            const auto& selected_game = games_[selected_game_index_];
            ImGui::Text(T("label_selected_game"), selected_game.GetExeName().c_str());
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), T("label_path"), selected_game.exe_path.c_str());
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f), "%s", T("warn_no_game_selected"));
        }
        
        ImGui::Separator();
        
        // Launch buttons
        bool can_launch = !games_.empty() && selected_game_index_ >= 0 && selected_game_index_ < (int)games_.size();
        bool clients_running = (client1_pid != 0 || client2_pid != 0);
        
        if (!can_launch) {
            ImGui::BeginDisabled();
        }
        
        // Dual client launch button
        if (ImGui::Button(T("dev_launch_dual_short"), ImVec2(200, 30))) {
            if (on_launch_local_client1 && on_launch_local_client2 && can_launch) {
                const auto& selected_game = games_[selected_game_index_];

                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Launching dual clients for: %s", selected_game.exe_path.c_str());

                // Both clients launch from the same folder — multi-instance
                // window check is patched and shared memory is PID-namespaced.
                bool success1 = on_launch_local_client1(selected_game.exe_path);
                if (success1) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Client 1 launched, starting client 2...");
                    bool success2 = on_launch_local_client2(selected_game.exe_path);
                    if (success2) {
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Client 2 (Guest) launched successfully");
                    } else {
                        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to launch Client 2 (Guest)");
                    }
                } else {
                    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to launch Client 1 (Host)");
                }
            }
        }
        
        ImGui::SameLine();
        
        // Stop all clients button
        if (!clients_running) {
            ImGui::BeginDisabled();
        }
        
        if (ImGui::Button(T("btn_stop_all_clients"), ImVec2(150, 30))) {
            if (on_terminate_all_clients) {
                bool success = on_terminate_all_clients();
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Terminate all clients: %s", success ? "success" : "failed");
            }
        }

        if (!clients_running) {
            ImGui::EndDisabled();
        }

        // "Launch Spectator" — third local instance subscribing to client1
        // (host on 7000) for replay-streamed spectator validation. Only
        // enabled once Launch Dual Clients has the host alive.
        ImGui::SameLine();
        bool can_spectate2 = on_launch_local_spectator && can_launch && client1_pid != 0;
        if (!can_spectate2) ImGui::BeginDisabled();
        if (ImGui::Button(T("btn_launch_spectator_short"), ImVec2(160, 30))) {
            if (can_spectate2) {
                const auto& selected_game = games_[selected_game_index_];
                bool ok = on_launch_local_spectator(selected_game.exe_path);
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Launch spectator: %s", ok ? "success" : "failed");
            }
        }
        if (!can_spectate2) ImGui::EndDisabled();
        ImGui::SetItemTooltip("%s", T("btn_launch_spectator_short_tip"));
        
        if (!can_launch) {
            ImGui::EndDisabled();
        }
        
        ImGui::Separator();
        
        // Client status display
        ImGui::Text("%s", T("label_client_status"));

        if (status_available) {
            // Client 1 status
            if (client1_pid != 0) {
                ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "%s", T("client1_online_host"));
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), T("label_pid"), client1_pid);
            } else {
                ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1.0f), "%s", T("client1_offline"));
            }
            
            // Client 2 status
            if (client2_pid != 0) {
                ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "%s", T("client2_online_guest"));
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), T("label_pid"), client2_pid);
            } else {
                ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1.0f), "%s", T("client2_offline"));
            }

            // Connection status
            if (client1_pid != 0 && client2_pid != 0) {
                ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "%s", T("network_status_connected"));
            } else if (client1_pid != 0 || client2_pid != 0) {
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "%s", T("network_status_waiting"));
            } else {
                ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1.0f), "%s", T("network_status_no_clients"));
            }
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f), "%s", T("client_status_unavailable"));
        }
    }

    ImGui::Separator();

    // Client Debug Logs
    if (ImGui::CollapsingHeader(T("debug_log_panel"), ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("%s", T("debug_log_realtime"));
        ImGui::Separator();

        // Client 1 Log Section
        ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "%s", T("client1_log_host"));
        ImGui::SameLine();
        if (ImGui::Button(T("debug_log_copy_c1"))) {
            // Read Client 1 log file and copy to clipboard
            std::ifstream log_file("FM2K_P1_Debug.log");
            if (log_file.is_open()) {
                std::stringstream buffer;
                buffer << log_file.rdbuf();
                log_file.close();
                
                std::string log_content = buffer.str();
                if (!log_content.empty()) {
                    SDL_SetClipboardText(log_content.c_str());
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Client 1 log copied to clipboard (%zu characters)", log_content.length());
                } else {
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Client 1 log file is empty");
                }
            } else {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Client 1 log file not found");
            }
        }
        
        // Display last few lines of Client 1 log
        {
            std::ifstream log_file("FM2K_P1_Debug.log");
            if (log_file.is_open()) {
                std::vector<std::string> lines;
                std::string line;
                while (std::getline(log_file, line)) {
                    lines.push_back(line);
                }
                log_file.close();
                
                // Show last 10 lines
                size_t start_idx = lines.size() > 10 ? lines.size() - 10 : 0;
                
                ImGui::BeginChild("Client1Log", ImVec2(0, 120), true, ImGuiWindowFlags_HorizontalScrollbar);
                for (size_t i = start_idx; i < lines.size(); ++i) {
                    // Color-code different log levels
                    if (lines[i].find("ERROR") != std::string::npos) {
                        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", lines[i].c_str());
                    } else if (lines[i].find("WARN") != std::string::npos) {
                        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "%s", lines[i].c_str());
                    } else if (lines[i].find("INPUT") != std::string::npos) {
                        ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.8f, 1.0f), "%s", lines[i].c_str());
                    } else {
                        ImGui::Text("%s", lines[i].c_str());
                    }
                }
                
                // Auto-scroll to bottom
                if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
                    ImGui::SetScrollHereY(1.0f);
                }
                ImGui::EndChild();
            } else {
                ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "%s", T("debug_log_no_file"));
            }
        }
        
        ImGui::Separator();
        
        // Client 2 Log Section
        ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.0f, 1.0f), "%s", T("client2_log_guest"));
        ImGui::SameLine();
        if (ImGui::Button(T("debug_log_copy_c2"))) {
            // Read Client 2 log file and copy to clipboard
            std::ifstream log_file("FM2K_P2_Debug.log");
            if (log_file.is_open()) {
                std::stringstream buffer;
                buffer << log_file.rdbuf();
                log_file.close();
                
                std::string log_content = buffer.str();
                if (!log_content.empty()) {
                    SDL_SetClipboardText(log_content.c_str());
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Client 2 log copied to clipboard (%zu characters)", log_content.length());
                } else {
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Client 2 log file is empty");
                }
            } else {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Client 2 log file not found");
            }
        }
        
        // Display last few lines of Client 2 log
        {
            std::ifstream log_file("FM2K_P2_Debug.log");
            if (log_file.is_open()) {
                std::vector<std::string> lines;
                std::string line;
                while (std::getline(log_file, line)) {
                    lines.push_back(line);
                }
                log_file.close();
                
                // Show last 10 lines
                size_t start_idx = lines.size() > 10 ? lines.size() - 10 : 0;
                
                ImGui::BeginChild("Client2Log", ImVec2(0, 120), true, ImGuiWindowFlags_HorizontalScrollbar);
                for (size_t i = start_idx; i < lines.size(); ++i) {
                    // Color-code different log levels
                    if (lines[i].find("ERROR") != std::string::npos) {
                        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", lines[i].c_str());
                    } else if (lines[i].find("WARN") != std::string::npos) {
                        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "%s", lines[i].c_str());
                    } else if (lines[i].find("INPUT") != std::string::npos) {
                        ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.8f, 1.0f), "%s", lines[i].c_str());
                    } else {
                        ImGui::Text("%s", lines[i].c_str());
                    }
                }
                
                // Auto-scroll to bottom
                if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
                    ImGui::SetScrollHereY(1.0f);
                }
                ImGui::EndChild();
            } else {
                ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "%s", T("debug_log_no_file"));
            }
        }
        
        ImGui::Separator();
        
        // Log Management
        ImGui::Text("%s", T("debug_log_management"));
        if (ImGui::Button(T("debug_log_clear_all"))) {
            // Clear both log files
            std::ofstream("FM2K_P1_Debug.log", std::ios::trunc).close();
            std::ofstream("FM2K_P2_Debug.log", std::ios::trunc).close();
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "All debug logs cleared");
        }

        ImGui::SameLine();
        if (ImGui::Button(T("debug_log_open_dir"))) {
            // Open the current directory in file explorer
            system("explorer .");
        }
        
    }
}

void LauncherUI::RenderNetworkTools() {
    RollbackStats stats = {};
    bool stats_available = false;

    if (on_get_rollback_stats) {
        stats_available = on_get_rollback_stats(stats);
    }

    if (!stats_available) {
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "%s", T("status_no_active_session"));
        return;
    }

    ImGui::Text(T("label_frame"), stats.confirmed_frames);
    ImGui::Text(T("label_rollbacks"), stats.speculative_frames);
    ImGui::Text(T("label_frame_advantage"), stats.frame_advantage);

    if (stats.speculative_frames == 0) {
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "%s", T("status_no_desyncs"));
    }
}

// RenderPerformanceStats removed - was just static info

// Custom log capture implementation
void LauncherUI::SDLCustomLogOutput(void* userdata, int category, SDL_LogPriority priority, const char* message) {
    LauncherUI* ui = static_cast<LauncherUI*>(userdata);
    if (!ui) {
        return;
    }

    // Chain to the original logger to keep console output
    if (ui->original_log_function_) {
        ui->original_log_function_(ui->original_log_userdata_, category, priority, message);
    }
    
    // Add to our internal buffer for the UI
    ui->AddLog(message);
}

void LauncherUI::AddLog(const char* message) {
    if (!log_buffer_mutex_) {
        return;
    }

    SDL_LockMutex(log_buffer_mutex_);
    
    log_buffer_.appendf("%s\n", message);
    scroll_to_bottom_ = true;

    SDL_UnlockMutex(log_buffer_mutex_);
}

void LauncherUI::ClearLog() {
    if (!log_buffer_mutex_) {
        return;
    }

    SDL_LockMutex(log_buffer_mutex_);
    log_buffer_.clear();
    SDL_UnlockMutex(log_buffer_mutex_);
}

void LauncherUI::RenderObjectAnalysis() {}
void LauncherUI::RenderSlotInspectionWindow() {}