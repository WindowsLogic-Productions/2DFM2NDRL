#include "FM2K_Integration.h"
#include "FM2K_HubClient.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <wininet.h>
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
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Select Games Folder...")) {
                // ... folder selection logic ...
            }
            if (ImGui::MenuItem("Exit", "Alt+F4")) {
                if (on_exit) on_exit();
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Session")) {
            if (launcher_state_ == LauncherState::InGame || launcher_state_ == LauncherState::Connecting) {
                if (ImGui::MenuItem("Disconnect")) {
                    if (on_session_stop) on_session_stop();
                }
            } else {
                ImGui::MenuItem("Disconnect", nullptr, false, false); // Disabled
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View")) {
            if (ImGui::MenuItem("Developer Mode", nullptr, developer_mode_)) {
                developer_mode_ = !developer_mode_;
            }
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }
}

void LauncherUI::RenderGameSelection() {
    ImGui::Text("Games Folder");
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
    if (ImGui::Button("Set") || (path_changed && ImGui::IsKeyPressed(ImGuiKey_Enter))) {
        if (on_games_folder_set) {
            // Update our internal path to match what user typed
            games_root_path_ = path_buf;
            on_games_folder_set(path_buf);
        }
    }
    ImGui::PopID();
    
    ImGui::Separator();
    ImGui::Text("Available FM2K Games");
    ImGui::Separator();

    if (scanning_games_) {
        ImGui::Text("Scanning for games...");
    } else if (games_.empty()) {
        ImGui::Text("No games found in the specified directory.");
        ImGui::Text("Please select a valid games folder.");
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
    if (ImGui::CollapsingHeader("Network Configuration", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Indent();

        // Session Type (Host/Join)
        static int session_type = 0; // 0: Host, 1: Join
        ImGui::RadioButton("Host", &session_type, 0); ImGui::SameLine();
        ImGui::RadioButton("Join", &session_type, 1);

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

            ImGui::Text("Your address:");
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "%s", address_with_port);
            ImGui::SameLine();
            if (ImGui::Button("Copy")) {
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
            if (ImGui::Button("Paste")) {
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
    if (launcher_state_ == LauncherState::Connecting) {
        ImGui::OpenPopup("Connecting...");
    }

    if (ImGui::BeginPopupModal("Connecting...", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Establishing connection, please wait...");
        if (launcher_state_ != LauncherState::Connecting) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    
    if (launcher_state_ == LauncherState::Disconnected) {
        ImGui::OpenPopup("Disconnected");
    }

    if (ImGui::BeginPopupModal("Disconnected", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("The network connection was lost.");
        if (ImGui::Button("OK")) {
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
    if (ImGui::CollapsingHeader("Session Controls", ImGuiTreeNodeFlags_DefaultOpen)) {
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

        // ---------- USER-FACING SECTION ----------
        ImGui::Text("Play:");
        ImGui::Separator();

        if (ImGui::Button("Online (Hub)", ImVec2(-1, 0))) {
            // Hub flow not yet wired to backend — placeholder routes
            // through Online Session for now until HubClient lands.
            if (on_online_session_start) {
                on_online_session_start(network_config_);
            }
        }
        ImGui::SetItemTooltip("Connect to the FM2K hub to find players (placeholder — opens room list).");

        if (ImGui::Button("Offline (Single Player)", ImVec2(-1, 0))) {
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
            if (on_offline_session_start) {
                on_offline_session_start();
            }
        }
        ImGui::SetItemTooltip("Local-only — both players controlled at the same machine.");

        // ---------- DEVELOPER SECTION ----------
        if (developer_mode_) {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "Developer mode");
            ImGui::Separator();

            ImGui::Text("Boot strategy:");
            ImGui::RadioButton("Safe — boot to title, auto-mash to CSS (universal)", &s_boot_strategy, 0);
            ImGui::SetItemTooltip(
                "Boots to title_screen_manager (skips intro cutscene). The "
                "hook auto-mashes button A with cursor pre-set to VS Player. "
                "Works on every game — adds ~10 frames to boot.");
            ImGui::RadioButton("Fast — boot directly to CSS (verified games only)", &s_boot_strategy, 1);
            ImGui::SetItemTooltip(
                "Skips title screen entirely. WORKS on WW. BREAKS StudioS "
                "Fighters / Strip Fighter Zero — characters self-damage on "
                "frame 0. Only enable per-game once verified safe.");

            ImGui::Checkbox("Auto-mash title → CSS", &s_auto_title_skip);
            ImGui::SetItemTooltip(
                "Default ON. Disable to walk title screen manually with your "
                "own inputs.");

            ImGui::Spacing();
            ImGui::Text("Diagnostics:");
            ImGui::Checkbox("Bypass trampoline (vanilla main_game_loop)", &s_bypass_trampoline);
            ImGui::SetItemTooltip(
                "Routes Hook_RunGameLoop to vanilla. Other hooks still fire. "
                "Offline only — netplay/spectator require the trampoline.");

            ImGui::Checkbox("Skip VS-player-mode force-set", &s_skip_vs_mode_patch);
            ImGui::SetItemTooltip("Don't force g_game_mode_flag=1 at boot.");

            ImGui::Checkbox("Force t4-walk patch (masks the real bug)", &s_force_t4_patch);
            ImGui::SetItemTooltip(
                "Re-enables the case-200 t4-walk neuter patch (0x408EC5).");

            ImGui::Checkbox("T4 probe (log fighter pool conditions)", &s_t4_probe);
            ImGui::SetItemTooltip("Pre-update pool walk; logs when count<2.");

            ImGui::Spacing();
            if (ImGui::Button("Online Session (legacy)", ImVec2(-1, 0))) {
                if (on_online_session_start) {
                    on_online_session_start(network_config_);
                }
            }
            ImGui::SetItemTooltip("Network play using the network-config panel below (pre-hub direct P2P).");

            if (ImGui::Button("Stress Test (Determinism Check)", ImVec2(-1, 0))) {
                if (on_stress_session_start) {
                    on_stress_session_start();
                }
            }
            ImGui::SetItemTooltip(
                "GekkoStressSession with a single instance. Forces rollback every 10 frames "
                "and compares save hashes — any DESYNC = local determinism bug.");

            ImGui::Spacing();
            ImGui::Text("Local Testing:");
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
        
        if (ImGui::Button("Launch Dual Clients (Localhost)", ImVec2(-1, 0))) {
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

        ImGui::SetItemTooltip("Launch two separate game instances connected via localhost for network testing");

        // "Launch Spectator" — spawns a third local instance that subscribes
        // to client1 (host on 7000) for replay-streamed spectating. Only
        // makes sense after Launch Dual Clients has the host running.
        bool can_spectate = on_launch_local_spectator && game_selected && client1_pid != 0;
        if (!can_spectate) ImGui::BeginDisabled();
        if (ImGui::Button("Launch Spectator (subscribes to host)", ImVec2(-1, 0))) {
            if (can_spectate) {
                const auto& selected_game = games_[selected_game_index_];
                bool ok = on_launch_local_spectator(selected_game.exe_path);
                if (!ok) {
                    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to launch spectator");
                }
            }
        }
        if (!can_spectate) ImGui::EndDisabled();
        ImGui::SetItemTooltip("Launch a third local instance that subscribes to client1 (host on port 7000) and replays the input stream");

        // "Launch Spectator 2 (chain)" — daisy-chain test. Subscribes to
        // spectator 1 (port 7002) instead of the host. Validates that
        // spectator 1 correctly relays its received frames to its own
        // subscribers. Disabled until both dual clients + spectator 1 are
        // running.
        bool can_spectate2 = on_launch_local_spectator2 && game_selected && client1_pid != 0;
        if (!can_spectate2) ImGui::BeginDisabled();
        if (ImGui::Button("Launch Spectator 2 (chain to spec 1)", ImVec2(-1, 0))) {
            if (can_spectate2) {
                const auto& selected_game = games_[selected_game_index_];
                bool ok = on_launch_local_spectator2(selected_game.exe_path);
                if (!ok) {
                    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to launch spectator 2");
                }
            }
        }
        if (!can_spectate2) ImGui::EndDisabled();
        ImGui::SetItemTooltip("Launch a fourth local instance bound on 7003 that subscribes to spectator 1 on 7002 (daisy-chain validation). Spectator 1 must be running.");

        if (clients_running) {
            ImGui::Text("Clients running (PID: %u, %u)", client1_pid, client2_pid);
            if (ImGui::Button("Terminate All Clients", ImVec2(-1, 0))) {
                if (on_terminate_all_clients) {
                    on_terminate_all_clients();
                }
            }
        }
        }  // end if (developer_mode_)

        ImGui::Spacing();
        ImGui::Separator();

        if (ImGui::Button("Stop Session", ImVec2(-1, 0))) {
            if (on_session_stop) {
                on_session_stop();
            }
        }
        ImGui::SetItemTooltip("Terminate the currently running game session");

        if (!game_selected) {
            ImGui::EndDisabled();
        }

        ImGui::Unindent();
    }
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
                break;
            case K::ChallengeFailed:
                hs.status_line = "challenge failed: " + ev.error;
                break;
            case K::ChallengeCancelled:
                hs.show_challenge_modal = false;
                hs.pending_challenge_from_id.clear();
                hs.status_line = "challenge cancelled";
                break;
            case K::ChallengeDeclined:
                hs.status_line = "challenge declined";
                break;
            case K::MatchStart: {
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
                    // Make sure the launcher's selected_game_ record is
                    // up to date even if RoomJoined fired before games
                    // discovery completed.
                    if (on_game_selected) on_game_selected(games_[idx]);

                    // Plumb hub coordinates through the spawned game's
                    // env so FM2KHook's nat_traversal can fire a STUN
                    // probe and authenticated punch on Netplay_Init.
                    // Inherited via CreateProcess in
                    // FM2KGameInstance::Launch.
                    // TODO(settings): hub URL is hardcoded here as it
                    // is in HubClient — both move to a Settings panel
                    // together later.
                    ::SetEnvironmentVariableA("FM2K_HUB_UDP_ADDR",   "127.0.0.1:7711");
                    ::SetEnvironmentVariableA("FM2K_HUB_USER_ID",    hs.my_id.c_str());
                    ::SetEnvironmentVariableA("FM2K_HUB_MATCH_TOKEN", ev.match.token.c_str());

                    NetworkConfig cfg = network_config_;
                    cfg.session_mode = SessionMode::ONLINE;
                    cfg.is_host = (ev.match.role == "host");
                    cfg.remote_address =
                        ev.match.peer_udp_ip + ":" +
                        std::to_string(ev.match.peer_udp_port);
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
            case K::Error:
                hs.status_line = "error: " + ev.error;
                break;
        }
    });

    // ---- UI ----
    ImGui::SeparatorText("Hub");

    static char s_nick[32] = "";
    if (!hs.client.IsConnected()) {
        ImGui::PushItemWidth(-120);
        ImGui::InputText("Nick", s_nick, sizeof(s_nick));
        // Local UDP port — the socket the hook binds for game traffic.
        // Same-machine testing requires distinct ports per launcher,
        // otherwise the second one hits WSAEADDRINUSE on bind. Real
        // network play: any single port works since each box is its
        // own bind namespace.
        ImGui::InputInt("Port", &network_config_.local_port);
        if (network_config_.local_port < 1024)  network_config_.local_port = 7000;
        if (network_config_.local_port > 65535) network_config_.local_port = 7000;
        ImGui::PopItemWidth();
        const bool can_connect = s_nick[0] != '\0';
        if (!can_connect) ImGui::BeginDisabled();
        if (ImGui::Button(can_connect ? "Connect" : "(set a nick first)", ImVec2(-1, 0))) {
            hs.my_nick = s_nick;
            // TODO(settings): hub address belongs in a Settings panel.
            // Hardcoded to localhost for the demo.
            hs.client.Connect("127.0.0.1", 7711, "/", hs.my_nick);
            hs.status_line = "connecting...";
        }
        if (!can_connect) ImGui::EndDisabled();
    } else {
        ImGui::TextColored(ImVec4(0.3f, 0.8f, 0.4f, 1.0f),
                           "Connected as %s", hs.my_nick.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Disconnect")) {
            hs.client.Disconnect();
        }
    }
    if (!hs.status_line.empty()) {
        ImGui::TextDisabled("%s", hs.status_line.c_str());
    }
    if (!hs.client.IsConnected()) return;

    // ---- Rooms ----
    ImGui::SeparatorText("Rooms");
    if (hs.rooms.empty()) {
        ImGui::TextDisabled("No rooms yet — join one with the selected game below.");
    }
    if (ImGui::BeginTable("##rooms", 4,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("Game");
        ImGui::TableSetupColumn("Players",   ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn("Installed", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("",          ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableHeadersRow();
        for (auto& r : hs.rooms) {
            int installed_idx = FindInstalledGameForRoom(games_, r.id);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(r.name.c_str());
            ImGui::TableSetColumnIndex(1); ImGui::Text("%d", r.user_count);
            ImGui::TableSetColumnIndex(2);
            if (installed_idx >= 0) {
                ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.4f, 1.0f), "yes");
            } else {
                ImGui::TextColored(ImVec4(0.85f, 0.5f, 0.4f, 1.0f), "no");
            }
            ImGui::TableSetColumnIndex(3);
            ImGui::PushID(r.id.c_str());
            if (r.id == hs.current_room_id) {
                ImGui::TextColored(ImVec4(0.3f, 0.8f, 0.4f, 1.0f), "joined");
            } else if (ImGui::SmallButton("Join")) {
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

    // ---- Users in current room ----
    ImGui::SeparatorText(hs.current_room_id.empty()
        ? "Players in current room"
        : ("Players in " + hs.current_room_id).c_str());
    if (hs.current_room_id.empty()) {
        ImGui::TextDisabled("Join a room to see players.");
    } else if (hs.users.empty()) {
        ImGui::TextDisabled("Empty room — wait for someone else to join.");
    } else {
        if (ImGui::BeginTable("##users", 4,
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
            ImGui::TableSetupColumn("Nick");
            ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 110.0f);
            ImGui::TableSetupColumn("Ping",   ImGuiTableColumnFlags_WidthFixed, 60.0f);
            ImGui::TableSetupColumn("",       ImGuiTableColumnFlags_WidthFixed, 100.0f);
            ImGui::TableHeadersRow();

            for (auto& [uid, u] : hs.users) {
                if (uid == hs.my_id) continue;  // don't list self
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(u.nick.c_str());

                ImGui::TableSetColumnIndex(1);
                ImVec4 c(0.6f, 0.6f, 0.6f, 1.0f);
                if (u.status == "idle")        c = ImVec4(0.3f, 0.9f, 0.4f, 1.0f);
                else if (u.status == "in_match") c = ImVec4(0.95f, 0.7f, 0.2f, 1.0f);
                else if (u.status == "challenging") c = ImVec4(0.6f, 0.7f, 1.0f, 1.0f);
                ImGui::TextColored(c, "%s", u.status.c_str());

                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%dms", u.rtt_ms);

                ImGui::TableSetColumnIndex(3);
                ImGui::PushID(uid.c_str());
                bool can_challenge = (u.status == "idle");
                if (!can_challenge) ImGui::BeginDisabled();
                if (ImGui::SmallButton("Challenge")) {
                    hs.client.Challenge(uid);
                }
                if (!can_challenge) ImGui::EndDisabled();
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }

    // ---- Incoming-challenge modal ----
    if (hs.show_challenge_modal) {
        ImGui::OpenPopup("Incoming challenge");
        hs.show_challenge_modal = false;
    }
    if (ImGui::BeginPopupModal("Incoming challenge", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("%s wants to play.", hs.pending_challenge_from_nick.c_str());
        ImGui::Spacing();
        if (ImGui::Button("Accept", ImVec2(120, 0))) {
            hs.client.AcceptChallenge(hs.pending_challenge_from_id);
            hs.pending_challenge_from_id.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Decline", ImVec2(120, 0))) {
            hs.client.DeclineChallenge(hs.pending_challenge_from_id);
            hs.pending_challenge_from_id.clear();
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

    if (ImGui::Button("Clear")) {
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
    ImGui::Text("Local Multi-Client Testing");
    ImGui::Separator();
    
    // Client Launch Controls
    if (ImGui::CollapsingHeader("Launch Control", ImGuiTreeNodeFlags_DefaultOpen)) {
        // Get client status
        uint32_t client1_pid = 0, client2_pid = 0;
        bool status_available = false;
        if (on_get_client_status) {
            status_available = on_get_client_status(client1_pid, client2_pid);
        }
        
        // Selected game display
        if (!games_.empty() && selected_game_index_ >= 0 && selected_game_index_ < (int)games_.size()) {
            const auto& selected_game = games_[selected_game_index_];
            ImGui::Text("Selected Game: %s", selected_game.GetExeName().c_str());
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Path: %s", selected_game.exe_path.c_str());
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f), "⚠ No game selected");
        }
        
        ImGui::Separator();
        
        // Launch buttons
        bool can_launch = !games_.empty() && selected_game_index_ >= 0 && selected_game_index_ < (int)games_.size();
        bool clients_running = (client1_pid != 0 || client2_pid != 0);
        
        if (!can_launch) {
            ImGui::BeginDisabled();
        }
        
        // Dual client launch button
        if (ImGui::Button("Launch Dual Clients", ImVec2(200, 30))) {
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
        
        if (ImGui::Button("Stop All Clients", ImVec2(150, 30))) {
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
        if (ImGui::Button("Launch Spectator", ImVec2(160, 30))) {
            if (can_spectate2) {
                const auto& selected_game = games_[selected_game_index_];
                bool ok = on_launch_local_spectator(selected_game.exe_path);
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Launch spectator: %s", ok ? "success" : "failed");
            }
        }
        if (!can_spectate2) ImGui::EndDisabled();
        ImGui::SetItemTooltip("Spawn a third local instance bound on 7002 that subscribes to host on 7000 (replay-streamed spectating). Requires Launch Dual Clients first.");
        
        if (!can_launch) {
            ImGui::EndDisabled();
        }
        
        ImGui::Separator();
        
        // Client status display
        ImGui::Text("Client Status:");
        
        if (status_available) {
            // Client 1 status
            if (client1_pid != 0) {
                ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "● Client 1: Online (Host - 127.0.0.1:7000)");
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "(PID: %u)", client1_pid);
            } else {
                ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1.0f), "○ Client 1: Offline");
            }
            
            // Client 2 status
            if (client2_pid != 0) {
                ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "● Client 2: Online (Guest - 127.0.0.1:7001)");
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "(PID: %u)", client2_pid);
            } else {
                ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1.0f), "○ Client 2: Offline");
            }
            
            // Connection status
            if (client1_pid != 0 && client2_pid != 0) {
                ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "✓ Network Status: Connected");
            } else if (client1_pid != 0 || client2_pid != 0) {
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "⚠ Network Status: Waiting for second client");
            } else {
                ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1.0f), "○ Network Status: No clients running");
            }
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f), "⚠ Client status unavailable");
        }
    }
    
    ImGui::Separator();
    
    // Client Debug Logs
    if (ImGui::CollapsingHeader("Client Debug Logs", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("Real-time debug output from each client:");
        ImGui::Separator();
        
        // Client 1 Log Section
        ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "● Client 1 Log (Host)");
        ImGui::SameLine();
        if (ImGui::Button("Copy Client 1 Log")) {
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
                ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "No log file available (client not started)");
            }
        }
        
        ImGui::Separator();
        
        // Client 2 Log Section
        ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.0f, 1.0f), "● Client 2 Log (Guest)");
        ImGui::SameLine();
        if (ImGui::Button("Copy Client 2 Log")) {
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
                ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "No log file available (client not started)");
            }
        }
        
        ImGui::Separator();
        
        // Log Management
        ImGui::Text("Log Management:");
        if (ImGui::Button("Clear All Logs")) {
            // Clear both log files
            std::ofstream("FM2K_P1_Debug.log", std::ios::trunc).close();
            std::ofstream("FM2K_P2_Debug.log", std::ios::trunc).close();
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "All debug logs cleared");
        }
        
        ImGui::SameLine();
        if (ImGui::Button("Open Log Directory")) {
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
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "No active session");
        return;
    }

    ImGui::Text("Frame: %u", stats.confirmed_frames);
    ImGui::Text("Rollbacks: %u", stats.speculative_frames);
    ImGui::Text("Frame Advantage: %.1f", stats.frame_advantage);

    if (stats.speculative_frames == 0) {
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "No desyncs");
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