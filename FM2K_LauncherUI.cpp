#include "FM2K_Integration.h"
#include "FM2K_HubClient.h"
#include "FM2K_DiscordAuth.h"
#include "FM2K_Locale.h"
#include "FM2K_Updater.h"
#include "version_local.h"
#include "FM2KHook/src/ui/input_binder.h"
#include "FM2KHook/src/ui/shared_mem.h"
#include "FM2K_GameIni.h"
#include "FM2K_DDrawRedirect.h"
#include "FM2K_CncDDraw.h"
#include "FM2K_Utf8Path.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <wininet.h>
#include <shellapi.h>  // Shell_NotifyIcon for challenge toast
#include <iostream>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <vector>
#include <array>
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
#include <unordered_set>

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
    // Snapshot of the room we were in at the moment the WS dropped.
    // Used by the Connected handler to auto-rejoin after a hub
    // restart / network blip; cleared once the rejoin call is sent.
    std::string last_room_id;
    std::string last_room_name;

    std::vector<fm2k::HubRoom> rooms;                     // discovered rooms
    std::unordered_map<std::string, fm2k::HubUser> users; // users in current room
    std::string pending_challenge_from_id;
    std::string pending_challenge_from_nick;
    // Match settings the challenger sent us (#54). Rendered in the
    // accept modal so the user sees what they're agreeing to. Sentinel
    // -1 across the struct = challenger didn't send any (older client).
    fm2k::MatchSettings pending_challenge_settings;
    // Hub-authoritative settings for the active hub-driven match. Set
    // on K::MatchStart from ev.match.settings; consumed by the launch
    // path (FM2KLauncher::StartOnlineSession via on_online_session_start
    // can't see this directly — we expose it via the existing
    // network_config_ piggyback below). Random-stage env vars are
    // derived from `random_seed`/`random_stage_*`.
    fm2k::MatchSettings current_match_settings;
    std::string status_line;
    bool show_challenge_modal = false;

    // Outbound challenge state — populated when WE click Challenge on
    // somebody, cleared when the hub tells us the outcome (declined,
    // cancelled, failed, or match_start). Drives the "Waiting for X..."
    // modal so the challenger gets feedback instead of a silent UI.
    std::string outgoing_challenge_to_id;
    std::string outgoing_challenge_to_nick;
    bool        show_outgoing_challenge_modal = false;

    // Hash-mismatch popup state. Set when the hook publishes a
    // FM2K_MATCH_OUTCOME_HASH_MISMATCH; cleared when the user
    // dismisses the modal. The log_excerpt is the most recent
    // "GameHash: manifest" block read from the spawned game's hook
    // log so the popup can show *what* hashed (per-file name|size|
    // content_hash) and the user knows which file to compare against
    // their peer.
    bool        show_hash_mismatch_modal = false;
    std::string hash_mismatch_log_excerpt;

    // Active hub-driven match. Set on K::MatchStart from ev.match.token,
    // cleared once we publish a match_result to the hub. Used by the
    // shared-mem poll path so a single bump of `match_outcome_seq` in
    // the hook turns into exactly one outbound match_result. Empty when
    // we're not in a hub match (offline / dev / spectator).
    std::string current_match_token;
    // Per-match peer + role snapshot — stashed on K::MatchStart so the
    // local results.csv writer (#42) can render the row from MY
    // perspective even after the hub modal / users-list state has moved
    // on. role is "host" (we're P1) or "guest" (we're P2); peer_nick
    // is the opponent's display name as the hub gave it to us.
    std::string current_match_role;
    std::string current_match_peer_id;
    std::string current_match_peer_nick;
    std::string current_match_game_id;
    // Per-process last-seen outcome seq, keyed by PID. The hook starts
    // seq at 0; first real outcome arrives as 1. We send match_result
    // when the value we read is greater than what's stored here, then
    // bump our copy to that value so subsequent identical reads (the
    // shared mem stays at the last value forever) don't re-send.
    std::unordered_map<uint32_t, uint32_t> last_outcome_seq;

    // My own overall W/L/D, populated by hub `record` events and used
    // for the launcher titlebar. (-1, -1, -1) means we haven't received
    // a record yet — render no titlebar suffix in that case.
    int my_wins   = -1;
    int my_losses = -1;
    int my_draws  = -1;
    // Per-opponent record from MY perspective ("how I've done vs them").
    // Keyed by opponent hub user_id; populated from the `vs_breakdown`
    // attached to the record event when we issue an unfiltered
    // QueryRecord. Empty until first record arrives or for opponents
    // we've never played.
    struct VsCell { int wins = 0, losses = 0, draws = 0; };
    std::unordered_map<std::string, VsCell> my_vs;
    // Recent matches (#49). Hub answers RequestRecentMatches with at
    // most N rows ordered newest-first. Mirrored here so the "Recent
    // Matches" window can render from cached state without re-hitting
    // the hub on every render.
    std::vector<fm2k::HubEvent::MatchRow> recent_matches;
    bool recent_matches_loaded = false;
    // Currently-in-flight matches (lobby panel). Snapshot from
    // RequestCurrentMatches on connect; live updates from
    // MatchInProgressStarted/Updated/Ended broadcasts. Sorted by
    // started_at ascending (newest at the bottom) on each render.
    std::vector<fm2k::HubEvent::MatchInProgress> current_matches;
    bool current_matches_loaded = false;
    // Per-PID last-seen match_chars_seq from the hook. The hook bumps
    // this counter exactly once per Netplay_StartBattleSession (after
    // chars + stage are published). Launcher fires match_progress only
    // when this counter advances, so during the inter-battle CSS
    // window — where shared mem still holds the prev battle's data —
    // no spurious match_progress is sent and the lobby's "(in CSS)"
    // row stays clean until the new battle actually starts.
    std::unordered_map<uint32_t, uint32_t> last_chars_seq;
    // True once the hook has published an outcome that resolved the
    // current match (any of self_won/peer_won/draw/disconnect). Reset
    // on the next K::MatchStart so back-to-back matches don't share a
    // stale flag.
    bool        match_result_sent = false;
    // De-dupe for peer-disconnected toasts. Three independent paths
    // can fire one for a single dropout: (a) the hook publishes a
    // DISCONNECT outcome via shared mem, (b) the hub sends a UserUpdate
    // moving the peer out of "in_match", (c) the hub sends a peer-
    // disconnected event when the peer's WS closes. Whichever lands
    // first sets this flag; the other two skip. Reset on MatchStart.
    bool        disconnect_toast_fired = false;
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

// Read the spawned game's hook log and extract the most recent
// "GameHash: manifest" block. Used by the hash-mismatch popup so the
// user sees their per-file inventory (filename | size | content_hash)
// without having to dig through the full hook log themselves.
//
// The hook log normally lives at <game_dir>/logs/FM2K_P{idx}_Debug.log.
// When the game is installed under C:\Program Files\ and the hook
// process isn't elevated, Windows VirtualStore redirects the write
// to %LOCALAPPDATA%\VirtualStore\<original_path>. We probe both
// locations and pick whichever has the freshest "manifest" content.
// Returns a friendly error string (still safe to display) if neither
// path contains a usable log.
static std::string ExtractGameHashManifest(const std::filesystem::path& exe_path,
                                           int player_index) {
    if (exe_path.empty()) return "(no game selected)";
    const std::string base =
        std::string("FM2K_P") + std::to_string(player_index + 1) + "_Debug.log";
    const std::filesystem::path canonical_log =
        exe_path.parent_path() / "logs" / base;

    auto read_log = [](const std::filesystem::path& p) -> std::string {
        FILE* f = _wfopen(p.wstring().c_str(), L"rb");
        if (!f) return {};
        std::vector<char> buf;
        char chunk[8192];
        while (size_t n = std::fread(chunk, 1, sizeof(chunk), f)) {
            buf.insert(buf.end(), chunk, chunk + n);
        }
        std::fclose(f);
        return std::string(buf.begin(), buf.end());
    };

    // VirtualStore mirror path: %LOCALAPPDATA%\VirtualStore\<rest>
    // where <rest> is the original full path WITHOUT the drive letter.
    // Win32 paints LocalAppData\VirtualStore over the install dir
    // for unprivileged writes to Program Files / Windows / etc.
    std::filesystem::path virtualstore_log;
    {
        wchar_t lad[MAX_PATH] = {0};
        if (GetEnvironmentVariableW(L"LOCALAPPDATA", lad, MAX_PATH) > 0) {
            // Strip drive prefix ("C:\foo\bar" -> "foo\bar"). On a
            // standard Windows install drive letter is always 2 chars
            // ("C:") followed by a separator; relative_path() drops both.
            std::filesystem::path rel = canonical_log.relative_path();
            // relative_path() of "C:\Program Files\..." gives "Program
            // Files\..." which is exactly what VirtualStore wants.
            virtualstore_log =
                std::filesystem::path(lad) / "VirtualStore" / rel;
        }
    }

    // Read both candidates; prefer the one that actually contains a
    // manifest marker (in case both exist but only one was written
    // this session — e.g. an old non-virtualized log lingering).
    std::string text = read_log(canonical_log);
    if (text.empty() ||
        (text.find("local manifest follows") == std::string::npos &&
         text.find("GameHash: manifest") == std::string::npos))
    {
        if (!virtualstore_log.empty()) {
            std::string vs = read_log(virtualstore_log);
            if (!vs.empty()) {
                text = std::move(vs);
            }
        }
    }
    if (text.empty()) {
        return std::string("(couldn't open ") +
               fm2k::utf8path::FilenameUtf8(canonical_log) +
               " from either the game folder or VirtualStore — "
               "is the launcher running with the wrong privileges?)";
    }

    // Find the LAST manifest dump. Two possible markers, in order of
    // preference:
    //   1. "local manifest follows"  — emitted by netplay.cpp on the
    //      actual hash-mismatch path. Always present when this popup
    //      fires from a current build, and includes the offending
    //      hashes both peers traded inline above the dump.
    //   2. "GameHash: manifest"      — emitted by game_hash.cpp once
    //      at boot, INFO level. Fallback for cases where we open the
    //      log on a session that didn't actually hit a mismatch yet,
    //      or older hook builds before the per-mismatch dump landed.
    size_t pos = text.rfind("local manifest follows");
    if (pos == std::string::npos) {
        pos = text.rfind("GameHash: manifest");
    }
    if (pos == std::string::npos) {
        return "(log present, but no manifest dump found — older hook "
               "build, or the log was rotated. Open the log in a text "
               "editor and search for 'manifest'.)";
    }
    // Walk back to start of the line.
    size_t line_start = pos;
    while (line_start > 0 && text[line_start - 1] != '\n') --line_start;
    // Take the next ~120 lines max.
    std::string out;
    size_t cursor = line_start;
    int lines = 0;
    while (cursor < text.size() && lines < 120) {
        size_t nl = text.find('\n', cursor);
        if (nl == std::string::npos) nl = text.size();
        out.append(text, cursor, nl - cursor);
        out.push_back('\n');
        cursor = nl + 1;
        ++lines;
    }
    return out;
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
        // Wide-construct so JP-named exes give us their actual UTF-8
        // bytes back from stem(), not CP1252 mangle. Otherwise rooms
        // never match games with non-ASCII filenames.
        std::filesystem::path exe(fm2k::utf8path::Utf8ToWide(games[i].exe_path));
        const std::string stem = lower(fm2k::utf8path::StemUtf8(exe));
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
    , games_root_paths_{}
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
    on_games_folders_set = nullptr;
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
        // Range: ASCII + Latin-1 Supplement (Armonté etc.) + Halfwidth
        // and Fullwidth Forms (FM2K games shipped by Japanese authors
        // commonly have full-width-titled exes like ＣＰＷ.exe). Segoe
        // UI / Tahoma / Arial all ship with the full-width block in
        // their CMAPs, so requesting it here pulls those glyphs into
        // the atlas without needing the MS Gothic merge to backfill.
        // Belt-and-suspenders: the JP merge below ALSO requests FF00-
        // FFEF, but ImGui's packed accumulator decompression has been
        // observed to drop ranges silently — claiming the block from
        // the Latin font directly is the reliable path.
        static const ImWchar latin_range[] = {
            0x0020, 0x00FF,   // Basic Latin + Latin-1 Supplement
            0xFF00, 0xFFEF,   // Halfwidth + Fullwidth Forms
            0,
        };
        ImFont* latin_font = nullptr;
        for (const char* p : latin_font_paths) {
            latin_font = io.Fonts->AddFontFromFileTTF(p, 16.0f, &latin_config,
                                                     latin_range);
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
        const char* jp_loaded_path = nullptr;
        for (const char* p : jp_font_paths) {
            if (io.Fonts->AddFontFromFileTTF(p, 16.0f, &jp_config,
                                             io.Fonts->GetGlyphRangesJapanese())) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Merged Japanese font: %s", p);
                jp_loaded = true;
                jp_loaded_path = p;
                break;
            }
        }
        // Belt-and-suspenders: explicitly request the Halfwidth and
        // Fullwidth Forms block (U+FF00-FFEF) from the same JP font.
        // GetGlyphRangesJapanese() *should* include this range, but the
        // packed accumulator decompression has historically dropped it
        // on some ImGui revisions. Game directories commonly contain
        // full-width-titled exes (e.g. ＣＰＷ.exe), so missing glyphs
        // here render as visible underscores in the Settings panel —
        // very specifically the bug we just hit. Adding the range
        // again with MergeMode=true is a no-op when it was already
        // included; otherwise it backfills the missing glyphs.
        if (jp_loaded && jp_loaded_path) {
            static const ImWchar fullwidth_range[] = {
                0xFF00, 0xFFEF,   // Halfwidth + Fullwidth Forms
                0,
            };
            io.Fonts->AddFontFromFileTTF(jp_loaded_path, 16.0f, &jp_config,
                                         fullwidth_range);
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
    fm2k::cnc_ddraw::Shutdown();

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

    // Drain any new outcome the hook published into a hub match_result.
    // Lives at the top of Render so it fires regardless of whether the
    // user has the Hub panel docked-visible. Idempotent — second call
    // for the same seq is a no-op.
    PollMatchOutcome();

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

                // Two-pane default — narrow left rail for Games &
                // Configuration + Debug & Diagnostics (as tabs); wide
                // right pane holds the Hub. Mirrors the operator's
                // own working layout (~270 / ~1010 split at 1280×720).
                // The split ratio is chosen to keep the rail >= 220
                // even at our 640×480 minimum window size, so the rail
                // tabs stay clickable on small displays.
                ImGuiID main_id = dockspace_id;
                ImGuiID left_id = 0, right_id = 0;
                // Left rail = ~22% of the dockspace width. Floor at 220
                // for usability on tiny windows.
                const float work_w   = viewport->WorkSize.x;
                const float left_pct = (work_w > 0.0f)
                    ? std::max(0.18f, std::min(0.30f, 220.0f / work_w))
                    : 0.21f;
                ImGui::DockBuilderSplitNode(main_id, ImGuiDir_Left,
                                            left_pct, &left_id, &right_id);
                ImGui::DockBuilderDockWindow("Games & Configuration", left_id);
                ImGui::DockBuilderDockWindow("Debug & Diagnostics",   left_id);
                ImGui::DockBuilderDockWindow("Hub",                   right_id);
                // Settings windows are popups (NoDocking) — they
                // intentionally float above the dockspace and aren't
                // listed here.
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
            // "Select Games Folder" used to live here but it duplicates
            // Settings → Games Folders, so it's been folded into that
            // tab. Exit is the only thing left in File since it's the
            // canonical place users look for "quit the app."
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
            // Single Settings… entry opens a tabbed window; everything
            // else (bindings, host config, hub server, games folders,
            // recent matches) lives as tabs inside that window. Discord
            // sign-in stays separate because its OAuth flow has its own
            // pending/error/success state machine.
            if (ImGui::MenuItem(T("menu_settings_window"))) {
                show_settings_ = true;
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
            // Show the actual Discord display name in the top-bar pill,
            // not the launcher's custom in-app nick — those can be
            // arbitrary strings and confuse users about which account
            // they're signed in to. Falls back to nick / "signed in" if
            // the cache is missing the new field (older auth.json).
            discord_nick_      = a.discord_global_name.empty()
                                 ? a.nick
                                 : a.discord_global_name;
        }

        // Kick off the version check exactly once on first menu-bar
        // render. Async; pill below shows the result whenever it lands.
        static bool s_did_update_check = false;
        if (!s_did_update_check) {
            s_did_update_check = true;
            fm2k::updater::CheckForUpdates();
            // Same first-frame slot also kicks the cnc-ddraw bundled
            // installer. Idempotent; if the install is up-to-date the
            // worker bails after one HTTPS round-trip. New installs
            // get auto-fetched without any user click.
            fm2k::cnc_ddraw::EnsureInstalled();
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
    // Single tabbed Settings window (replaces the five separate
    // floating settings sub-windows).
    RenderSettingsWindow();
    // Discord auth stays as its own window — OAuth pairing has its
    // own pending/error/success state machine that doesn't fit in
    // a tab next to the other static editors.
    if (show_discord_auth_) {
        RenderDiscordAuthWindow();
    }
    // Legacy floating-window paths kept for any code that still
    // toggles the per-section flags (none after the menu cleanup,
    // but defensive — opens nothing unless someone flips a flag).
    if (show_host_config_)    RenderHostConfigWindow();
    if (show_hub_server_)     RenderHubServerWindow();
    if (show_games_folders_)  RenderGamesFoldersWindow();
    if (show_recent_matches_) RenderRecentMatchesWindow();
}

// Settings → Hub Server… window. Lets the user point the launcher at a
// custom hub (their own hub.py instance, a friend's box, etc.) without
// cluttering the main Hub panel for casual users who just want the
// default 2dfm.sytes.net.
void LauncherUI::RenderHubServerBody() {
    if (!hub_host_initialized_) {
        hub_host_initialized_ = true;
        const char* env_h = std::getenv("FM2K_HUB_HOST");
        const char* def   = (env_h && env_h[0]) ? env_h : "2dfm.sytes.net";
        std::snprintf(hub_host_, sizeof(hub_host_), "%s", def);
    }
    ImGui::PushItemWidth(280);
    ImGui::InputText(T("netcfg_host"), hub_host_, sizeof(hub_host_));
    ImGui::PopItemWidth();
    ImGui::TextWrapped(
        "Hub server hostname or IP. Default 2dfm.sytes.net for public play. "
        "Use 127.0.0.1 (or localhost) when running your own hub.py on the same "
        "machine — NAT routers rarely hairpin so the public DNS won't loop back. "
        "Takes effect on next Connect.");
}

void LauncherUI::RenderHubServerWindow() {
    if (!ImGui::Begin("Hub Server", &show_hub_server_, ImGuiWindowFlags_None)) {
        ImGui::End();
        return;
    }
    RenderHubServerBody();
    ImGui::End();
}

// One Settings window with TabBar — bindings, host config, hub server,
// games folders, recent matches. Floats above the dockspace, can't be
// dragged or resized (popup-style modal feel without actually being a
// modal — the user can still click around outside it). Replaces the
// five separate floating sub-windows. Each tab calls a body-only
// renderer; click the X to close, settings auto-save on edit.
void LauncherUI::RenderSettingsWindow() {
    if (!show_settings_) return;

    // Center on the viewport at first open. Re-center on subsequent
    // opens so the window is always findable; user can still nudge it
    // via SetWindowPos in their imgui.ini if they really want.
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImVec2 size(560.0f, 420.0f);
    ImGui::SetNextWindowPos(
        ImVec2(vp->WorkPos.x + (vp->WorkSize.x - size.x) * 0.5f,
               vp->WorkPos.y + (vp->WorkSize.y - size.y) * 0.5f),
        ImGuiCond_Appearing);
    ImGui::SetNextWindowSize(size, ImGuiCond_Appearing);

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoDocking  |
        ImGuiWindowFlags_NoResize   |
        ImGuiWindowFlags_NoMove;

    if (!ImGui::Begin(T("menu_settings"), &show_settings_, flags)) {
        ImGui::End();
        return;
    }

    if (ImGui::BeginTabBar("##settings_tabs",
                           ImGuiTabBarFlags_Reorderable)) {
        // Input Bindings — single tab with a nested P1/P2 sub-tabbar so
        // the player picker doesn't bloat the top-level tabs.
        if (ImGui::BeginTabItem(T("input_bindings"))) {
            if (!input_binder_initialized_) {
                FM2KInputBinder::Init();
                input_binder_initialized_ = true;
            }
            if (ImGui::BeginTabBar("##input_bindings_players")) {
                if (ImGui::BeginTabItem(T("tab_p1"))) {
                    RenderInputBindingsTab(0);
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem(T("tab_p2"))) {
                    RenderInputBindingsTab(1);
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(T("panel_host_config"))) {
            RenderHostConfigBody();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(T("menu_hub_server"))) {
            RenderHubServerBody();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(T("menu_games_folders"))) {
            RenderGamesFoldersBody();
            ImGui::EndTabItem();
        }
        // Display — every cnc-ddraw [ddraw] setting. Lives here rather
        // than in the Debug & Diagnostics → Renderer tab because it's a
        // permanent config surface, not a dev knob.
        if (ImGui::BeginTabItem("Display")) {
            RenderDisplayBody();
            ImGui::EndTabItem();
        }
        // Recent Matches lives in the Hub panel (collapsing section
        // beside the room list), not Settings — match-history isn't a
        // configuration concern, it's session data.
        ImGui::EndTabBar();
    }

    ImGui::End();
}

// Settings → Games Folders… window. Multi-root editor for the launcher's
// games-discovery roots. Lives behind a Settings menu entry so the main
// panel stays focused on the games list itself; casual users with a
// single folder almost never need this UI after first-run setup.
void LauncherUI::RenderGamesFoldersBody() {
    // Per-row edit buffers persist across frames so users can keep
    // typing without ImGui resetting their input. Sized to match the
    // typical launcher.cfg line length; long absolute paths fit fine.
    static std::vector<std::array<char, 512>> row_bufs;
    static std::vector<std::string> last_seen;
    static std::array<char, 512> add_buf{};
    static bool add_buf_initialized = false;

    auto sync_rows_from_paths = [&]() {
        row_bufs.assign(games_root_paths_.size(), {});
        for (size_t i = 0; i < games_root_paths_.size(); ++i) {
            SDL_strlcpy(row_bufs[i].data(),
                        games_root_paths_[i].c_str(), row_bufs[i].size());
        }
        last_seen = games_root_paths_;
    };

    if (last_seen != games_root_paths_) sync_rows_from_paths();
    if (!add_buf_initialized) { add_buf_initialized = true; add_buf[0] = '\0'; }

    auto current_paths = [&]() -> std::vector<std::string> {
        std::vector<std::string> out;
        out.reserve(row_bufs.size());
        for (auto& buf : row_bufs) {
            if (buf[0] != '\0') out.emplace_back(buf.data());
        }
        return out;
    };
    auto commit = [&](std::vector<std::string> paths) {
        games_root_paths_ = paths;
        last_seen = paths;
        if (on_games_folders_set) on_games_folders_set(std::move(paths));
    };

    ImGui::TextWrapped("%s", T("hint_games_folders_window"));
    ImGui::Separator();

    ImGui::PushID("GamesFoldersWindow");
    int remove_index = -1;
    for (size_t i = 0; i < row_bufs.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        ImGui::SetNextItemWidth(380);
        bool changed = ImGui::InputText("##path", row_bufs[i].data(),
                                        row_bufs[i].size());
        ImGui::SameLine();
        if (ImGui::Button(T("btn_set")) ||
            (changed && ImGui::IsKeyPressed(ImGuiKey_Enter))) {
            commit(current_paths());
        }
        ImGui::SameLine();
        if (ImGui::Button(T("btn_remove"))) {
            remove_index = static_cast<int>(i);
        }
        ImGui::PopID();
    }
    if (remove_index >= 0 && remove_index < (int)row_bufs.size()) {
        row_bufs.erase(row_bufs.begin() + remove_index);
        commit(current_paths());
    }

    ImGui::Separator();
    ImGui::SetNextItemWidth(380);
    bool add_changed = ImGui::InputTextWithHint(
        "##add_path", T("hint_add_games_folder"),
        add_buf.data(), add_buf.size());
    ImGui::SameLine();
    if (ImGui::Button(T("btn_add")) ||
        (add_changed && ImGui::IsKeyPressed(ImGuiKey_Enter))) {
        if (add_buf[0] != '\0') {
            std::array<char, 512> nb{};
            SDL_strlcpy(nb.data(), add_buf.data(), nb.size());
            row_bufs.push_back(nb);
            add_buf[0] = '\0';
            commit(current_paths());
        }
    }
    ImGui::PopID();
}

void LauncherUI::RenderGamesFoldersWindow() {
    if (!ImGui::Begin(T("menu_games_folders"), &show_games_folders_,
                      ImGuiWindowFlags_None)) {
        ImGui::End();
        return;
    }
    RenderGamesFoldersBody();
    ImGui::End();
}

// Settings → Recent Matches… (#49). Read-only scoreboard fed by
// HubClient::RequestRecentMatches → K::RecentMatchesReceived. Refreshed
// on Connected and after every match_result so the just-finished match
// shows up at the top. The Refresh button forces an out-of-band fetch
// for users who left the launcher open between hub deployments.
void LauncherUI::RenderRecentMatchesBody() {
    auto& hs = *hub_state_;

    if (ImGui::Button(T("btn_refresh"))) {
        if (hs.client.IsConnected()) hs.client.RequestRecentMatches(50);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%u %s", (unsigned)hs.recent_matches.size(),
                        T("label_matches"));
    ImGui::Separator();

    if (!hs.recent_matches_loaded) {
        ImGui::TextDisabled("%s", T("status_loading"));
    } else if (hs.recent_matches.empty()) {
        ImGui::TextDisabled("%s", T("status_no_matches"));
    } else if (ImGui::BeginTable("##recent_matches", 6,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
            ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
            ImVec2(0, 360))) {
        ImGui::TableSetupColumn(T("col_when"),    ImGuiTableColumnFlags_WidthFixed, 130.0f);
        ImGui::TableSetupColumn(T("col_game"),    ImGuiTableColumnFlags_WidthFixed, 130.0f);
        ImGui::TableSetupColumn(T("col_p1"));
        ImGui::TableSetupColumn(T("col_p2"));
        ImGui::TableSetupColumn(T("col_stage"),   ImGuiTableColumnFlags_WidthFixed, 130.0f);
        ImGui::TableSetupColumn(T("col_winner"),  ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        const ImVec4 kWin (0.4f, 0.95f, 0.4f, 1.0f);
        const ImVec4 kLoss(0.95f, 0.4f, 0.4f, 1.0f);
        const ImVec4 kDraw(0.7f, 0.7f, 0.7f, 1.0f);

        for (const auto& m : hs.recent_matches) {
            ImGui::TableNextRow();

            // Localised time. finished_at is unix seconds; format with
            // strftime so it respects the system locale's date separator.
            ImGui::TableSetColumnIndex(0);
            char tbuf[64] = "";
            if (m.finished_at > 0.0) {
                std::time_t tt = static_cast<std::time_t>(m.finished_at);
                std::tm lt{};
                if (localtime_s(&lt, &tt) == 0) {
                    std::strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M", &lt);
                }
            }
            ImGui::TextUnformatted(tbuf[0] ? tbuf : "—");

            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(m.game_id.empty() ? "—" : m.game_id.c_str());

            // P1 / P2 nick + char. Char fallback chain: wire-baked name
            // (from MatchResult) wins; else local KGT lookup; else
            // id-only stub. -1 char_id renders just the nick (the row
            // simply lacks the char info — happens for older matches
            // committed before stage/char baking landed).
            auto draw_player = [&](const std::string& id, const std::string& nick,
                                   int char_id, const std::string& char_name) {
                std::string suffix;
                if (!char_name.empty()) {
                    suffix = " (" + char_name + ")";
                } else if (char_id >= 0 && on_resolve_char_name) {
                    std::string n = on_resolve_char_name(m.game_id,
                                                         (uint32_t)char_id);
                    if (!n.empty()) suffix = " (" + n + ")";
                    else            suffix = " (#" + std::to_string(char_id) + ")";
                }
                if (!hs.my_id.empty() && id == hs.my_id) {
                    ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.3f, 1.0f),
                                       "%s%s", nick.c_str(), suffix.c_str());
                } else {
                    ImGui::Text("%s%s", nick.c_str(), suffix.c_str());
                }
            };
            ImGui::TableSetColumnIndex(2);
            draw_player(m.p1_id, m.p1_nick, m.p1_char_id, m.p1_char_name);
            ImGui::TableSetColumnIndex(3);
            draw_player(m.p2_id, m.p2_nick, m.p2_char_id, m.p2_char_name);

            // Stage column. Same fallback chain as the live-matches
            // panel: wire-baked > local KGT lookup > id-only > "—".
            ImGui::TableSetColumnIndex(4);
            if (!m.stage_name.empty()) {
                ImGui::TextUnformatted(m.stage_name.c_str());
            } else if (m.stage_id >= 0) {
                std::string n;
                if (on_resolve_stage_name) {
                    n = on_resolve_stage_name(m.game_id,
                                              (uint32_t)m.stage_id);
                }
                if (!n.empty()) ImGui::TextUnformatted(n.c_str());
                else            ImGui::Text("Stage #%d", m.stage_id);
            } else {
                ImGui::TextDisabled("—");
            }

            // Winner column: from local user's perspective when they're
            // in the match (Win / Loss); pure side label otherwise.
            ImGui::TableSetColumnIndex(5);
            if (m.winner_id.empty()) {
                ImGui::TextColored(kDraw, "%s", T("outcome_draw"));
            } else if (!hs.my_id.empty() && (hs.my_id == m.p1_id || hs.my_id == m.p2_id)) {
                const bool i_won = (m.winner_id == hs.my_id);
                ImGui::TextColored(i_won ? kWin : kLoss, "%s",
                                   i_won ? T("outcome_win") : T("outcome_loss"));
            } else {
                const bool p1_won = (m.winner_id == m.p1_id);
                ImGui::TextUnformatted(p1_won ? "P1" : "P2");
            }
        }
        ImGui::EndTable();
    }
}

void LauncherUI::RenderRecentMatchesWindow() {
    if (!ImGui::Begin(T("menu_recent_matches"), &show_recent_matches_)) {
        ImGui::End();
        return;
    }
    RenderRecentMatchesBody();
    ImGui::End();
}

// Live-matches lobby panel. Fed by MatchInProgressStarted/Updated/Ended
// broadcasts + the CurrentMatchesReceived snapshot at connect time. Char
// + stage cells call fm2k::FormatCharLabel / FormatStageLabel which fall
// back to "Char #N" / "Stage #N" when the local launcher doesn't have
// the game installed AND the sender didn't bake the name into the
// payload (older client).
void LauncherUI::RenderInProgressMatchesBody() {
    auto& hs = *hub_state_;

    if (ImGui::CollapsingHeader(T("hub_live_matches_header"),
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::SmallButton(T("btn_refresh"))) {
            if (hs.client.IsConnected()) hs.client.RequestCurrentMatches();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%u %s", (unsigned)hs.current_matches.size(),
                            T("label_matches"));

        if (!hs.current_matches_loaded) {
            ImGui::TextDisabled("%s", T("status_loading"));
            return;
        }
        if (hs.current_matches.empty()) {
            ImGui::TextDisabled("%s", T("status_no_matches"));
            return;
        }

        // Sort newest-first on every render — list size stays small
        // (handful of in-flight matches max) so an in-place sort is
        // cheaper than maintaining a sorted insert in the dispatcher.
        std::sort(hs.current_matches.begin(), hs.current_matches.end(),
                  [](const auto& a, const auto& b) {
                      return a.started_at > b.started_at;
                  });

        if (ImGui::BeginTable("##live_matches", 5,
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
                ImVec2(0, 200))) {
            ImGui::TableSetupColumn(T("col_game"),
                ImGuiTableColumnFlags_WidthFixed, 110.0f);
            ImGui::TableSetupColumn(T("col_p1"));
            ImGui::TableSetupColumn(T("col_p2"));
            ImGui::TableSetupColumn(T("col_stage"),
                ImGuiTableColumnFlags_WidthFixed, 130.0f);
            ImGui::TableSetupColumn(T("col_duration"),
                ImGuiTableColumnFlags_WidthFixed, 70.0f);
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableHeadersRow();

            const double now = (double)std::time(nullptr);
            for (const auto& m : hs.current_matches) {
                ImGui::TableNextRow();

                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(m.game_id.empty() ? "—" : m.game_id.c_str());

                auto fmt_char = [&](const std::string& nick,
                                    const std::string& id,
                                    int char_id,
                                    const std::string& char_name) {
                    // Fallback chain:
                    //   1. wire-baked name   "瑞希君 (#3)"  (sender had it)
                    //   2. local KGT lookup   ("game installed locally")
                    //   3. id-only stub      "Char #3"
                    //   4. unreported (CSS)  "(selecting…)"  dim italic
                    if (char_id < 0 && char_name.empty()) {
                        // CSS state: no chars locked in yet. Render the
                        // nick normally (so the player is still visible)
                        // and tag the char slot dim so the row reads as
                        // "in CSS" at a glance.
                        const ImVec4 self(0.95f, 0.85f, 0.3f, 1.0f);
                        const bool is_self = (!hs.my_id.empty() && id == hs.my_id);
                        if (is_self) {
                            ImGui::TextColored(self, "%s", nick.c_str());
                        } else {
                            ImGui::TextUnformatted(nick.c_str());
                        }
                        ImGui::SameLine();
                        ImGui::TextDisabled("— %s", T("status_in_css"));
                        return;
                    }
                    std::string label;
                    if (!char_name.empty()) {
                        label = char_name + " (#" +
                                std::to_string(char_id) + ")";
                    } else if (on_resolve_char_name) {
                        std::string n = on_resolve_char_name(
                            m.game_id, (uint32_t)char_id);
                        label = n.empty()
                            ? ("Char #" + std::to_string(char_id))
                            : (n + " (#" + std::to_string(char_id) + ")");
                    } else {
                        label = "Char #" + std::to_string(char_id);
                    }
                    if (!hs.my_id.empty() && id == hs.my_id) {
                        ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.3f, 1.0f),
                                           "%s — %s", nick.c_str(),
                                           label.c_str());
                    } else {
                        ImGui::Text("%s — %s", nick.c_str(), label.c_str());
                    }
                };

                ImGui::TableSetColumnIndex(1);
                fmt_char(m.p1_nick, m.p1_id, m.p1_char_id, m.p1_char_name);
                ImGui::TableSetColumnIndex(2);
                fmt_char(m.p2_nick, m.p2_id, m.p2_char_id, m.p2_char_name);

                ImGui::TableSetColumnIndex(3);
                if (!m.stage_name.empty()) {
                    ImGui::Text("%s (#%d)", m.stage_name.c_str(),
                                m.stage_id);
                } else if (m.stage_id >= 0) {
                    // Best-effort local resolve via the launcher's
                    // FindKgtByGameId callback. Empty string means the
                    // game isn't installed locally — fall back to
                    // id-only stub.
                    std::string n;
                    if (on_resolve_stage_name) {
                        n = on_resolve_stage_name(m.game_id,
                                                  (uint32_t)m.stage_id);
                    }
                    if (!n.empty()) {
                        ImGui::Text("%s (#%d)", n.c_str(), m.stage_id);
                    } else {
                        ImGui::Text("Stage #%d", m.stage_id);
                    }
                } else {
                    // No stage reported yet — same CSS-state visualization
                    // as the char cells.
                    ImGui::TextDisabled("%s", T("status_in_css"));
                }

                ImGui::TableSetColumnIndex(4);
                if (m.started_at > 0.0 && now > m.started_at) {
                    int secs = (int)(now - m.started_at);
                    int mins = secs / 60;
                    secs %= 60;
                    ImGui::Text("%d:%02d", mins, secs);
                } else {
                    ImGui::TextDisabled("—");
                }
            }
            ImGui::EndTable();
        }
    }
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

// Tiny int-keyed setting reader/writer — same flat key=value format as
// the bool helpers; integers like SOCD mode use this. Default returned
// when the key is missing or the value isn't a valid int.
static int ReadIntSetting(const std::string& path, const char* key, int dflt) {
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f) return dflt;
    char line[256];
    int out = dflt;
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
        char* endp = nullptr;
        long parsed = std::strtol(v, &endp, 10);
        if (endp && endp != v) out = (int)parsed;
        break;
    }
    std::fclose(f);
    return out;
}

static void WriteIntSetting(const std::string& path, const char* key, int value) {
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
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%d", value);
    for (auto& p : kv) if (p.first == key) { p.second = buf; found = true; }
    if (!found) kv.emplace_back(key, buf);
    if (FILE* f = std::fopen(path.c_str(), "w")) {
        for (const auto& p : kv) std::fprintf(f, "%s=%s\n", p.first.c_str(), p.second.c_str());
        std::fclose(f);
    }
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

void LauncherUI::FireSystemNotification(const std::string& title_utf8,
                                        const std::string& body_utf8) {
    HWND hwnd = nullptr;
    if (window_) {
        hwnd = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(window_),
                                            SDL_PROP_WINDOW_WIN32_HWND_POINTER,
                                            nullptr);
    }
    if (notify_flash_ && hwnd && hwnd != GetForegroundWindow()) {
        FLASHWINFO fi = { sizeof(fi), hwnd,
                          FLASHW_ALL | FLASHW_TIMERNOFG, 0, 0 };
        FlashWindowEx(&fi);
    }
    if (notify_sound_) {
        MessageBeep(MB_ICONWARNING);
    }
    if (notify_toast_ && hwnd) {
        NOTIFYICONDATAW nid{};
        nid.cbSize = sizeof(nid);
        nid.hWnd   = hwnd;
        nid.uID    = 1;
        nid.hIcon  = LoadIcon(nullptr, IDI_APPLICATION);

        auto utf8_to_wide = [](const char* in, wchar_t* out, int out_len) {
            if (!in || !out || out_len <= 0) return;
            int n = MultiByteToWideChar(CP_UTF8, 0, in, -1, out, out_len);
            if (n <= 0) out[0] = L'\0';
        };
        utf8_to_wide("FM2K Rollback Launcher", nid.szTip,
                     (int)(sizeof(nid.szTip) / sizeof(WCHAR)));

        nid.uFlags = NIF_ICON | NIF_TIP;
        Shell_NotifyIconW(NIM_ADD, &nid);

        nid.uFlags      = NIF_INFO | NIF_ICON | NIF_TIP;
        nid.dwInfoFlags = NIIF_WARNING;
        utf8_to_wide(title_utf8.c_str(), nid.szInfoTitle,
                     (int)(sizeof(nid.szInfoTitle) / sizeof(WCHAR)));
        utf8_to_wide(body_utf8.c_str(),  nid.szInfo,
                     (int)(sizeof(nid.szInfo) / sizeof(WCHAR)));
        Shell_NotifyIconW(NIM_MODIFY, &nid);

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
                      ImGuiWindowFlags_None)) {
        ImGui::End();
        return;
    }

    if (s_cached.valid) {
        // Display the actual Discord global_name; nick is the launcher-
        // local custom override which can be anything.
        const std::string display = s_cached.discord_global_name.empty()
            ? s_cached.nick : s_cached.discord_global_name;
        ImGui::TextColored(ImVec4(0.3f, 0.8f, 0.4f, 1.0f),
                           "Signed in as %s", display.c_str());
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
                // Surface the Discord display name (global_name) on the
                // sign-in confirmation, not the launcher's custom nick —
                // the user is verifying which Discord account they
                // bound, not what nick they'll appear as in lobbies.
                const std::string display = a.discord_global_name.empty()
                    ? a.nick : a.discord_global_name;
                s_status = "Signed in as " + display + ". You can connect to the hub now.";
                s_pairing.reset();
                // Refresh the menu-bar pill so it flips green this frame.
                discord_signed_in_ = true;
                discord_nick_      = display;
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
void LauncherUI::RenderHostConfigBody() {
    ImGui::TextWrapped(
        "Per-game match settings. Edits here override the game's "
        "default game.ini for THIS launcher; the host's resolved values "
        "get pushed to the client + spectators on challenge (#54). "
        "HitJudge / GameInformation are force-zeroed online — saved "
        "to disk for offline practice but never applied to a hub match.");
    ImGui::Separator();

    if (selected_game_index_ < 0 ||
        selected_game_index_ >= (int)games_.size())
    {
        ImGui::TextDisabled("%s", T("warn_no_game_selected"));
        return;
    }
    const auto& game = games_[selected_game_index_];
    // Wide-construct so JP-named exes (ＣＰＷ.exe etc.) keep their
    // bytes intact through stem()/parent_path() instead of
    // round-tripping through MinGW's ANSI codepage.
    const std::filesystem::path exe =
        fm2k::utf8path::Utf8ToWide(game.exe_path);
    const std::filesystem::path ini = fm2k::game_ini::PathForExe(exe);

    static int          s_loaded_for = -1;
    static fm2k::game_ini::GamePlayConfig s_defaults;
    static fm2k::game_ini::GamePlayConfig s_override;
    static bool         s_dirty = false;
    if (s_loaded_for != selected_game_index_) {
        s_loaded_for = selected_game_index_;
        s_defaults = {};
        s_override = {};
        fm2k::game_ini::Load(ini, s_defaults);
        fm2k::game_ini::LoadOverride(exe, s_override);
        s_dirty = false;
    }

    // Render via TextUnformatted instead of Text("%s", ...). MinGW's
    // vsnprintf goes through the C locale's narrow conversion and
    // turns non-CP1252 bytes (full-width forms like ＣＰＷ.exe) into
    // '_'. The games list above gets away with TextUnformatted +
    // SameLine; we do the same here for the static prefix and the
    // dynamic name. ImGui itself decodes UTF-8 just fine — the
    // mangling is exclusively in printf-style format specifiers.
    ImGui::TextUnformatted("Game: ");
    ImGui::SameLine(0.0f, 0.0f);
    {
        // One-shot diagnostic: dump the bytes we're handing to ImGui
        // so we can see whether they survive (vs. games-list which
        // works) or get mangled by some intermediate layer. Logs once
        // per game-selection change.
        static int s_logged_for = -2;
        if (s_logged_for != selected_game_index_) {
            s_logged_for = selected_game_index_;
            const std::string n = game.GetExeName();
            std::string hex;
            char buf[8];
            for (unsigned char c : n) {
                std::snprintf(buf, sizeof(buf), "%02X ", c);
                hex += buf;
            }
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "HostConfig: GetExeName='%s' (%zu bytes: %s)",
                n.c_str(), n.size(), hex.c_str());
        }
        ImGui::TextUnformatted(game.GetExeName().c_str());
    }

    {
        std::string ini_display = game.GetExeDir();
        if (!ini_display.empty() && ini_display.back() != '/' &&
            ini_display.back() != '\\') {
            ini_display += '/';
        }
        ini_display += "game.ini";
        // TextDisabled has the same printf trap — wrap it manually.
        ImGui::PushStyleColor(ImGuiCol_Text,
            ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextUnformatted("game.ini: ");
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::TextUnformatted(ini_display.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::Spacing();

    if (ImGui::BeginTable("##gameplay_overrides", 4,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders)) {
        ImGui::TableSetupColumn("Setting",   ImGuiTableColumnFlags_WidthFixed, 170.0f);
        ImGui::TableSetupColumn("Default",   ImGuiTableColumnFlags_WidthFixed,  70.0f);
        ImGui::TableSetupColumn("Override",  ImGuiTableColumnFlags_WidthFixed,  90.0f);
        ImGui::TableSetupColumn("Notes");
        ImGui::TableHeadersRow();

        struct Row {
            const char* label;
            int fm2k::game_ini::GamePlayConfig::* member;
            const char* note;
            int min;
            int max;
        };
        static const Row rows[] = {
            {"Round count (1v1)",   &fm2k::game_ini::GamePlayConfig::vs_single_play, "Editor.TestPlay.VSSinglePlay",      1,  9},
            {"Round count (team)",  &fm2k::game_ini::GamePlayConfig::vs_team_play,   "Editor.TestPlay.VSTeamPlay",        1,  9},
            {"Round timer (s)",     &fm2k::game_ini::GamePlayConfig::time,           "0 = infinite",                      0, 99},
            {"Game speed",          &fm2k::game_ini::GamePlayConfig::game_speed,     "default 10",                        1, 16},
            {"Stage",               &fm2k::game_ini::GamePlayConfig::stage_nb,       "stage index, 0 = first",            0, 99},
            {"Joystick (0=KB,1=Pad)",&fm2k::game_ini::GamePlayConfig::joystick,      "force 0 unless game lags",          0,  1},
            {"P0 CPU",              &fm2k::game_ini::GamePlayConfig::player0_cpu,    "force 0 online (anti-cheat)",       0,  1},
            {"P1 CPU",              &fm2k::game_ini::GamePlayConfig::player1_cpu,    "force 0 online (anti-cheat)",       0,  1},
            {"Hit-judge overlay",   &fm2k::game_ini::GamePlayConfig::hit_judge,      "FORCED 0 online (anti-cheat)",      0,  1},
            {"Damage info overlay", &fm2k::game_ini::GamePlayConfig::game_information,"FORCED 0 online (anti-cheat)",     0,  1},
            {"VS mode",             &fm2k::game_ini::GamePlayConfig::vs_mode,        "Editor.TestPlay.VSMode",            0,  9},
        };

        for (const auto& r : rows) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(r.label);

            ImGui::TableSetColumnIndex(1);
            const int def_val = s_defaults.*r.member;
            if (def_val == fm2k::game_ini::kUnset) {
                ImGui::TextDisabled("—");
            } else {
                ImGui::Text("%d", def_val);
            }

            ImGui::TableSetColumnIndex(2);
            ImGui::PushID(r.label);
            int  cur     = s_override.*r.member;
            bool enabled = (cur != fm2k::game_ini::kUnset);
            bool prev_enabled = enabled;
            ImGui::Checkbox("##en", &enabled);
            if (enabled != prev_enabled) {
                if (enabled) {
                    s_override.*r.member = (def_val == fm2k::game_ini::kUnset)
                        ? r.min : def_val;
                } else {
                    s_override.*r.member = fm2k::game_ini::kUnset;
                }
                s_dirty = true;
            }
            if (enabled) {
                ImGui::SameLine();
                int v = s_override.*r.member;
                ImGui::SetNextItemWidth(60.0f);
                if (ImGui::InputInt("##v", &v, 0)) {
                    if (v < r.min) v = r.min;
                    if (v > r.max) v = r.max;
                    s_override.*r.member = v;
                    s_dirty = true;
                }
            }
            ImGui::PopID();

            ImGui::TableSetColumnIndex(3);
            ImGui::TextDisabled("%s", r.note);
        }
        ImGui::EndTable();
    }

    ImGui::Separator();
    if (s_dirty) {
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f),
                           "Unsaved overrides — apply or reset.");
    }
    if (ImGui::Button("Apply overrides")) {
        if (fm2k::game_ini::SaveOverride(exe, s_override)) {
            s_dirty = false;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Host config: saved overrides for %s",
                game.GetExeName().c_str());
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset to defaults")) {
        s_override = {};
        fm2k::game_ini::SaveOverride(exe, s_override);
        s_dirty = false;
    }
    ImGui::SameLine();
    if (ImGui::Button("Reload")) {
        s_loaded_for = -1;
    }

    // ─── Random stage (#56) ──────────────────────────────────────
    // Per-launcher persistence (settings.ini, not per-game) so the
    // user toggles once and it follows them across games. Both peers
    // run the same xorshift sequence from a shared seed so rematches
    // re-roll deterministically without per-match wire traffic.
    ImGui::Spacing();
    ImGui::SeparatorText("Random stage");
    if (!random_state_loaded_) {
        random_state_loaded_ = true;
        LoadRandomStageState();
    }
    bool prev_enable = random_stage_enable_;
    if (ImGui::Checkbox("Enable random stage", &random_stage_enable_)) {
        SaveRandomStageState();
    }
    if (random_stage_enable_) {
        ImGui::SameLine();
        ImGui::TextDisabled("(rolls a fresh stage each match)");
        ImGui::SetNextItemWidth(80);
        if (ImGui::InputInt("Min stage", &random_stage_min_, 0)) {
            if (random_stage_min_ < 0) random_stage_min_ = 0;
            if (random_stage_min_ > random_stage_max_) random_stage_min_ = random_stage_max_;
            SaveRandomStageState();
        }
        ImGui::SetNextItemWidth(80);
        if (ImGui::InputInt("Max stage", &random_stage_max_, 0)) {
            if (random_stage_max_ < random_stage_min_) random_stage_max_ = random_stage_min_;
            if (random_stage_max_ > 99) random_stage_max_ = 99;
            SaveRandomStageState();
        }
        ImGui::TextWrapped(
            "Inclusive range. Set to your game's stage count - 1 (FM2K "
            "indexes from 0). Both peers' hooks seed an xorshift PRNG with "
            "the host's seed, then advance by one per match — deterministic "
            "lockstep, no extra wire traffic per rematch.");
    }
    (void)prev_enable;
}

void LauncherUI::RenderHostConfigWindow() {
    if (!ImGui::Begin("Host Config", &show_host_config_, ImGuiWindowFlags_None)) {
        ImGui::End();
        return;
    }
    RenderHostConfigBody();
    ImGui::End();
}

void LauncherUI::RenderGameSelection() {
    // Games-folder list editor lives in Settings → Games Folders… The
    // main panel just shows the current root count + a button to open
    // the editor, so the games list itself dominates the panel.
    {
        const size_t n = games_root_paths_.size();
        if (n == 0) {
            ImGui::TextDisabled("%s", T("status_invalid_games_folder"));
        } else if (n == 1) {
            ImGui::TextDisabled("%s: %s", T("panel_games_folder"),
                                games_root_paths_[0].c_str());
        } else {
            ImGui::TextDisabled("%s: %u", T("panel_games_folders"),
                                static_cast<unsigned>(n));
        }
        ImGui::SameLine();
        if (ImGui::SmallButton(T("btn_edit_games_folders"))) {
            show_games_folders_ = true;
        }
    }

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

            // Compact two-tone row, engine tag on the LEFT:
            //   [2K] wanwan.exe       (tag dim gray, name normal)
            //   [95] CPW.exe          ditto for FM95
            //   [2K] AOB.exe          (both yellow when packer detected)
            // The Selectable owns the click + selection highlight; we overlay
            // the two-color text on top so tag and name carry independent
            // colors. ImGuiSelectableFlags_AllowItemOverlap lets the text
            // sit above the click region without blocking it.
            const bool packed = !game.packer_label.empty();
            const char* engine_tag = (game.engine == FM2K::Engine::FM95) ? "[95]" : "[2K]";

            ImVec2 cursor = ImGui::GetCursorScreenPos();
            const float row_h = ImGui::GetTextLineHeightWithSpacing();
            const float row_w = ImGui::GetContentRegionAvail().x;

            bool clicked = ImGui::Selectable("##row_sel", is_selected,
                                             ImGuiSelectableFlags_AllowItemOverlap,
                                             ImVec2(row_w, row_h));
            const bool hovered = ImGui::IsItemHovered();

            // Overlay the text on top of the (now invisible-labeled) Selectable.
            ImGui::SetCursorScreenPos(cursor);
            const ImVec4 yellow = ImVec4(0.92f, 0.78f, 0.30f, 1.0f);
            const ImVec4 dim    = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);

            ImGui::PushStyleColor(ImGuiCol_Text, packed ? yellow : dim);
            ImGui::TextUnformatted(engine_tag);
            ImGui::PopStyleColor();
            ImGui::SameLine();

            if (packed) ImGui::PushStyleColor(ImGuiCol_Text, yellow);
            ImGui::TextUnformatted(game.GetExeName().c_str());
            if (packed) ImGui::PopStyleColor();

            if (hovered) {
                if (packed) {
                    ImGui::SetTooltip("Packed with %s — may not run with rollback hooks until unpacked.\n"
                                      "Hash: 0x%016llx",
                                      game.packer_label.c_str(),
                                      (unsigned long long)game.xxh64);
                } else if (!game.clean_label.empty()) {
                    ImGui::SetTooltip("%s\nHash: 0x%016llx (registered)",
                                      game.clean_label.c_str(),
                                      (unsigned long long)game.xxh64);
                } else {
                    ImGui::SetTooltip("Hash: 0x%016llx",
                                      (unsigned long long)game.xxh64);
                }
            }
            if (clicked) {
                selected_game_index_ = static_cast<int>(i);
                if (on_game_selected) {
                    on_game_selected(game);
                }
                // Route the input binder to this game's per-game profile
                // (creates fm2k_inputs_<basename>.ini lookup; reads
                // default if no override exists). Strip .exe suffix.
                // Construct path from wide so stem() preserves JP bytes.
                std::filesystem::path p(
                    fm2k::utf8path::Utf8ToWide(game.exe_path));
                std::string stem = fm2k::utf8path::StemUtf8(p);
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
                if (game.kgt.valid) {
                    int p = 0, s = 0, d = 0;
                    for (const auto& n : game.kgt.player_names) if (!n.empty()) ++p;
                    for (const auto& n : game.kgt.stage_names)  if (!n.empty()) ++s;
                    for (const auto& n : game.kgt.demo_names)   if (!n.empty()) ++d;
                    ImGui::SetTooltip(
                        "EXE: %s\nKGT: %s\nProject: %s\n%d chars / %d stages / %d demos",
                        game.exe_path.c_str(), game.dll_path.c_str(),
                        game.kgt.project_name.empty() ? "(unnamed)" : game.kgt.project_name.c_str(),
                        p, s, d);
                } else {
                    ImGui::SetTooltip("EXE: %s\nKGT: %s", game.exe_path.c_str(), game.dll_path.c_str());
                }
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

void LauncherUI::SetGamesRootPaths(const std::vector<std::string>& paths) {
    games_root_paths_ = paths;
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
        // FM95-specific opt-in: drive the trampoline tick from
        // Hook_UpdateGameState instead of the (skipped) RUN_GAME_LOOP
        // detour, so FM95 reaches rollback parity with FM2K.
        static bool s_fm95_trampoline   = false;
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
                std::filesystem::path exe(
                    fm2k::utf8path::Utf8ToWide(games_[selected_game_index_].exe_path));
                const std::string game_id = fm2k::utf8path::StemUtf8(exe);
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
            ::SetEnvironmentVariableA("FM95_TRAMPOLINE",
                                      s_fm95_trampoline ? "1" : nullptr);
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

            // ---------- FM2K diagnostics (collapsed by default) ----------
            // FM2K-engine-specific toggles — most users never touch these
            // outside of debugging desync repros.
            if (ImGui::CollapsingHeader("FM2K diagnostics")) {
                ImGui::Indent();

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

                ImGui::Unindent();
            }

            // ---------- FM95 / CPW (collapsed by default) ----------
            // Engine-specific to FM95Hook.dll-injected games. Won't fire
            // on FM2K builds — environment vars get set anyway, and
            // FM2KHook just ignores them.
            if (ImGui::CollapsingHeader("FM95 (CPW etc.)")) {
                ImGui::Indent();

                ImGui::Checkbox("Trampoline-driven loop (FM95_TRAMPOLINE)",
                                &s_fm95_trampoline);
                ImGui::SetItemTooltip(
                    "FM95's RUN_GAME_LOOP is _WinMain (no separate driver), so the "
                    "trampoline can't replace it like on FM2K. With this enabled, "
                    "Hook_UpdateGameState calls TrampolineFrameTick() and Hook_"
                    "RenderGame skips the host's natural render — the trampoline's "
                    "RenderFrameWithSnapshot drives one render per frame. Required "
                    "for FM95 rollback parity. OFF = current working baseline (no "
                    "rollback driver, host runs CPW natively). Toggle off if you "
                    "see regressions.");

                ImGui::Unindent();
            }

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

// Local match log — CCCaster's results.csv with FM2K-specific columns.
// One row per match end, written from MY perspective (the user running
// THIS launcher), so each peer keeps its own view. Format:
//
//   when_iso, game_id, role, my_nick, my_char, peer_nick, peer_char, result
//
// "role" is "P1" or "P2" depending on whether we hosted the match. CCCaster
// always writes one record per match, but only on `_localPlayer == 1`; we
// instead write per-perspective so the file is meaningful even if the user
// switches between hosting and joining.
//
// "result" is the same outcome string we send to the hub: self_won /
// peer_won / draw / disconnect. CCCaster used per-side round counts; we
// only have final winner so we surface that directly.
//
// Lives in the same %APPDATA%\FM2K_Rollback dir as audio.ini / settings.ini.
// First write emits a UTF-8 BOM so Excel renders Japanese / accented names
// correctly when the user double-clicks the file.
static std::string ResultsCsvPath() {
    const char* a = std::getenv("APPDATA");
    if (!a || !*a) return "";
    std::string dir = std::string(a) + "\\FM2K_Rollback";
    CreateDirectoryA(dir.c_str(), nullptr);
    return dir + "\\results.csv";
}

// RFC-4180 escape: wrap in quotes if the field contains comma / quote /
// newline, doubling internal quotes. Empty string → empty (unquoted).
static std::string CsvEscape(const std::string& in) {
    bool needs_quote = false;
    for (char c : in) {
        if (c == ',' || c == '"' || c == '\n' || c == '\r') {
            needs_quote = true;
            break;
        }
    }
    if (!needs_quote) return in;
    std::string out;
    out.reserve(in.size() + 2);
    out.push_back('"');
    for (char c : in) {
        if (c == '"') out.push_back('"');
        out.push_back(c);
    }
    out.push_back('"');
    return out;
}

void LauncherUI::AppendResultsCsvRow(const char* outcome_str,
                                     uint32_t p1_char_id, uint32_t p2_char_id,
                                     const std::string& p1_char_name,
                                     const std::string& p2_char_name) {
    if (!hub_state_) return;
    auto& hs = *hub_state_;
    const std::string path = ResultsCsvPath();
    if (path.empty()) return;

    // Detect first-write so we can emit BOM + header. GetFileAttributesA
    // is the cheapest existence check on Win32.
    const bool fresh =
        (GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES);

    FILE* f = std::fopen(path.c_str(), "ab");
    if (!f) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "results.csv: open failed: %s", path.c_str());
        return;
    }
    if (fresh) {
        // UTF-8 BOM (EF BB BF). Excel needs this to interpret the file
        // as UTF-8 instead of guessing the system codepage.
        const unsigned char bom[3] = { 0xEF, 0xBB, 0xBF };
        std::fwrite(bom, 1, sizeof(bom), f);
        std::fprintf(f,
            "when,game_id,role,my_nick,my_char,peer_nick,peer_char,result\r\n");
    }

    // Map our role to a P1/P2 label: host == P1 by hub convention.
    const bool i_am_p1 = (hs.current_match_role == "host");
    const std::string my_char_name   = i_am_p1 ? p1_char_name : p2_char_name;
    const std::string peer_char_name = i_am_p1 ? p2_char_name : p1_char_name;
    const uint32_t    my_char_id     = i_am_p1 ? p1_char_id   : p2_char_id;
    const uint32_t    peer_char_id   = i_am_p1 ? p2_char_id   : p1_char_id;

    // Prefer the resolved .player filename; fall back to "id N" when the
    // hook didn't manage to resolve (older shared-mem version, char_id
    // out of roster range, etc).
    auto fmt_char = [](const std::string& name, uint32_t id) -> std::string {
        if (!name.empty()) return name;
        if (id == 0xFFFFFFFFu) return "";
        return "id " + std::to_string(id);
    };

    // ISO-8601 timestamp in local time. Sortable + readable; Excel parses
    // this format as a date out of the box.
    char when_iso[32] = "";
    std::time_t now = std::time(nullptr);
    std::tm lt{};
    if (localtime_s(&lt, &now) == 0) {
        std::strftime(when_iso, sizeof(when_iso), "%Y-%m-%dT%H:%M:%S", &lt);
    }

    std::fprintf(f, "%s,%s,%s,%s,%s,%s,%s,%s\r\n",
        CsvEscape(when_iso).c_str(),
        CsvEscape(hs.current_match_game_id).c_str(),
        i_am_p1 ? "P1" : "P2",
        CsvEscape(hs.my_nick).c_str(),
        CsvEscape(fmt_char(my_char_name, my_char_id)).c_str(),
        CsvEscape(hs.current_match_peer_nick).c_str(),
        CsvEscape(fmt_char(peer_char_name, peer_char_id)).c_str(),
        CsvEscape(outcome_str ? outcome_str : "").c_str());

    std::fclose(f);
}

void LauncherUI::UpdateWindowTitleWithRecord() {
    if (!window_ || !hub_state_) return;
    auto& hs = *hub_state_;
    char title[192];
    if (hs.my_wins < 0) {
        // No record yet — keep the bare title.
        std::snprintf(title, sizeof(title), "FM2K Rollback Launcher");
    } else if (!hs.my_nick.empty()) {
        std::snprintf(title, sizeof(title),
                      "FM2K Rollback Launcher \xe2\x80\x94 %s (%d-%d-%d)",
                      hs.my_nick.c_str(), hs.my_wins, hs.my_losses, hs.my_draws);
    } else {
        std::snprintf(title, sizeof(title),
                      "FM2K Rollback Launcher \xe2\x80\x94 %d-%d-%d",
                      hs.my_wins, hs.my_losses, hs.my_draws);
    }
    SDL_SetWindowTitle(window_, title);
}

void LauncherUI::LoadSocdState() {
    const std::string path = NotifySettingsPath();   // shared settings.ini
    if (path.empty()) return;
    socd_mode_[0] = ReadIntSetting(path, "socd_mode_p1", 1);  // 1 = Hitbox SOCD
    socd_mode_[1] = ReadIntSetting(path, "socd_mode_p2", 1);
    // Clamp to known range so a hand-edited bad value can't blow up
    // the hook's switch statement.
    for (int i = 0; i < 2; ++i) {
        if (socd_mode_[i] < 0 || socd_mode_[i] > 5) socd_mode_[i] = 1;
    }
}

void LauncherUI::SaveSocdState() {
    const std::string path = NotifySettingsPath();
    if (path.empty()) return;
    WriteIntSetting(path, "socd_mode_p1", socd_mode_[0]);
    WriteIntSetting(path, "socd_mode_p2", socd_mode_[1]);
}

void LauncherUI::LoadRandomStageState() {
    const std::string path = NotifySettingsPath();
    if (path.empty()) return;
    random_stage_enable_ = (ReadIntSetting(path, "random_stage_enable", 0) != 0);
    random_stage_min_    = ReadIntSetting(path, "random_stage_min", 0);
    random_stage_max_    = ReadIntSetting(path, "random_stage_max", 7);
    if (random_stage_min_ < 0)   random_stage_min_ = 0;
    if (random_stage_max_ > 99)  random_stage_max_ = 99;
    if (random_stage_max_ < random_stage_min_) random_stage_max_ = random_stage_min_;
}

void LauncherUI::SaveRandomStageState() {
    const std::string path = NotifySettingsPath();
    if (path.empty()) return;
    WriteIntSetting(path, "random_stage_enable", random_stage_enable_ ? 1 : 0);
    WriteIntSetting(path, "random_stage_min",    random_stage_min_);
    WriteIntSetting(path, "random_stage_max",    random_stage_max_);
}

void LauncherUI::RenderInputBindingsTab(int player_slot) {
    if (player_slot < 0 || player_slot > 1) return;

    if (!socd_state_loaded_) {
        socd_state_loaded_ = true;
        LoadSocdState();
    }

    // SOCD picker — purely local. Each P1/P2 slot keeps its own mode
    // because dual-local dev mode runs both slots from one launcher
    // and wants each child process configured independently. Online
    // mode applies socd_mode_[g_player_index] to the spawned game's
    // FM2K_SOCD_MODE env var at launch.
    static const char* kSocdLabels[6] = {
        "0 — Default        (R wins L+R, U wins U+D)",
        "1 — Hitbox SOCD    (L+R neutral, U wins U+D)  [tournament default]",
        "2 — U/D Cancel     (R wins L+R, U+D neutral)",
        "3 — Both Cancel    (L+R neutral, U+D neutral)",
        "4 — Up Bias        (R wins L+R, U wins U+D)",
        "5 — Hitbox + UpBias",
    };
    ImGui::TextDisabled(
        "SOCD is local — applied before inputs hit the wire, so peers "
        "running different modes do NOT desync.");
    ImGui::SetNextItemWidth(380);
    char combo_id[32];
    std::snprintf(combo_id, sizeof(combo_id), "##socd_p%d", player_slot + 1);
    if (ImGui::Combo(combo_id, &socd_mode_[player_slot], kSocdLabels, 6)) {
        SaveSocdState();
        // Live-update the env so a freshly-spawned game picks up the
        // new mode; running games don't reload (hook caches on first
        // GetSOCDMode call) — they get the new value next launch.
        char buf[8];
        std::snprintf(buf, sizeof(buf), "%d", socd_mode_[player_slot]);
        if (player_slot == 0) _putenv_s("FM2K_SOCD_MODE_P1", buf);
        else                   _putenv_s("FM2K_SOCD_MODE_P2", buf);
    }
    ImGui::Separator();

    // Bindings body — inherits the existing per-player binding UI.
    if (FM2KInputBinder::RenderBody(player_slot)) FM2KInputBinder::Save();
}

void LauncherUI::PushStatsToHook() {
    if (!hub_state_ || !on_get_client_status) return;
    auto& hs = *hub_state_;

    uint32_t pid1 = 0, pid2 = 0;
    if (!on_get_client_status(pid1, pid2)) return;

    // Resolve vs-peer record from the cached breakdown (filled by the
    // hub's `record` event with no opponent_id filter — the same data
    // the lobby's vs column reads).
    int32_t vs_w = -1, vs_l = -1, vs_d = -1;
    if (!hs.current_match_peer_id.empty()) {
        auto it = hs.my_vs.find(hs.current_match_peer_id);
        if (it != hs.my_vs.end()) {
            vs_w = it->second.wins;
            vs_l = it->second.losses;
            vs_d = it->second.draws;
        } else {
            // No prior matches against this peer — explicit zeros so the
            // titlebar shows "0-0-0" instead of dashes.
            vs_w = vs_l = vs_d = 0;
        }
    }

    auto write = [&](uint32_t pid) {
        if (pid == 0) return;
        const std::string name = "FM2K_SharedMem_" + std::to_string(pid);
        HANDLE h = OpenFileMappingA(FILE_MAP_READ | FILE_MAP_WRITE,
                                    FALSE, name.c_str());
        if (!h) return;
        FM2KSharedMemData* data = static_cast<FM2KSharedMemData*>(
            MapViewOfFile(h, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0,
                          sizeof(FM2KSharedMemData)));
        if (data && data->magic == FM2K_SHARED_MEM_MAGIC &&
            data->version == FM2K_SHARED_MEM_VERSION)
        {
            data->ui_wins      = hs.my_wins;
            data->ui_losses    = hs.my_losses;
            data->ui_draws     = hs.my_draws;
            data->ui_vs_wins   = vs_w;
            data->ui_vs_losses = vs_l;
            data->ui_vs_draws  = vs_d;
            auto stash = [](char* dst, size_t cap, const std::string& src) {
                if (cap == 0) return;
                size_t n = std::min<size_t>(src.size(), cap - 1);
                if (n) memcpy(dst, src.data(), n);
                dst[n] = '\0';
            };
            stash(data->ui_peer_nick, sizeof(data->ui_peer_nick),
                  hs.current_match_peer_nick);
            stash(data->ui_my_nick,   sizeof(data->ui_my_nick),
                  hs.my_nick);
        }
        if (data) UnmapViewOfFile(data);
        CloseHandle(h);
    };
    write(pid1);
    write(pid2);
}

void LauncherUI::PushHudSystemMessage(const char* text_utf8, uint32_t ttl_ms) {
    if (!on_get_client_status || !text_utf8) return;
    uint32_t pid1 = 0, pid2 = 0;
    if (!on_get_client_status(pid1, pid2)) return;

    // Mirrors PushStatsToHook's open-write-close pattern. Bumps the
    // HUD seq AFTER the buffer + expiry land, so a fc_hud read that
    // races us only ever sees a coherent {message, expiry, seq}
    // snapshot.
    auto write = [&](uint32_t pid) {
        if (pid == 0) return;
        const std::string name = "FM2K_SharedMem_" + std::to_string(pid);
        HANDLE h = OpenFileMappingA(FILE_MAP_READ | FILE_MAP_WRITE,
                                    FALSE, name.c_str());
        if (!h) return;
        FM2KSharedMemData* data = static_cast<FM2KSharedMemData*>(
            MapViewOfFile(h, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0,
                          sizeof(FM2KSharedMemData)));
        if (data && data->magic == FM2K_SHARED_MEM_MAGIC &&
            data->version == FM2K_SHARED_MEM_VERSION)
        {
            const size_t cap = sizeof(data->hud_system_message);
            const size_t n = std::min<size_t>(std::strlen(text_utf8), cap - 1);
            if (n) std::memcpy(data->hud_system_message, text_utf8, n);
            data->hud_system_message[n] = '\0';
            data->hud_system_message_expiry_tick = GetTickCount() + ttl_ms;
            data->hud_system_message_seq += 1;
        }
        if (data) UnmapViewOfFile(data);
        CloseHandle(h);
    };
    write(pid1);
    write(pid2);
}

void LauncherUI::NotifyHubMatchEnded() {
    if (!hub_state_) return;
    if (!hub_state_->client.IsConnected()) return;
    hub_state_->client.MatchEnded();
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Hub: signaled match_ended (game terminated)");
}

void LauncherUI::PollMatchOutcome() {
    if (!hub_state_) return;
    auto& hs = *hub_state_;

    // Push the latest stats (peer nick + W-L-D) to the spawned game's
    // shared mem on every poll. The MatchStart-time push fires BEFORE
    // the game process has created its FM2K_SharedMem mapping, so that
    // initial write silently no-ops. Re-pushing here lands the data as
    // soon as the mapping appears (typically within ~1s of game spawn),
    // so the in-game titlebar shows "vs <peer> 0-0-0" on the FIRST
    // match instead of waiting until match #2's K::MatchStart fires.
    // Cheap when no game is running (on_get_client_status returns 0).
    if (!hs.current_match_token.empty()) {
        PushStatsToHook();
    }

    // Nothing more to report if there's no live hub-driven match or
    // the hub dropped. NOTE: we DON'T early-return on match_result_sent
    // anymore — FM2K rematches stay inside the same hub `in_match`
    // session (no fresh K::MatchStart fires), so the second match's
    // outcome would be permanently swallowed if we gated on a flag
    // that's set true after match #1 ends. Per-PID last_outcome_seq
    // tracking already handles the "have we processed THIS bump"
    // dedup. match_result_sent is now only used by the K::Peer-
    // Disconnected handler to skip a redundant send.
    if (hs.current_match_token.empty()) return;
    if (!hs.client.IsConnected()) {
        // One-shot diagnostic: log why we're bailing the FIRST time
        // we'd otherwise have sent a match_result. Helps surface the
        // case where the hub WS died mid-match — without this we just
        // silently drop the result and the hub never commits.
        static bool s_warned_disconnect = false;
        if (!s_warned_disconnect) {
            s_warned_disconnect = true;
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "PollMatchOutcome: hub WS disconnected — match_result will NOT be sent");
        }
        return;
    }
    if (!on_get_client_status) return;

    uint32_t pid1 = 0, pid2 = 0;
    if (!on_get_client_status(pid1, pid2)) return;

    auto try_pid = [&](uint32_t pid) {
        // Don't gate on hs.match_result_sent. Per-PID last_outcome_seq
        // already dedupes (line below), and after match #1 commits,
        // match_result_sent stays true through the rematch — which
        // means the post-match DISCONNECT outcome (peer closed window
        // mid-CSS) would never get processed → on_session_stop never
        // fires → survivor's game stays open with no opponent.
        if (pid == 0) return;

        const std::string name = "FM2K_SharedMem_" + std::to_string(pid);
        HANDLE h = OpenFileMappingA(FILE_MAP_READ, FALSE, name.c_str());
        if (!h) return;

        FM2KSharedMemData* data = static_cast<FM2KSharedMemData*>(
            MapViewOfFile(h, FILE_MAP_READ, 0, 0, sizeof(FM2KSharedMemData)));
        if (!data) {
            CloseHandle(h);
            return;
        }

        if (data->magic == FM2K_SHARED_MEM_MAGIC &&
            data->version == FM2K_SHARED_MEM_VERSION)
        {
            const uint32_t seq        = data->match_outcome_seq;
            const uint8_t  outcome_u8 = data->match_outcome;
            const uint32_t p1_char_id = data->match_p1_char_id;
            const uint32_t p2_char_id = data->match_p2_char_id;
            const uint32_t stage_id   = data->match_stage_id;
            // Resolved .player filenames (UTF-8 already, hook converts
            // from CP932). Bound the strnlen at the shared-mem buffer
            // size so a malformed publish (no NUL) can't walk off the
            // mapping.
            std::string p1_char_name(
                data->match_p1_char_name,
                strnlen(data->match_p1_char_name, FM2K_MATCH_CHAR_NAME_MAX));
            std::string p2_char_name(
                data->match_p2_char_name,
                strnlen(data->match_p2_char_name, FM2K_MATCH_CHAR_NAME_MAX));
            // Prefer KGT-derived display names over the hook's shared-mem
            // values. The hook publishes filenames (from g_char_slot_data
            // / g_player_file_name_array, e.g. "c1.player" → "c1") which
            // for some games (vanpri) are placeholders, while the actual
            // display name lives inside each .player file's header. The
            // launcher's parser already enriches kgt.player_names from
            // those .player headers, so when local KGT resolves to a
            // different (non-filename) name, that's the one to ship.
            if (on_resolve_char_name) {
                if (p1_char_id != 0xFFFFFFFFu) {
                    std::string n = on_resolve_char_name(
                        hs.current_match_game_id, p1_char_id);
                    if (!n.empty()) p1_char_name = std::move(n);
                }
                if (p2_char_id != 0xFFFFFFFFu) {
                    std::string n = on_resolve_char_name(
                        hs.current_match_game_id, p2_char_id);
                    if (!n.empty()) p2_char_name = std::move(n);
                }
            }
            // Stage name resolved from local KGT (FM2K has no in-memory
            // stage filename table; the launcher already parsed the .kgt
            // header at discovery). Empty when game isn't installed
            // locally — hub will store id-only in that case.
            std::string stage_name;
            if (stage_id != 0xFFFFFFFFu && on_resolve_stage_name) {
                stage_name = on_resolve_stage_name(hs.current_match_game_id,
                                                   stage_id);
            }

            // Fire match_progress to the hub when the hook bumps its
            // chars_seq counter (which it does exactly once per
            // Netplay_StartBattleSession). Gating on seq advance —
            // not on the chars themselves — sidesteps the rotate-
            // window race where shared mem still holds the prev
            // battle's chars at the time we set current_match_token
            // to the rotated value. During CSS / inter-battle, the
            // counter doesn't advance, no fire happens, and the
            // lobby's empty "(in CSS)" row stays put until the new
            // battle actually starts.
            const uint32_t chars_seq = data->match_chars_seq;
            if (!hs.current_match_token.empty() &&
                chars_seq != 0 &&
                chars_seq != hs.last_chars_seq[pid])
            {
                hs.client.ReportMatchProgress(
                    hs.current_match_token,
                    p1_char_id, p2_char_id,
                    p1_char_name, p2_char_name,
                    stage_id, stage_name);
                hs.last_chars_seq[pid] = chars_seq;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Hub: match_progress sent token=%.8s... "
                    "p1=%u(\"%s\") p2=%u(\"%s\") stage=%u(\"%s\") "
                    "chars_seq=%u",
                    hs.current_match_token.c_str(),
                    p1_char_id, p1_char_name.c_str(),
                    p2_char_id, p2_char_name.c_str(),
                    stage_id, stage_name.c_str(),
                    (unsigned)chars_seq);
            }
            const uint32_t last_seen  = hs.last_outcome_seq[pid];

            if (seq > last_seen && outcome_u8 != FM2K_MATCH_OUTCOME_NONE) {
                hs.last_outcome_seq[pid] = seq;

                // CSS_ABORT: peer left during char select before battle
                // started. Close the surviving local game but DON'T
                // record anything — the match never reached battle, so
                // there's no W/L/D to commit. No CSV row, no hub
                // MatchResult, no in-flight commit.
                if (outcome_u8 == FM2K_MATCH_OUTCOME_CSS_ABORT) {
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Hub: peer left during CSS — closing local game (no record)");
                    hs.status_line = "peer left during CSS — match not recorded";
                    if (on_session_stop) on_session_stop();
                    // Clear so subsequent broadcasts of the same token
                    // (e.g. a match_rotated arriving from the hub) don't
                    // re-trigger this.
                    hs.current_match_token.clear();
                    hs.current_match_peer_id.clear();
                    hs.current_match_peer_nick.clear();
                    UnmapViewOfFile(data);
                    CloseHandle(h);
                    return;
                }

                // Hash mismatch: peers' game files diverge. Closes the
                // local session and surfaces a clear toast pointing the
                // user at the hook log so they can identify the
                // offending file. Same no-record treatment as CSS_ABORT —
                // no battle, no W/L/D delta.
                if (outcome_u8 == FM2K_MATCH_OUTCOME_HASH_MISMATCH) {
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Hub: GAME DATA MISMATCH — see launcher popup for "
                        "the per-file manifest.");
                    hs.status_line = "game files don't match peer";
                    // Read this game's hook log and extract the
                    // "GameHash: manifest" block so the popup can
                    // render the actual filename | size | content-hash
                    // rows that fed the local hash. The user diffs
                    // against the peer's log (which they get over
                    // Discord) to find the offending row.
                    // Player index: 0 for host, 1 for guest. Drives the
                    // hook log filename (FM2K_P1 vs FM2K_P2).
                    const int player_index =
                        network_config_.is_host ? 0 : 1;
                    std::filesystem::path game_exe;
                    if (selected_game_index_ >= 0 &&
                        selected_game_index_ < (int)games_.size()) {
                        game_exe = games_[selected_game_index_].exe_path;
                    }
                    hs.hash_mismatch_log_excerpt =
                        ExtractGameHashManifest(game_exe, player_index);
                    hs.show_hash_mismatch_modal = true;
                    FireSystemNotification(
                        T("toast_hash_mismatch_title"),
                        T("toast_hash_mismatch_body"));
                    if (on_session_stop) on_session_stop();
                    hs.current_match_token.clear();
                    hs.current_match_peer_id.clear();
                    hs.current_match_peer_nick.clear();
                    UnmapViewOfFile(data);
                    CloseHandle(h);
                    return;
                }

                const char* outcome_str = nullptr;
                switch (outcome_u8) {
                    case FM2K_MATCH_OUTCOME_SELF_WON:   outcome_str = "self_won"; break;
                    case FM2K_MATCH_OUTCOME_PEER_WON:   outcome_str = "peer_won"; break;
                    case FM2K_MATCH_OUTCOME_DRAW:       outcome_str = "draw";     break;
                    case FM2K_MATCH_OUTCOME_DISCONNECT: outcome_str = "disconnect"; break;
                    default: break;
                }
                if (outcome_str) {
                    // Local CSV mirror first — runs even if the hub
                    // send queues silently because we're disconnected.
                    // Same data, written to %APPDATA%\FM2K_Rollback\
                    // results.csv. CCCaster-equivalent for offline
                    // history.
                    AppendResultsCsvRow(outcome_str,
                                        p1_char_id, p2_char_id,
                                        p1_char_name, p2_char_name);

                    hs.client.MatchResult(hs.current_match_token, outcome_str,
                                          p1_char_id, p2_char_id,
                                          p1_char_name, p2_char_name,
                                          stage_id, stage_name);
                    hs.match_result_sent = true;
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Hub: match_result sent token=%.8s... outcome=%s "
                        "p1=%u(\"%s\") p2=%u(\"%s\") stage=%u(\"%s\") pid=%lu seq=%u",
                        hs.current_match_token.c_str(), outcome_str,
                        p1_char_id, p1_char_name.c_str(),
                        p2_char_id, p2_char_name.c_str(),
                        stage_id, stage_name.c_str(),
                        (unsigned long)pid, (unsigned)seq);
                    // Refresh our cached W/L/D so the UI updates
                    // immediately for the next room render. Also
                    // refresh the recent-matches list so the just-
                    // committed match shows up at the top.
                    hs.client.QueryRecord();
                    hs.client.RequestRecentMatches(50);

                    // Peer-disconnect: tear down the surviving local
                    // game so the user isn't stuck staring at a frozen
                    // CSS / battle screen with no opponent. Visible
                    // toast so the user knows WHY the window is
                    // closing instead of treating it as a crash.
                    if (outcome_u8 == FM2K_MATCH_OUTCOME_DISCONNECT) {
                        const std::string& peer_nick =
                            hs.current_match_peer_nick.empty()
                                ? std::string("Opponent")
                                : hs.current_match_peer_nick;
                        char body[160];
                        std::snprintf(body, sizeof(body),
                                      T("toast_peer_disconnected_body"),
                                      peer_nick.c_str());
                        hs.status_line = std::string("peer disconnected — closing match");
                        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "Hub: peer dropped, stopping local session");
                        if (!hs.disconnect_toast_fired) {
                            hs.disconnect_toast_fired = true;
                            FireSystemNotification(
                                T("toast_peer_disconnected_title"),
                                body);
                            // 5-second in-game centered overlay so the
                            // user sees why their match ended without
                            // alt-tabbing to the launcher's toast.
                            PushHudSystemMessage(
                                T("toast_peer_disconnected_title"), 5000);
                        }
                        if (on_session_stop) on_session_stop();
                    }
                }
            }
        }

        UnmapViewOfFile(data);
        CloseHandle(h);
    };

    try_pid(pid1);
    try_pid(pid2);
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
                // Pull our own W/L/D + per-opponent breakdown so the
                // lobby column and titlebar both have data on first
                // render. Refreshed after every match end via the same
                // QueryRecord call in PollMatchOutcome.
                hs.client.QueryRecord();
                // Pre-load the recent-matches panel so it isn't blank
                // the first time the user opens it. 50 rows is the
                // hub's default cap; anything more is a "history page"
                // which we don't ship yet.
                hs.client.RequestRecentMatches(50);
                // Snapshot current in-flight matches for the lobby
                // panel. Live updates arrive via MatchInProgress*
                // broadcasts after this point.
                hs.client.RequestCurrentMatches();
                // Auto-rejoin: if we were in a room when the hub
                // dropped (hub restart, network blip), put us back
                // in. last_room_id is set by the Disconnected handler
                // before it clears current_room_id. The hub re-creates
                // the room on demand if it's not seeded, so the name
                // we pass is purely cosmetic — fall back to id when we
                // don't have a separately-cached display name.
                if (!hs.last_room_id.empty()) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Hub: auto-rejoining room '%s' after reconnect",
                        hs.last_room_id.c_str());
                    const std::string& name = hs.last_room_name.empty()
                        ? hs.last_room_id : hs.last_room_name;
                    hs.client.JoinRoom(hs.last_room_id, name);
                    hs.last_room_id.clear();
                    hs.last_room_name.clear();
                }
                break;
            case K::Disconnected:
                // Stash the room we were in so the next K::Connected
                // event can auto-rejoin (hub restart shouldn't kick
                // users back to the game-picker).
                if (!hs.current_room_id.empty()) {
                    hs.last_room_id = hs.current_room_id;
                    // Look up the display name from the cached rooms
                    // list so the rejoin call carries something
                    // sensible if the hub doesn't seed this room.
                    for (const auto& r : hs.rooms) {
                        if (r.id == hs.current_room_id) {
                            hs.last_room_name = r.name;
                            break;
                        }
                    }
                }
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
                        + fm2k::utf8path::StemUtf8(
                            std::filesystem::path(
                                fm2k::utf8path::Utf8ToWide(games_[idx].exe_path)));
                } else {
                    hs.status_line = "joined room '" + hs.current_room_id +
                        "' — game not in your library, install it before challenging";
                }
                break;
            }
            case K::RoomLeft:
                hs.current_room_id.clear();
                // Explicit leave — clear the auto-rejoin snapshot so a
                // subsequent hub disconnect doesn't drag us back into
                // a room we just left.
                hs.last_room_id.clear();
                hs.last_room_name.clear();
                hs.users.clear();
                break;
            case K::UserJoined:
                if (ev.room_id == hs.current_room_id) hs.users[ev.user.id] = ev.user;
                break;
            case K::UserLeft:
                if (ev.room_id == hs.current_room_id) hs.users.erase(ev.user_id);
                break;
            case K::UserStatus: {
                // Fast peer-abort detection: when our match peer's hub
                // status TRANSITIONS from in_match to idle while we
                // haven't reported our own match_result yet (i.e. peer
                // closed window / Alt-F4'd before the match concluded
                // normally), tear down the local session immediately.
                // Without this we'd wait for the in-game GekkoNet
                // timeout (~5s) before the hook publishes DISCONNECT.
                //
                // Two guards stop the spam:
                //   - Compare against the PREVIOUSLY-cached status —
                //     fire only on the in_match → !in_match edge, not
                //     on every periodic re-broadcast of "idle".
                //   - hs.match_result_sent — at normal match end the
                //     peer also goes idle, but we already sent our own
                //     outcome, so don't double-handle that as an abort.
                std::string prev_status;
                if (auto it = hs.users.find(ev.user.id); it != hs.users.end()) {
                    prev_status = it->second.status;
                }
                hs.users[ev.user.id] = ev.user;

                const bool is_match_peer =
                    !hs.current_match_peer_id.empty() &&
                    ev.user.id == hs.current_match_peer_id;
                const bool transitioned_out =
                    prev_status == "in_match" &&
                    ev.user.status != "in_match";
                const bool peer_aborted =
                    is_match_peer && transitioned_out &&
                    !hs.current_match_token.empty() &&
                    !hs.match_result_sent;

                if (peer_aborted) {
                    const std::string& peer_nick =
                        hs.current_match_peer_nick.empty()
                            ? std::string("Opponent")
                            : hs.current_match_peer_nick;
                    char body[160];
                    std::snprintf(body, sizeof(body),
                                  T("toast_peer_disconnected_body"),
                                  peer_nick.c_str());
                    hs.status_line = "peer left match — closing local game";
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                                "Hub: %s (peer status=%s)",
                                hs.status_line.c_str(),
                                ev.user.status.c_str());
                    if (!hs.disconnect_toast_fired) {
                        hs.disconnect_toast_fired = true;
                        FireSystemNotification(
                            T("toast_peer_disconnected_title"), body);
                        PushHudSystemMessage(
                            T("toast_peer_disconnected_title"), 5000);
                    }
                    // No MatchResult here. This branch fires when the
                    // peer's hub status flipped off in_match WITHOUT us
                    // having sent our own match_result yet — meaning
                    // battle hadn't ended (we'd have already published
                    // and recorded otherwise). CSS-phase aborts must not
                    // count toward W/L/D. The hub's in-flight match
                    // sweeps cleanly via the "ambiguous, drop" branch
                    // when both peers go silent without reports.
                    // match_result_sent stays false; the hook's
                    // CSS_ABORT path (if it fires before this) already
                    // cleared current_match_token, and we clear again
                    // here so re-broadcasts don't re-trigger.
                    if (on_session_stop) on_session_stop();
                    // Clear so a re-broadcast of the same idle status
                    // can't re-trigger this branch.
                    hs.current_match_token.clear();
                    hs.current_match_peer_id.clear();
                    hs.current_match_peer_nick.clear();
                }
                break;
            }
            case K::UserRtt:
                if (auto it = hs.users.find(ev.user_id); it != hs.users.end()) {
                    it->second.rtt_ms = ev.rtt_ms;
                }
                break;
            case K::ChallengeReceived:
                hs.pending_challenge_from_id   = ev.challenge.from_id;
                hs.pending_challenge_from_nick = ev.challenge.from_nick;
                hs.pending_challenge_settings  = ev.challenge.settings;
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
                // Match is on — drop both modals (incoming and outgoing)
                // and clear any pending challenge state on both sides.
                // Without clearing the incoming modal here, the
                // accepter sees their challenge dialog persist after
                // accept (it's normally dismissed by the click handler,
                // but kb-shortcut accepts or hub-side timeouts can
                // leave it visible).
                hs.show_outgoing_challenge_modal = false;
                hs.outgoing_challenge_to_id.clear();
                hs.outgoing_challenge_to_nick.clear();
                hs.show_challenge_modal = false;
                hs.pending_challenge_from_id.clear();
                hs.pending_challenge_from_nick.clear();
                // Cancel any pending taskbar flash from a prior
                // FireChallengeNotification / FireSystemNotification
                // call. FLASHW_TIMERNOFG only auto-stops when the
                // window comes to the foreground; if the user accepted
                // via the modal without focusing the launcher first
                // (e.g. clicked through Discord's notification toast),
                // the flash keeps blinking forever. FLASHW_STOP forces
                // it off explicitly. No-op if nothing was flashing.
                if (window_) {
                    HWND hwnd = (HWND)SDL_GetPointerProperty(
                        SDL_GetWindowProperties(window_),
                        SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
                    if (hwnd) {
                        FLASHWINFO fi = { sizeof(fi), hwnd,
                                          FLASHW_STOP, 0, 0 };
                        FlashWindowEx(&fi);
                    }
                }
                // Remember the match token so the shared-mem outcome
                // poll can correlate the hook's report with the hub's
                // match record. Cleared after we send match_result.
                hs.current_match_token    = ev.match.token;
                hs.current_match_role     = ev.match.role;
                hs.current_match_peer_id  = ev.match.peer.id;
                hs.current_match_peer_nick= ev.match.peer.nick;
                hs.current_match_game_id  = hs.current_room_id;
                hs.current_match_settings = ev.match.settings;
                hs.match_result_sent      = false;
                hs.disconnect_toast_fired = false;
                hs.last_outcome_seq.clear();
                // Reset chars_seq tracking — fresh game spawn means a
                // fresh shared-mem mapping with seq=0; first
                // Netplay_StartBattleSession will bump to 1 and fire
                // match_progress against this token.
                hs.last_chars_seq.clear();
                // Push the freshly-set peer nick + cached vs record into
                // the spawned game's shared mem. The hook reads from
                // there to fill the in-game titlebar; without this, the
                // first time the game window renders post-MatchStart it
                // would show "vs <empty>" until the next K::RecordReceived.
                PushStatsToHook();
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

                    // Per-player local SOCD mode. The hook reads
                    // FM2K_SOCD_MODE on first GetSOCDMode() call;
                    // we pick the local slot's value (host == P1,
                    // guest == P2) since this game process is the
                    // local user. Each peer keeps its own mode so
                    // they don't desync across modes.
                    if (!socd_state_loaded_) {
                        socd_state_loaded_ = true;
                        LoadSocdState();
                    }
                    {
                        const int local_slot = (ev.match.role == "host") ? 0 : 1;
                        char socd_buf[8];
                        std::snprintf(socd_buf, sizeof(socd_buf), "%d",
                                      socd_mode_[local_slot]);
                        ::SetEnvironmentVariableA("FM2K_SOCD_MODE", socd_buf);
                    }
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
                    cfg.remote_address =
                        peer_ip + ":" + std::to_string(peer_port);

                    // Plumb the hub-authoritative match_settings into
                    // env vars so the launcher's StartOnlineSession
                    // path applies them to game.ini before spawn (#54)
                    // and the hook reads random-stage params from env
                    // (#56). Both peers see identical values from the
                    // hub, so they spawn with identical configs and
                    // run the same xorshift sequence on rematches.
                    const auto& s = ev.match.settings;
                    auto set_env = [](const char* k, int v) {
                        if (v == -1) { ::SetEnvironmentVariableA(k, nullptr); return; }
                        char buf[32]; std::snprintf(buf, sizeof(buf), "%d", v);
                        ::SetEnvironmentVariableA(k, buf);
                    };
                    set_env("FM2K_GP_PLAYER0_CPU",      s.player0_cpu);
                    set_env("FM2K_GP_PLAYER1_CPU",      s.player1_cpu);
                    set_env("FM2K_GP_GAME_SPEED",       s.game_speed);
                    set_env("FM2K_GP_HIT_JUDGE",        s.hit_judge);
                    set_env("FM2K_GP_GAME_INFO",        s.game_information);
                    set_env("FM2K_GP_STAGE_NB",         s.stage_nb);
                    set_env("FM2K_GP_JOYSTICK",         s.joystick);
                    set_env("FM2K_GP_TIME",             s.time);
                    set_env("FM2K_GP_VS_MODE",          s.vs_mode);
                    set_env("FM2K_GP_VS_SINGLE_PLAY",   s.vs_single_play);
                    set_env("FM2K_GP_VS_TEAM_PLAY",     s.vs_team_play);
                    if (s.random_seed != 0) {
                        char buf[32];
                        std::snprintf(buf, sizeof(buf), "%u",
                                      (unsigned)s.random_seed);
                        ::SetEnvironmentVariableA("FM2K_STAGE_RANDOM_SEED", buf);
                        set_env("FM2K_STAGE_RANDOM_MIN", s.random_stage_min);
                        set_env("FM2K_STAGE_RANDOM_MAX", s.random_stage_max);
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Random-stage: ENABLED seed=%u range=%d..%d "
                            "(env vars set on launcher process; child game "
                            "inherits these on CreateProcess)",
                            (unsigned)s.random_seed,
                            s.random_stage_min, s.random_stage_max);
                    } else {
                        ::SetEnvironmentVariableA("FM2K_STAGE_RANDOM_SEED", nullptr);
                        ::SetEnvironmentVariableA("FM2K_STAGE_RANDOM_MIN",  nullptr);
                        ::SetEnvironmentVariableA("FM2K_STAGE_RANDOM_MAX",  nullptr);
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Random-stage: DISABLED (host's match_settings "
                            "carried random_seed=0 — host hasn't enabled the "
                            "Random Stage toggle, or the wire dropped it)");
                    }

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
            case K::MatchRotated: {
                // Hub minted a fresh in-flight token after the previous
                // match committed. FM2K rematches stay inside the same
                // hub_session (peers loop CSS → battle → CSS without
                // respawning the game), so the next outcome publish
                // would otherwise be sent under the OLD (already-
                // committed) token and silently dropped by the hub.
                // Update token + reset match_result_sent. No game spawn —
                // we're just relabeling the in-flight match.
                //
                // Critically: do NOT clear last_outcome_seq. The hook's
                // match_outcome_seq monotonically increments across all
                // matches in the same process. Clearing the launcher's
                // last_seen would re-trigger PollMatchOutcome on the
                // already-processed seq, sending the previous match's
                // outcome under the rotated token → infinite commit/
                // rotate loop. The hook bumps seq on the NEXT match's
                // outcome publish, which correctly compares > the
                // preserved last_seen.
                hs.current_match_token = ev.match.token;
                hs.match_result_sent   = false;
                hs.disconnect_toast_fired = false;
                // Don't touch last_chars_seq — the same hook (same
                // PID) keeps incrementing the seq across rotates.
                // The next Netplay_StartBattleSession will bump it,
                // PollMatchOutcome will see seq advance, and fire
                // match_progress under the new token. Clearing
                // here would re-fire the prev battle's chars under
                // the new token.
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Hub: match rotated, new token=%.8s...",
                            hs.current_match_token.c_str());
                break;
            }
            case K::PeerDisconnected: {
                // Hub-side: peer's WebSocket dropped. The hook on the
                // surviving instance will *usually* notice via GekkoNet
                // peer-timeout and publish a DISCONNECT outcome, but we
                // can't always count on that (e.g. peer was idle in CSS
                // and the GekkoNet session is between matches). Stop
                // the local session here too so the survivor doesn't
                // hang on the menu screen waiting for someone who's
                // gone. Idempotent — second StopSession is a no-op.
                const std::string& peer_nick =
                    hs.current_match_peer_nick.empty()
                        ? std::string("Opponent")
                        : hs.current_match_peer_nick;
                char body[160];
                std::snprintf(body, sizeof(body),
                              T("toast_peer_disconnected_body"),
                              peer_nick.c_str());
                hs.status_line = "peer disconnected — closing match";
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "Hub: %s", hs.status_line.c_str());
                if (!hs.disconnect_toast_fired) {
                    hs.disconnect_toast_fired = true;
                    FireSystemNotification(T("toast_peer_disconnected_title"), body);
                    PushHudSystemMessage(
                        T("toast_peer_disconnected_title"), 5000);
                }
                if (on_session_stop) on_session_stop();
                // Best-effort match_result so the hub closes its
                // in-flight record. If we never had a current_match_token
                // (e.g. peer dropped before MatchStart fired), this is
                // a no-op on the hub side — match_id won't correlate.
                if (!hs.current_match_token.empty() && !hs.match_result_sent) {
                    hs.client.MatchResult(hs.current_match_token, "disconnect");
                    hs.match_result_sent = true;
                }
                break;
            }
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
            case K::RecordReceived: {
                // Only the unfiltered global-record reply (no opponent_id /
                // game_id filter) carries the per-opponent breakdown that
                // populates the lobby column. Filtered queries from other
                // call sites (per-game tab, per-opponent tooltip) overwrite
                // their own narrower views and shouldn't clobber the
                // overall numbers.
                if (ev.record.user_id == hs.my_id &&
                    ev.record.opponent_id.empty() &&
                    ev.record.game_id.empty())
                {
                    hs.my_wins   = ev.record.wins;
                    hs.my_losses = ev.record.losses;
                    hs.my_draws  = ev.record.draws;
                    hs.my_vs.clear();
                    for (auto& row : ev.record.vs_breakdown) {
                        if (row.opponent_id.empty()) continue;
                        hs.my_vs[row.opponent_id] = {row.wins, row.losses, row.draws};
                    }
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Hub: record W-L-D = %d-%d-%d (vs %u opponents)",
                        hs.my_wins, hs.my_losses, hs.my_draws,
                        (unsigned)hs.my_vs.size());
                    UpdateWindowTitleWithRecord();
                    // Push to the in-game shared mem so the game window
                    // titlebar (and later the overlay) can render the
                    // updated W/L/D without an alt-tab to the launcher.
                    PushStatsToHook();
                }
                break;
            }
            case K::RecentMatchesReceived:
                hs.recent_matches        = ev.recent_matches;
                hs.recent_matches_loaded = true;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Hub: cached %u recent matches",
                            (unsigned)hs.recent_matches.size());
                break;
            case K::CurrentMatchesReceived:
                hs.current_matches        = ev.current_matches;
                hs.current_matches_loaded = true;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Hub: cached %u in-progress matches",
                            (unsigned)hs.current_matches.size());
                break;
            case K::MatchInProgressStarted:
            case K::MatchInProgressUpdated: {
                // Replace existing token entry or append. Keeps the list
                // in last-write-wins state without rebuilding from a
                // fresh snapshot for every update.
                const auto& upd = ev.current_match_update;
                bool replaced = false;
                for (auto& m : hs.current_matches) {
                    if (m.token == upd.token) {
                        m = upd;
                        replaced = true;
                        break;
                    }
                }
                if (!replaced) hs.current_matches.push_back(upd);
                break;
            }
            case K::MatchInProgressEnded: {
                const std::string& tok = ev.current_match_token;
                hs.current_matches.erase(
                    std::remove_if(hs.current_matches.begin(),
                                   hs.current_matches.end(),
                                   [&](const auto& m) { return m.token == tok; }),
                    hs.current_matches.end());
                break;
            }
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
        std::filesystem::path exe(fm2k::utf8path::Utf8ToWide(g.exe_path));
        std::string game_id = fm2k::utf8path::StemUtf8(exe);
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

    // ---- Live in-progress matches across the hub ----
    RenderInProgressMatchesBody();

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
        if (ImGui::BeginTable("##users", 5,
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
            ImGui::TableSetupColumn(T("col_nick"));
            ImGui::TableSetupColumn(T("col_status"), ImGuiTableColumnFlags_WidthFixed, 110.0f);
            ImGui::TableSetupColumn(T("col_ping"),   ImGuiTableColumnFlags_WidthFixed, 60.0f);
            // "vs" column — my W-L-D against this opponent, "—" if we've
            // never played them. Self-row leaves it blank.
            ImGui::TableSetupColumn(T("col_vs"),     ImGuiTableColumnFlags_WidthFixed, 90.0f);
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
                if (is_self) {
                    // Self row: show overall global W/L/D in this column
                    // instead of an "vs me" cell that doesn't make sense.
                    if (hs.my_wins >= 0) {
                        ImGui::Text("%d-%d-%d", hs.my_wins, hs.my_losses, hs.my_draws);
                    } else {
                        ImGui::TextDisabled("—");
                    }
                } else {
                    auto it = hs.my_vs.find(uid);
                    if (it != hs.my_vs.end()) {
                        ImGui::Text("%d-%d-%d",
                                    it->second.wins, it->second.losses, it->second.draws);
                    } else {
                        ImGui::TextDisabled("—");
                    }
                }

                ImGui::TableSetColumnIndex(4);
                ImGui::PushID(uid.c_str());
                // Self-row shows nothing in the action column — challenging
                // yourself isn't a thing. Other rows get the Challenge button
                // gated on idle status.
                if (!is_self) {
                    bool can_challenge = (u.status == "idle");
                    if (!can_challenge) ImGui::BeginDisabled();
                    if (ImGui::SmallButton(T("btn_challenge"))) {
                        // Build the host's resolved [GamePlay] config
                        // for THIS challenge so the target sees the
                        // round count / time / stage / etc. in their
                        // accept modal (#54). Anti-cheat clamps land
                        // launcher-side before the wire encode so the
                        // target can't see un-clamped values.
                        fm2k::MatchSettings ms;
                        if (selected_game_index_ >= 0 &&
                            selected_game_index_ < (int)games_.size())
                        {
                            const auto& g = games_[selected_game_index_];
                            fm2k::game_ini::GamePlayConfig cfg;
                            fm2k::game_ini::LoadResolved(g.exe_path, cfg);
                            fm2k::game_ini::ForceOnlineClamps(cfg);
                            ms.player0_cpu      = cfg.player0_cpu;
                            ms.player1_cpu      = cfg.player1_cpu;
                            ms.game_speed       = cfg.game_speed;
                            ms.hit_judge        = cfg.hit_judge;
                            ms.game_information = cfg.game_information;
                            ms.stage_nb         = cfg.stage_nb;
                            ms.joystick         = cfg.joystick;
                            ms.time             = cfg.time;
                            ms.exit_flag        = cfg.exit_flag;
                            ms.vs_mode          = cfg.vs_mode;
                            ms.vs_single_play   = cfg.vs_single_play;
                            ms.vs_team_play     = cfg.vs_team_play;
                        }
                        // Random-stage extension (#56). When enabled,
                        // generate a fresh xorshift seed per challenge
                        // and ship it to the peer. Both peers re-seed
                        // their hook PRNG from this same value, then
                        // run identical sequences on rematches with
                        // zero per-rematch wire traffic. Seed != 0
                        // is the wire signal "random is on" — keep
                        // a tiny rejection loop so we never accidentally
                        // hand a 0 seed.
                        if (!random_state_loaded_) {
                            random_state_loaded_ = true;
                            LoadRandomStageState();
                        }
                        if (random_stage_enable_ &&
                            random_stage_max_ >= random_stage_min_)
                        {
                            uint32_t seed = 0;
                            while (seed == 0) {
                                // 32-bit mix of two rand() bursts; not
                                // cryptographic but plenty random for
                                // a uniform stage roll.
                                seed = (uint32_t)((std::rand() & 0xFFFF) |
                                                  ((std::rand() & 0xFFFF) << 16));
                            }
                            ms.random_seed      = seed;
                            ms.random_stage_min = random_stage_min_;
                            ms.random_stage_max = random_stage_max_;
                            // Override any explicit stage_nb so the
                            // client doesn't apply both. Random takes
                            // precedence when on.
                            ms.stage_nb = -1;
                        }
                        hs.client.Challenge(uid, ms);
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

        // Match settings preview (#54). Only render if the challenger
        // actually sent any — older clients leave the whole struct at
        // -1 and we want to keep the modal compact in that case.
        const auto& s = hs.pending_challenge_settings;
        const bool any_set =
            s.player0_cpu != -1 || s.player1_cpu != -1 ||
            s.game_speed  != -1 || s.hit_judge   != -1 ||
            s.game_information != -1 || s.stage_nb != -1 ||
            s.joystick != -1 || s.time != -1 ||
            s.exit_flag != -1 || s.vs_mode != -1 ||
            s.vs_single_play != -1 || s.vs_team_play != -1;
        if (any_set) {
            ImGui::SeparatorText(T("label_match_settings"));
            if (ImGui::BeginTable("##match_settings_preview", 2,
                    ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
                auto row = [](const char* label, int v) {
                    if (v == -1) return;
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted(label);
                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text("%d", v);
                };
                row("Round count (1v1)",   s.vs_single_play);
                row("Round count (team)",  s.vs_team_play);
                row("Round timer (s)",     s.time);
                row("Game speed",          s.game_speed);
                row("Stage",               s.stage_nb);
                row("Joystick",            s.joystick);
                row("VS mode",             s.vs_mode);
                ImGui::EndTable();
            }
            ImGui::Spacing();
        }

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

    // ---- Hash-mismatch popup ----
    // Fires when the hook publishes FM2K_MATCH_OUTCOME_HASH_MISMATCH.
    // Shows the local game's "GameHash: manifest" excerpt so the user
    // can diff against the peer's log to find the offending file.
    if (hs.show_hash_mismatch_modal) {
        ImGui::OpenPopup("##hash_mismatch");
    }
    if (ImGui::BeginPopupModal("##hash_mismatch", nullptr,
                               ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::Text("Game data mismatch — match cancelled.");
        ImGui::Spacing();
        ImGui::TextWrapped(
            "Your .kgt / .player roster differs from your peer's. "
            "Below is what we hashed locally. Send this to your peer (or "
            "exchange hook logs) — the row with a different size or "
            "content_hash is the file that needs to match. Read the hook "
            "log for more details.");
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextDisabled("Local manifest (filename | size | content_hash):");
        // Scrolling read-only text region. Wide on purpose so the
        // hash columns line up.
        ImGui::InputTextMultiline(
            "##hash_mm_log",
            hs.hash_mismatch_log_excerpt.data(),
            hs.hash_mismatch_log_excerpt.size() + 1,
            ImVec2(720, 280),
            ImGuiInputTextFlags_ReadOnly);
        ImGui::Spacing();
        if (ImGui::Button("Copy to clipboard", ImVec2(160, 0))) {
            ImGui::SetClipboardText(hs.hash_mismatch_log_excerpt.c_str());
        }
        ImGui::SameLine();
        if (ImGui::Button("Close", ImVec2(120, 0))) {
            hs.show_hash_mismatch_modal = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // ---- Recent matches (collapsing) ----
    // Lives at the bottom of the Hub panel — it's session data, not a
    // configuration setting, so it doesn't belong in the Settings tabs.
    // Collapsed by default; users who care about history click to
    // expand. The body renderer is shared with the legacy floating
    // window so any styling fixes apply to both.
    ImGui::Spacing();
    if (ImGui::CollapsingHeader(T("menu_recent_matches"))) {
        RenderRecentMatchesBody();
    }
}

void LauncherUI::LoadDDrawCfgIfNeeded() {
    if (ddraw_cfg_loaded_) return;
    fm2k::cnc_ddraw::LoadIni(ddraw_cfg_);
    ddraw_cfg_loaded_ = true;
}

// Settings → Display. Mirrors every documented cnc-ddraw [ddraw] key
// from <install_dir>\ddraw.ini. Edits write per-key on change so each
// flick of a checkbox lands on disk immediately — no Apply button.
// Changes take effect on the NEXT game launch (cnc-ddraw reads its ini
// at DLL_PROCESS_ATTACH); the header label calls that out.
//
// Sectioning mirrors cnc-ddraw config.exe's tabs for familiarity:
// Window mode → Renderer → Performance → Hotkeys → Compatibility →
// Undocumented (collapsing).
void LauncherUI::RenderDisplayBody() {
    LoadDDrawCfgIfNeeded();

    namespace cd = fm2k::cnc_ddraw;
    auto& c = ddraw_cfg_;

    ImGui::TextWrapped(
        "cnc-ddraw renderer settings. Changes apply on the NEXT game launch.");
    ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f),
        "Editing %s", cd::IniPath().c_str());
    ImGui::Spacing();
    if (ImGui::Button("Reset to launcher defaults")) {
        if (cd::ResetIniToDefault()) {
            ddraw_cfg_ = cd::IniConfig{};   // back to header defaults
            cd::LoadIni(ddraw_cfg_);        // pull in baked-ini values
        }
    }
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.85f, 0.7f, 0.4f, 1.0f),
        "Wipes any per-game [<exe>] blocks");
    ImGui::Separator();

    // ── Window mode ──────────────────────────────────────────────────
    if (ImGui::CollapsingHeader("Window mode", ImGuiTreeNodeFlags_DefaultOpen)) {
        // Render mode is the (windowed, fullscreen) tuple. cnc-ddraw
        // semantics: windowed=true & fullscreen=false → real windowed,
        // both true → borderless windowed-fullscreen, fullscreen=true
        // & windowed=false → fullscreen-upscaled. Surface as a 3-way
        // combo + an "advanced" raw checkbox pair.
        const char* mode_items[] = {
            "Windowed",
            "Borderless (windowed-fullscreen)",
            "Fullscreen upscaled"
        };
        int mode_idx = 0;
        if      ( c.windowed &&  c.fullscreen) mode_idx = 1;
        else if (!c.windowed &&  c.fullscreen) mode_idx = 2;
        else                                    mode_idx = 0;
        if (ImGui::Combo("Mode", &mode_idx, mode_items, IM_ARRAYSIZE(mode_items))) {
            switch (mode_idx) {
                case 0: c.windowed = true;  c.fullscreen = false; break;
                case 1: c.windowed = true;  c.fullscreen = true;  break;
                case 2: c.windowed = false; c.fullscreen = true;  break;
            }
            cd::SaveBool("windowed",   c.windowed);
            cd::SaveBool("fullscreen", c.fullscreen);
        }

        if (ImGui::DragInt("Width  (0 = use game's)",  &c.width,  1, 0, 7680)) {
            cd::SaveInt("width",  c.width);
        }
        if (ImGui::DragInt("Height (0 = use game's)",  &c.height, 1, 0, 4320)) {
            cd::SaveInt("height", c.height);
        }

        char ar_buf[32] = {};
        std::snprintf(ar_buf, sizeof(ar_buf), "%s", c.aspect_ratio.c_str());
        if (ImGui::InputText("Aspect ratio (e.g. 4:3, 16:9, blank=auto)",
                             ar_buf, sizeof(ar_buf))) {
            c.aspect_ratio = ar_buf;
            cd::SaveString("aspect_ratio", c.aspect_ratio);
        }

        if (ImGui::Checkbox("Maintain aspect ratio", &c.maintas)) {
            cd::SaveBool("maintas", c.maintas);
        }
        ImGui::SameLine();
        if (ImGui::Checkbox("Integer scaling (boxing)", &c.boxing)) {
            cd::SaveBool("boxing", c.boxing);
        }

        if (ImGui::Checkbox("Window border (windowed mode)", &c.border)) {
            cd::SaveBool("border", c.border);
        }
        ImGui::SameLine();
        if (ImGui::Checkbox("User resizable", &c.resizable)) {
            cd::SaveBool("resizable", c.resizable);
        }

        const char* center_items[] = {
            "Never center", "Automatic", "Always center"
        };
        if (ImGui::Combo("Center on resolution change", &c.center_window,
                         center_items, IM_ARRAYSIZE(center_items))) {
            cd::SaveInt("center_window", c.center_window);
        }
        if (ImGui::DragInt("Window posX (-32000 = center)", &c.posX, 1, -32000, 32000)) {
            cd::SaveInt("posX", c.posX);
        }
        if (ImGui::DragInt("Window posY (-32000 = center)", &c.posY, 1, -32000, 32000)) {
            cd::SaveInt("posY", c.posY);
        }
        const char* save_items[] = {
            "Don't save", "Save to [ddraw]", "Save per-game [<exe>]"
        };
        if (ImGui::Combo("Save window position/size on exit", &c.savesettings,
                         save_items, IM_ARRAYSIZE(save_items))) {
            cd::SaveInt("savesettings", c.savesettings);
        }
    }

    // ── Renderer ─────────────────────────────────────────────────────
    if (ImGui::CollapsingHeader("Renderer", ImGuiTreeNodeFlags_DefaultOpen)) {
        // Renderer is locked to direct3d9: our in-game ImGui overlay
        // hooks `IDirect3DDevice9::EndScene` (FM2KHook/src/ui/imgui_overlay.cpp)
        // and would never attach if cnc-ddraw routed through OpenGL or
        // GDI. Force-write on every render of this tab so a stale
        // pre-existing ini gets corrected the moment the user opens it.
        if (c.renderer != "direct3d9") {
            c.renderer = "direct3d9";
            cd::SaveString("renderer", c.renderer);
        }
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(ImVec4(0.6f, 0.85f, 0.6f, 1.0f),
            "Renderer: direct3d9  (locked)");
        ImGui::TextWrapped(
            "Locked so the launcher's in-game ImGui overlay can attach via "
            "IDirect3DDevice9::EndScene. Edit ddraw.ini directly if you "
            "really need a different backend (opengl / gdi / direct3d9on12).");

        const char* d3d9_items[] = {
            "Nearest neighbor", "Bilinear", "Bicubic (16/32-bit only)",
            "Lanczos (16/32-bit only)"
        };
        if (ImGui::Combo("Direct3D9 upscale filter", &c.d3d9_filter,
                         d3d9_items, IM_ARRAYSIZE(d3d9_items))) {
            cd::SaveInt("d3d9_filter", c.d3d9_filter);
        }

        char shader_buf[512] = {};
        std::snprintf(shader_buf, sizeof(shader_buf), "%s", c.shader.c_str());
        if (ImGui::InputText("Shader (path or name)", shader_buf, sizeof(shader_buf))) {
            c.shader = shader_buf;
            cd::SaveString("shader", c.shader);
        }

        if (ImGui::Checkbox("VSync", &c.vsync)) {
            cd::SaveBool("vsync", c.vsync);
        }
        ImGui::SameLine();
        if (ImGui::Checkbox("Disable fullscreen-exclusive mode", &c.nonexclusive)) {
            cd::SaveBool("nonexclusive", c.nonexclusive);
        }

        char inj_buf[64] = {};
        std::snprintf(inj_buf, sizeof(inj_buf), "%s", c.inject_resolution.c_str());
        if (ImGui::InputText("Inject resolution (e.g. 960x540)",
                             inj_buf, sizeof(inj_buf))) {
            c.inject_resolution = inj_buf;
            cd::SaveString("inject_resolution", c.inject_resolution);
        }

        if (ImGui::Checkbox("vhack (high-res patches: C&C, RA1, Worms 2, KKND Xtreme)",
                            &c.vhack)) {
            cd::SaveBool("vhack", c.vhack);
        }
    }

    // ── Performance ──────────────────────────────────────────────────
    if (ImGui::CollapsingHeader("Performance / framerate")) {
        if (ImGui::DragInt("maxfps (-1=screen, 0=unlimited, n=cap)",
                           &c.maxfps, 1, -1, 1000)) {
            cd::SaveInt("maxfps", c.maxfps);
        }
        if (ImGui::DragInt("maxgameticks (-1=disabled, -2=refresh rate, 0=60Hz vblank)",
                           &c.maxgameticks, 1, -2, 1000)) {
            cd::SaveInt("maxgameticks", c.maxgameticks);
        }
        if (ImGui::DragInt("minfps (0=disabled, -1=use maxfps, -2=force redraw)",
                           &c.minfps, 1, -2, 1000)) {
            cd::SaveInt("minfps", c.minfps);
        }
        const char* limiter_items[] = {
            "Automatic", "TestCooperativeLevel", "BltFast", "Unlock", "PeekMessage"
        };
        if (ImGui::Combo("Limiter type", &c.limiter_type,
                         limiter_items, IM_ARRAYSIZE(limiter_items))) {
            cd::SaveInt("limiter_type", c.limiter_type);
        }
    }

    // ── Input ────────────────────────────────────────────────────────
    if (ImGui::CollapsingHeader("Input")) {
        if (ImGui::Checkbox("Auto mouse-sensitivity scaling (adjmouse)", &c.adjmouse)) {
            cd::SaveBool("adjmouse", c.adjmouse);
        }
        if (ImGui::Checkbox("Devmode (don't lock cursor)", &c.devmode)) {
            cd::SaveBool("devmode", c.devmode);
        }
        if (ImGui::Checkbox("hook_peekmessage (cursor-lock fix on upscaling)",
                            &c.hook_peekmessage)) {
            cd::SaveBool("hook_peekmessage", c.hook_peekmessage);
        }
    }

    // ── Hotkeys ──────────────────────────────────────────────────────
    if (ImGui::CollapsingHeader("Hotkeys")) {
        // Press-to-bind UI mirrors the input-binder pattern: click
        // "Bind", wait for the user to release any keys still held from
        // the click, then capture the next VK that goes down. Esc =
        // cancel, Backspace = clear binding to 0 (disabled).
        //
        // We poll GetAsyncKeyState across VK 0x07..0xFE (skip mouse
        // buttons 0x01..0x06 so the click isn't read back). Each
        // capture stores the resolved VK back into the IniConfig field
        // and writes through fm2k::cnc_ddraw::SaveHex so the ini
        // matches the format the cnc-ddraw stock ini ships in (0xNN).
        static const char* s_capture_key   = nullptr;  // ini key being captured
        static int*        s_capture_field = nullptr;  // pointer into ddraw_cfg_
        static bool        s_capture_armed = false;    // released since click?

        auto vk_label = [](int vk) -> std::string {
            if (vk == 0) return "(disabled)";
            UINT scan = MapVirtualKeyA((UINT)vk, MAPVK_VK_TO_VSC);
            char buf[64] = {};
            if (scan && GetKeyNameTextA((LONG)(scan << 16), buf, sizeof(buf)) > 0) {
                char out[96];
                std::snprintf(out, sizeof(out), "%s  (0x%02X)", buf, vk);
                return out;
            }
            char out[32];
            std::snprintf(out, sizeof(out), "VK 0x%02X", vk);
            return out;
        };

        auto any_key_held = []() {
            for (int vk = 0x01; vk <= 0xFE; ++vk) {
                if ((GetAsyncKeyState(vk) & 0x8000) != 0) return true;
            }
            return false;
        };

        // Drive the capture state machine — runs once per frame regardless
        // of which row's Bind button started it. Fires before we render
        // the rows so a successful capture is reflected this frame.
        if (s_capture_key && s_capture_field) {
            if (!s_capture_armed) {
                if (!any_key_held()) s_capture_armed = true;
            } else {
                // Esc cancels without writing.
                if ((GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0) {
                    s_capture_key = nullptr;
                    s_capture_field = nullptr;
                    s_capture_armed = false;
                } else if ((GetAsyncKeyState(VK_BACK) & 0x8000) != 0) {
                    // Backspace clears the binding.
                    *s_capture_field = 0;
                    cd::SaveHex(s_capture_key, 0);
                    s_capture_key = nullptr;
                    s_capture_field = nullptr;
                    s_capture_armed = false;
                } else {
                    // Skip mouse buttons 0x01..0x06 (the click that
                    // started capture would re-trigger). All other VKs
                    // are fair game.
                    for (int vk = 0x07; vk <= 0xFE; ++vk) {
                        if ((GetAsyncKeyState(vk) & 0x8000) != 0) {
                            *s_capture_field = vk;
                            cd::SaveHex(s_capture_key, vk);
                            s_capture_key = nullptr;
                            s_capture_field = nullptr;
                            s_capture_armed = false;
                            break;
                        }
                    }
                }
            }
        }

        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f),
            "Click Bind, then press the key. Esc = cancel, Backspace = clear.");

        auto hk_row = [&](const char* label, const char* ini_key, int* field) {
            ImGui::PushID(ini_key);
            const bool waiting = (s_capture_key == ini_key);
            ImGui::AlignTextToFramePadding();
            ImGui::Text("%s", label);
            ImGui::SameLine(280.0f);
            if (waiting) {
                ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1.0f),
                    "%s",
                    s_capture_armed ? "Press a key..."
                                    : "Release any held keys...");
            } else {
                ImGui::TextUnformatted(vk_label(*field).c_str());
            }
            ImGui::SameLine(460.0f);
            if (waiting) {
                if (ImGui::Button("Cancel", ImVec2(80, 0))) {
                    s_capture_key = nullptr;
                    s_capture_field = nullptr;
                    s_capture_armed = false;
                }
            } else {
                if (ImGui::Button("Bind", ImVec2(80, 0))) {
                    s_capture_key = ini_key;
                    s_capture_field = field;
                    s_capture_armed = false;  // wait for release
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Clear", ImVec2(60, 0))) {
                *field = 0;
                cd::SaveHex(ini_key, 0);
                if (waiting) {
                    s_capture_key = nullptr;
                    s_capture_field = nullptr;
                    s_capture_armed = false;
                }
            }
            ImGui::PopID();
        };

        hk_row("Toggle fullscreen (Alt+...)",  "keytogglefullscreen",  &c.keytogglefullscreen);
        hk_row("Toggle fullscreen 2 (single)", "keytogglefullscreen2", &c.keytogglefullscreen2);
        hk_row("Maximize window (Alt+...)",    "keytogglemaximize",    &c.keytogglemaximize);
        hk_row("Maximize window 2 (single)",   "keytogglemaximize2",   &c.keytogglemaximize2);
        hk_row("Unlock cursor 1 (Ctrl+...)",   "keyunlockcursor1",     &c.keyunlockcursor1);
        hk_row("Unlock cursor 2 (RAlt+...)",   "keyunlockcursor2",     &c.keyunlockcursor2);
        hk_row("Screenshot",                   "keyscreenshot",        &c.keyscreenshot);

        ImGui::Spacing();
        if (ImGui::Checkbox("Alt+Enter toggles windowed/borderless instead of fullscreen",
                            &c.toggle_borderless)) {
            cd::SaveBool("toggle_borderless", c.toggle_borderless);
        }
        if (ImGui::Checkbox("Alt+Enter toggles windowed/upscaled instead",
                            &c.toggle_upscaled)) {
            cd::SaveBool("toggle_upscaled", c.toggle_upscaled);
        }
    }

    // ── Compatibility ────────────────────────────────────────────────
    if (ImGui::CollapsingHeader("Compatibility")) {
        if (ImGui::Checkbox("Hide WM_ACTIVATEAPP/NCACTIVATE on alt-tab (noactivateapp)",
                            &c.noactivateapp)) {
            cd::SaveBool("noactivateapp", c.noactivateapp);
        }
        if (ImGui::Checkbox("Force CPU0 affinity (singlecpu)", &c.singlecpu)) {
            cd::SaveBool("singlecpu", c.singlecpu);
        }
        const char* res_items[] = {
            "Small list", "Very small list", "Full list"
        };
        if (ImGui::Combo("Available display resolutions", &c.resolutions,
                         res_items, IM_ARRAYSIZE(res_items))) {
            cd::SaveInt("resolutions", c.resolutions);
        }
        const char* fc_items[] = {
            "Disabled", "Display top-left", "Display top-left + repaint",
            "Hide", "Display top-left + hide"
        };
        if (ImGui::Combo("fixchilds (child window handling)", &c.fixchilds,
                         fc_items, IM_ARRAYSIZE(fc_items))) {
            cd::SaveInt("fixchilds", c.fixchilds);
        }
        if (ImGui::DragInt("anti_aliased_fonts_min_size",
                           &c.anti_aliased_fonts_min_size, 1, 0, 100)) {
            cd::SaveInt("anti_aliased_fonts_min_size", c.anti_aliased_fonts_min_size);
        }
        if (ImGui::DragInt("min_font_size",
                           &c.min_font_size, 1, 0, 100)) {
            cd::SaveInt("min_font_size", c.min_font_size);
        }

        char ssdir_buf[260] = {};
        std::snprintf(ssdir_buf, sizeof(ssdir_buf), "%s", c.screenshotdir.c_str());
        if (ImGui::InputText("Screenshot directory", ssdir_buf, sizeof(ssdir_buf))) {
            c.screenshotdir = ssdir_buf;
            cd::SaveString("screenshotdir", c.screenshotdir);
        }
    }

    // ── Undocumented / advanced ──────────────────────────────────────
    if (ImGui::CollapsingHeader("Advanced (undocumented — only touch if needed)")) {
        ImGui::TextColored(ImVec4(0.85f, 0.6f, 0.4f, 1.0f),
            "Per cnc-ddraw: 'These will probably not solve your problem'.");
        if (ImGui::Checkbox("fix_alt_key_stuck", &c.fix_alt_key_stuck))
            cd::SaveBool("fix_alt_key_stuck", c.fix_alt_key_stuck);
        if (ImGui::Checkbox("game_handles_close", &c.game_handles_close))
            cd::SaveBool("game_handles_close", c.game_handles_close);
        if (ImGui::Checkbox("fix_not_responding", &c.fix_not_responding))
            cd::SaveBool("fix_not_responding", c.fix_not_responding);
        if (ImGui::Checkbox("no_compat_warning", &c.no_compat_warning))
            cd::SaveBool("no_compat_warning", c.no_compat_warning);
        if (ImGui::Checkbox("lock_surfaces", &c.lock_surfaces))
            cd::SaveBool("lock_surfaces", c.lock_surfaces);
        if (ImGui::Checkbox("flipclear", &c.flipclear))
            cd::SaveBool("flipclear", c.flipclear);
        if (ImGui::Checkbox("rgb555", &c.rgb555))
            cd::SaveBool("rgb555", c.rgb555);
        if (ImGui::Checkbox("no_dinput_hook", &c.no_dinput_hook))
            cd::SaveBool("no_dinput_hook", c.no_dinput_hook);
        if (ImGui::Checkbox("center_cursor_fix", &c.center_cursor_fix))
            cd::SaveBool("center_cursor_fix", c.center_cursor_fix);
        if (ImGui::Checkbox("lock_mouse_top_left", &c.lock_mouse_top_left))
            cd::SaveBool("lock_mouse_top_left", c.lock_mouse_top_left);
        if (ImGui::Checkbox("limit_gdi_handles", &c.limit_gdi_handles))
            cd::SaveBool("limit_gdi_handles", c.limit_gdi_handles);
        if (ImGui::Checkbox("remove_menu", &c.remove_menu))
            cd::SaveBool("remove_menu", c.remove_menu);

        if (ImGui::DragInt("guard_lines", &c.guard_lines, 1, 0, 1000))
            cd::SaveInt("guard_lines", c.guard_lines);
        if (ImGui::DragInt("max_resolutions", &c.max_resolutions, 1, 0, 100))
            cd::SaveInt("max_resolutions", c.max_resolutions);
        if (ImGui::DragInt("hook (mode 1-4; default 4)", &c.hook, 1, 0, 4))
            cd::SaveInt("hook", c.hook);
        if (ImGui::DragInt("refresh_rate (0 = monitor default)",
                           &c.refresh_rate, 1, 0, 360))
            cd::SaveInt("refresh_rate", c.refresh_rate);
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

        if (ImGui::BeginTabItem("Renderer")) {
            // Redirect is on-by-default. Toggle is presented as a
            // "Disable" knob — when checked, the next launch skips the
            // IAT patch + PATH prepend and the game loads stock
            // KnownDlls ddraw.dll. Useful for diagnosing whether a
            // problem is rendering-related or game-code-related.
            bool disabled = !FM2K::ddraw_redirect::GetForceRedirect();
            if (ImGui::Checkbox("Disable cnc-ddraw renderer (debug)", &disabled)) {
                FM2K::ddraw_redirect::SetForceRedirect(!disabled);
            }
            ImGui::TextWrapped(
                "When unchecked (default): patches DDRAW.dll -> 2DFMD.dll in "
                "the game's IAT before resume and prepends the cnc-ddraw dir "
                "onto the child PATH. The cnc-ddraw dir is FM2K_DDRAW_DIR if "
                "set, otherwise <launcher>\\cnc-ddraw.");
            std::wstring resolved = FM2K::ddraw_redirect::ResolveCncDdrawDir();
            std::string resolved_utf8;
            if (!resolved.empty()) {
                int n = WideCharToMultiByte(CP_UTF8, 0, resolved.data(),
                                            (int)resolved.size(),
                                            nullptr, 0, nullptr, nullptr);
                if (n > 0) {
                    resolved_utf8.assign((size_t)n, '\0');
                    WideCharToMultiByte(CP_UTF8, 0, resolved.data(),
                                        (int)resolved.size(),
                                        resolved_utf8.data(), n,
                                        nullptr, nullptr);
                }
            }
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                "Resolved cnc-ddraw dir: %s",
                resolved_utf8.empty() ? "(unresolved)" : resolved_utf8.c_str());

            ImGui::Separator();
            ImGui::Text("cnc-ddraw install");

            // Phase C: bundled cnc-ddraw downloader/updater. Status
            // pill mirrors FM2K_Updater's idiom — labels pulled from
            // a single switch on the snapshot's State.
            const auto snap = fm2k::cnc_ddraw::Get();
            const char* state_label = "?";
            ImVec4 state_color(0.7f, 0.7f, 0.7f, 1.0f);
            switch (snap.state) {
                case fm2k::cnc_ddraw::State::Idle:
                    state_label = "Idle (not checked)"; break;
                case fm2k::cnc_ddraw::State::Checking:
                    state_label = "Checking GitHub...";
                    state_color = ImVec4(0.7f, 0.85f, 1.0f, 1.0f); break;
                case fm2k::cnc_ddraw::State::NotInstalled:
                    state_label = "Not installed";
                    state_color = ImVec4(1.0f, 0.7f, 0.4f, 1.0f); break;
                case fm2k::cnc_ddraw::State::UpToDate:
                    state_label = "Up to date";
                    state_color = ImVec4(0.5f, 0.9f, 0.5f, 1.0f); break;
                case fm2k::cnc_ddraw::State::UpdateAvailable:
                    state_label = "Update available";
                    state_color = ImVec4(1.0f, 0.85f, 0.4f, 1.0f); break;
                case fm2k::cnc_ddraw::State::Downloading: {
                    static char dl[64];
                    if (snap.total_bytes > 0) {
                        std::snprintf(dl, sizeof(dl),
                            "Downloading %u / %u KB",
                            snap.downloaded_bytes / 1024,
                            snap.total_bytes / 1024);
                    } else {
                        std::snprintf(dl, sizeof(dl),
                            "Downloading %u KB", snap.downloaded_bytes / 1024);
                    }
                    state_label = dl;
                    state_color = ImVec4(0.7f, 0.85f, 1.0f, 1.0f);
                    break;
                }
                case fm2k::cnc_ddraw::State::Extracting:
                    state_label = "Extracting...";
                    state_color = ImVec4(0.7f, 0.85f, 1.0f, 1.0f); break;
                case fm2k::cnc_ddraw::State::Ready:
                    state_label = "Installed";
                    state_color = ImVec4(0.5f, 0.9f, 0.5f, 1.0f); break;
                case fm2k::cnc_ddraw::State::Failed:
                    state_label = "Failed";
                    state_color = ImVec4(1.0f, 0.5f, 0.5f, 1.0f); break;
            }
            ImGui::TextColored(state_color, "Status: %s", state_label);
            if (!snap.local_version.empty() || !snap.remote_version.empty()) {
                ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                    "Local: %s   Remote: %s",
                    snap.local_version.empty()  ? "(none)" : snap.local_version.c_str(),
                    snap.remote_version.empty() ? "(?)"   : snap.remote_version.c_str());
            }
            if (snap.state == fm2k::cnc_ddraw::State::Failed && !snap.error_detail.empty()) {
                ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f),
                    "Error: %s", snap.error_detail.c_str());
            }

            const bool busy = snap.state == fm2k::cnc_ddraw::State::Checking
                           || snap.state == fm2k::cnc_ddraw::State::Downloading
                           || snap.state == fm2k::cnc_ddraw::State::Extracting;
            if (busy) ImGui::BeginDisabled();
            if (ImGui::Button("Check & install")) {
                fm2k::cnc_ddraw::EnsureInstalled();
            }
            ImGui::SameLine();
            if (ImGui::Button("Force reinstall")) {
                fm2k::cnc_ddraw::ForceReinstall();
            }
            if (busy) ImGui::EndDisabled();

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