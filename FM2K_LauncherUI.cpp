#include "FM2K_Integration.h"
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
    // Initialize callbacks to null
    on_game_selected = nullptr;
    on_offline_session_start = nullptr;
    on_online_session_start = nullptr;
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
        RenderNetworkConfig();
        ImGui::Separator();
        RenderSessionControls();
    }
    ImGui::End();
    
    if (ImGui::Begin("Debug & Diagnostics", nullptr, panel_flags)) {
        RenderDebugTools();
    }
    ImGui::End();
    
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
        // View menu removed - using standard Dark theme only
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

        ImGui::Text("Single Player Sessions:");
        ImGui::Separator();
        
        if (ImGui::Button("True Offline (Local Only)", ImVec2(-1, 0))) {
            if (on_offline_session_start) {
                on_offline_session_start();
            }
        }
        ImGui::SetItemTooltip("Pure offline play - both players controlled locally, no networking");

        if (ImGui::Button("Online Session", ImVec2(-1, 0))) {
            if (on_online_session_start) {
                on_online_session_start(network_config_);
            }
        }
        ImGui::SetItemTooltip("Network play using the configuration below");

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
                
                // Create client 2 path by replacing directory name with "2" suffix
                std::filesystem::path original_path(selected_game.exe_path);
                std::filesystem::path original_dir = original_path.parent_path();
                std::filesystem::path exe_name = original_path.filename();
                
                std::string new_dir_name = original_dir.filename().string() + "2";
                std::filesystem::path parent_dir = original_dir.parent_path();
                std::filesystem::path client2_dir = parent_dir / new_dir_name;
                std::filesystem::path client2_path = client2_dir / exe_name;
                
                // Check if client2 directory exists
                if (!std::filesystem::exists(client2_dir)) {
                    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Client 2 directory not found: %s", client2_dir.string().c_str());
                    return;
                }
                
                // Launch both clients
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Starting dual clients...");
                bool success1 = on_launch_local_client1(selected_game.exe_path);
                
                if (success1) {
                    bool success2 = on_launch_local_client2(client2_path.string());
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
        
        if (clients_running) {
            ImGui::Text("Clients running (PID: %u, %u)", client1_pid, client2_pid);
            if (ImGui::Button("Terminate All Clients", ImVec2(-1, 0))) {
                if (on_terminate_all_clients) {
                    on_terminate_all_clients();
                }
            }
        }
        
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
                
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Launching dual clients for: %s", selected_game.exe_path.c_str());
                
                // Create client 2 path by replacing directory name with "2" suffix
                std::filesystem::path original_path(selected_game.exe_path);
                std::filesystem::path original_dir = original_path.parent_path();
                std::filesystem::path exe_name = original_path.filename();
                
                // Create new directory path: wanwan -> wanwan2
                std::string new_dir_name = original_dir.filename().string() + "2";
                std::filesystem::path parent_dir = original_dir.parent_path();
                std::filesystem::path client2_dir = parent_dir / new_dir_name;
                std::filesystem::path client2_path = client2_dir / exe_name;
                
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Client 1 path: %s", selected_game.exe_path.c_str());
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Client 2 path: %s", client2_path.string().c_str());
                
                // Verify client 2 path exists
                if (!std::filesystem::exists(client2_path)) {
                    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Client 2 executable not found: %s", client2_path.string().c_str());
                    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Please manually create %s directory with game files", client2_dir.string().c_str());
                    return;
                }
                
                // Launch both clients quickly (OnlineSession style)
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Starting client 1 (Host)...");
                bool success1 = on_launch_local_client1(selected_game.exe_path);  // wanwan/game.exe
                
                if (success1) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Client 1 launched, starting client 2...");
                    
                    // Launch client 2 from the "2" directory
                    bool success2 = on_launch_local_client2(client2_path.string());  // wanwan2/game.exe
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