// launcher_ui_hub.cpp -- LauncherUI hub/lobby panel + match polling + hub helpers. Split from FM2K_LauncherUI.cpp. NOTE: RenderHubPanel is large; flagged for follow-up factoring.
#include "FM2K_Integration.h"
#include "launcher_ui_hubstate.h"  // LauncherUI::HubState full def
#include "launcher_ui_internal.h"  // shared persistence helpers (namespace lui)
#include "FM2K_HubClient.h"
#include "FM2K_PortMapper.h"  // UPnP port mapper member of LauncherUI (Phase 1)
#include "FM2K_DiscordAuth.h"
#include "FM2K_Locale.h"
#include "FM2K_Updater.h"
#include "version_local.h"
#include "auto_upload_secret.h"
#include "FM2K_UploadQueue.h"
#include "FM2KHook/src/ui/input_binder.h"
#include "FM2KHook/src/ui/shared_mem.h"
#include "FM2KHook/src/util/pii_scrub.h"
#include "FM2K_GameIni.h"
#include "FM2K_DDrawRedirect.h"
#include "FM2K_CncDDraw.h"
#include "FM2K_Utf8Path.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <wininet.h>
#include <shellapi.h>  // Shell_NotifyIcon for challenge toast
#include <shobjidl.h>  // IFileOpenDialog (modern native folder picker)
#include <iostream>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <vector>
#include <array>
#include <thread>
#include <atomic>
#include <mutex>
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

using namespace lui;  // shared persistence helpers (launcher_ui_internal.h)

// Match-results CSV helpers (moved from FM2K_LauncherUI.cpp; AppendResultsCsvRow owns them).
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

// Pre-match launcher STUN + NAT classification (Phase 2a). Binds the UDP
// port the game will reuse, then sends TWO 0xCD 0x01 probes from the SAME
// socket:
//   (A) to hub:hub_udp_port  (the primary STUN port, 7711) -- the hub
//       records this user's *external* NAT mapping (user.udp_addr) BEFORE
//       any match_start fires. Without this, hub's peer_dict() ships the
//       launcher-reported local port -- which only matches reality on
//       port-preserving NATs, and silently breaks for everyone else
//       (Comcast CGNAT, Spanish ISPs that randomize ports, etc.).
//   (B) to hub:hub_udp_port+3 (the classification port, 7714) -- a PURE
//       REFLECTOR on the hub that echoes the observed source but never
//       touches user state.
// Both probes go to the SAME hub IP but DIFFERENT ports. Comparing the two
// reflected external ports gives the RFC-4787 mapping behavior:
//   port_a == port_b -> endpoint/port-independent mapping  -> "cone".
//   port_a != port_b -> a new external port per destination -> "symmetric".
//   no acks          -> "blocked" ; only one ack            -> "unknown".
//
// KNOWN LIMITATION (D7, accepted): same-IP-different-port catches
// PORT-dependent symmetric NATs but NOT a purely address-dependent one
// (which assigns a new port only per destination *IP*, not per port). That
// needs two different hub IPs, which we don't have. The miss is benign:
// such a NAT classifies as "cone" here, Phase 3 may try direct first, and
// punch + peer-learning + relay still back it up.
//
// The two probes are sent BACK-TO-BACK and both acks are collected under a
// SINGLE ~1s deadline (not two serial 800ms waits) to keep the UI hitch
// small. Closes the socket on return -- the game's hook re-binds the same
// local port shortly after, and most cone NATs preserve the
// (local_port -> ext_port) mapping for >=30s of inactivity.
//
// Returns the classification (also surfaced to the hub as udp_addr.nat_type).
fm2k::NatClassifyResult fm2k::LauncherStunClassify(uint16_t local_port,
                                                   const std::string& hub_host,
                                                   uint16_t hub_udp_port,
                                                   const std::string& user_id)
{
    fm2k::NatClassifyResult out;  // defaults to "unknown", ports 0
    if (user_id.empty()) return out;
    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);

    SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == INVALID_SOCKET) return out;
    BOOL reuse = TRUE;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&reuse), sizeof(reuse));
    sockaddr_in laddr{};
    laddr.sin_family = AF_INET;
    laddr.sin_port   = htons(local_port);
    laddr.sin_addr.s_addr = INADDR_ANY;
    if (bind(s, reinterpret_cast<sockaddr*>(&laddr), sizeof(laddr)) != 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "STUN: bind() to %u failed err=%d (skipping pre-match STUN)",
            (unsigned)local_port, WSAGetLastError());
        closesocket(s);
        return out;
    }

    addrinfo hints{};
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(hub_host.c_str(), nullptr, &hints, &res) != 0 || !res) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "STUN: getaddrinfo('%s') failed", hub_host.c_str());
        closesocket(s);
        return out;
    }
    sockaddr_in haddr_a = *reinterpret_cast<sockaddr_in*>(res->ai_addr);
    freeaddrinfo(res);
    sockaddr_in haddr_b = haddr_a;            // same IP...
    haddr_a.sin_port = htons(hub_udp_port);    // ...port 7711 (primary STUN)
    haddr_b.sin_port = htons(hub_udp_port + 3);// ...port 7714 (classification)

    constexpr size_t kUserIdLen = 24;
    uint8_t pkt[2 + kUserIdLen] = { 0xCD, 0x01 };
    size_t n = user_id.size();
    if (n > kUserIdLen) n = kUserIdLen;
    std::memcpy(pkt + 2, user_id.data(), n);

    // Fire both probes back-to-back so the hub answers them in parallel and
    // we wait once, not twice. The primary (A) is the one whose ack the hub
    // also uses to register user.udp_addr.
    int sent_a = sendto(s, reinterpret_cast<const char*>(pkt), sizeof(pkt), 0,
                        reinterpret_cast<const sockaddr*>(&haddr_a), sizeof(haddr_a));
    int sent_b = sendto(s, reinterpret_cast<const char*>(pkt), sizeof(pkt), 0,
                        reinterpret_cast<const sockaddr*>(&haddr_b), sizeof(haddr_b));
    if (sent_a != (int)sizeof(pkt) || sent_b != (int)sizeof(pkt)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "STUN: sendto failed err=%d (a=%d b=%d)", WSAGetLastError(),
            sent_a, sent_b);
    }

    // Single ~1s collection window for BOTH acks. We can't tell which ack is
    // which by the packet alone (both are 0xCD 0x02 ip port from the hub),
    // so we disambiguate by the recvfrom *source port*: an ack from
    // hub_udp_port answers probe A, one from hub_udp_port+3 answers probe B.
    // Loop until both arrive or the deadline passes, shrinking the per-recv
    // timeout to the remaining budget each pass.
    bool have_a = false, have_b = false;
    uint16_t ext_a = 0, ext_b = 0;
    char ip_a_str[INET_ADDRSTRLEN] = {}, ip_b_str[INET_ADDRSTRLEN] = {};
    const uint32_t deadline = static_cast<uint32_t>(SDL_GetTicks()) + 1000;
    while (!(have_a && have_b)) {
        uint32_t now = static_cast<uint32_t>(SDL_GetTicks());
        if (now >= deadline) break;
        DWORD recv_to = deadline - now;            // remaining budget, ms
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char*>(&recv_to), sizeof(recv_to));
        uint8_t buf[64];
        sockaddr_in from{};
        int from_len = sizeof(from);
        int got = recvfrom(s, reinterpret_cast<char*>(buf), sizeof(buf), 0,
                           reinterpret_cast<sockaddr*>(&from), &from_len);
        if (got < 8 || buf[0] != 0xCD || buf[1] != 0x02) {
            if (got <= 0) break;  // timeout/error -- stop waiting
            continue;             // junk packet, keep waiting within budget
        }
        uint32_t ip_be = 0;
        std::memcpy(&ip_be, buf + 2, 4);
        uint16_t port_be = 0;
        std::memcpy(&port_be, buf + 6, 2);
        uint16_t reflected = ntohs(port_be);
        in_addr ia{}; ia.s_addr = ip_be;
        char ip_str[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &ia, ip_str, sizeof(ip_str));
        uint16_t src_port = ntohs(from.sin_port);
        if (src_port == hub_udp_port && !have_a) {
            have_a = true; ext_a = reflected;
            std::memcpy(ip_a_str, ip_str, sizeof(ip_a_str));
        } else if (src_port == (uint16_t)(hub_udp_port + 3) && !have_b) {
            have_b = true; ext_b = reflected;
            std::memcpy(ip_b_str, ip_str, sizeof(ip_b_str));
        }
        // Any other source port: ignore (not one of our two probes).
    }

    out.port_a = ext_a;
    out.port_b = ext_b;
    if (have_a && have_b) {
        out.nat_type = (ext_a == ext_b) ? "cone" : "symmetric";
    } else if (!have_a && !have_b) {
        out.nat_type = "blocked";
    } else {
        out.nat_type = "unknown";  // only one side answered -- inconclusive
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "NAT classify: %s (port_a=%u port_b=%u, refl_ip=%s, local=%u, user=%s)",
        out.nat_type.c_str(), (unsigned)ext_a, (unsigned)ext_b,
        have_a ? ip_a_str : (have_b ? ip_b_str : "?"),
        (unsigned)local_port, user_id.c_str());
    if (!have_a) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "STUN: no ack from primary %s:%u within ~1s "
            "(hub records mapping anyway)",
            hub_host.c_str(), (unsigned)hub_udp_port);
    }

    closesocket(s);
    return out;
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

// Settings → Hub Server… window. Lets the user point the launcher at a
// custom hub (their own hub.py instance, a friend's box, etc.) without
// cluttering the main Hub panel for casual users who just want the
// default hub.2dfm.org.
void LauncherUI::RenderHubServerBody() {
    if (!hub_host_initialized_) {
        hub_host_initialized_ = true;
        const char* env_h = std::getenv("FM2K_HUB_HOST");
        const char* def   = (env_h && env_h[0]) ? env_h : "hub.2dfm.org";
        std::snprintf(hub_host_, sizeof(hub_host_), "%s", def);
    }
    ImGui::PushItemWidth(280);
    ImGui::InputText(T("netcfg_host"), hub_host_, sizeof(hub_host_));
    ImGui::PopItemWidth();
    ImGui::TextWrapped(
        "Hub server hostname or IP. Default hub.2dfm.org for public play. "
        "Use 127.0.0.1 (or localhost) when running your own hub.py on the same "
        "machine — NAT routers rarely hairpin so the public DNS won't loop back. "
        "Takes effect on next Connect.");
    // Update Channel toggle was moved to the top-level Settings menu —
    // 2 clicks instead of 4 to flip. See RenderMenuBar.
}

void LauncherUI::RenderHubServerWindow() {
    if (!ImGui::Begin("Hub Server", &show_hub_server_, ImGuiWindowFlags_None)) {
        ImGui::End();
        return;
    }
    RenderHubServerBody();
    ImGui::End();
}

// Settings → Recent Matches… (#49). Read-only scoreboard fed by
// HubClient::RequestRecentMatches → K::RecentMatchesReceived. Refreshed
// on Connected and after every match_result so the just-finished match
// shows up at the top. The Refresh button forces an out-of-band fetch
// for users who left the launcher open between hub deployments.
void LauncherUI::RenderRecentMatchesBody() {
    auto& hs = *hub_state_;

    // Unique-ID suffix — when the Recent Matches and Live Matches
    // panels are both visible the bare T("btn_refresh") collides on
    // ImGui's hashed-by-label widget IDs and clicking one fires the
    // other's callback (FlippySpatula's bug). PushID isolates them.
    ImGui::PushID("recent_matches_refresh");
    if (ImGui::Button(T("btn_refresh"))) {
        if (hs.client.IsConnected()) hs.client.RequestRecentMatches(50);
    }
    ImGui::PopID();
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
        ImGui::PushID("live_matches_refresh");
        if (ImGui::SmallButton(T("btn_refresh"))) {
            if (hs.client.IsConnected()) hs.client.RequestCurrentMatches();
        }
        ImGui::PopID();
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
    char title[256];
    // Session suffix — Patrick's bug. Empty until we've finished at
    // least one match this launcher session so the title isn't
    // cluttered with "0-0-0" for everyone who just opened the app.
    char session_buf[48] = {};
    if (hs.session_wins + hs.session_losses + hs.session_draws > 0) {
        std::snprintf(session_buf, sizeof(session_buf),
                      " \xe2\x80\xa2 session %d-%d-%d",
                      hs.session_wins, hs.session_losses, hs.session_draws);
    }
    if (hs.my_wins < 0) {
        // No overall record yet — bare title or session-only.
        if (session_buf[0]) {
            std::snprintf(title, sizeof(title),
                          "FM2K Rollback Launcher%s", session_buf);
        } else {
            std::snprintf(title, sizeof(title), "FM2K Rollback Launcher");
        }
    } else if (!hs.my_nick.empty()) {
        std::snprintf(title, sizeof(title),
                      "FM2K Rollback Launcher \xe2\x80\x94 %s (%d-%d-%d)%s",
                      hs.my_nick.c_str(), hs.my_wins, hs.my_losses, hs.my_draws,
                      session_buf);
    } else {
        std::snprintf(title, sizeof(title),
                      "FM2K Rollback Launcher \xe2\x80\x94 %d-%d-%d%s",
                      hs.my_wins, hs.my_losses, hs.my_draws, session_buf);
    }
    SDL_SetWindowTitle(window_, title);
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

void LauncherUI::PollUploadQueue() {
    // Read the static dev-checkbox state. Declared inside Render's local
    // g_auto_upload_logs is a file-scope mirror of the dev checkbox
    // state, kept in sync by Render() (lambda init + Checkbox handler
    // in the developer section).
    if (!g_auto_upload_logs) return;

    // Bail when the build had no secret baked in — nowhere to upload.
    if (!fm2k::kLogUploadSecret || fm2k::kLogUploadSecret[0] == '\0') return;

    if (games_.empty()) return;

    // Pause uploads while a match is live. Even off the UI thread, draining a
    // backlog of fat rngtrace.csv bundles saturates the user's UPSTREAM
    // bandwidth and competes with the game's live netplay UDP -> ping spikes
    // (Melancholy's 800ms persisted after the thread fix). Telemetry isn't
    // urgent: only drain at the menu, never while Connecting or InGame.
    if (launcher_state_ == LauncherState::Connecting ||
        launcher_state_ == LauncherState::InGame) return;

    // Trickle: at most one upload every ~1.5s so even a large backlog drains
    // gently instead of flooding the connection in a single burst.
    static uint64_t s_next_ms = 0;
    const uint64_t now_ms = (uint64_t)GetTickCount64();
    if (now_ms < s_next_ms) return;

    // Round-robin through ALL installed games, one game per tick, instead
    // of only the UI-selected one. The old code scanned
    // games_[selected_game_index_] only, which meant a crash/desync bundle
    // in (say) pkmncc/upload_queue never uploaded unless the user happened
    // to reopen the launcher AND re-select pkmncc — and selected_game_index_
    // defaults to -1, so a fresh launcher start drained nothing at all.
    // That stranded the bulk of field reports (match c785d0ca's two peers
    // among them). Rotating one game per frame keeps the per-tick cost
    // identical (still one Process() call) while guaranteeing every game's
    // queue drains regardless of selection.
    // CRITICAL: Process() does a SYNCHRONOUS WinHTTP POST (up to a 15s
    // timeout per upload). This runs every frame from Render(), so doing it
    // on this (the UI/render) thread freezes the launcher for the duration
    // of each upload -- catastrophic once a user has a backlog of stranded
    // bundles (multi-second-to-minute unresponsive launcher, reported on
    // 0.2.65 when the upload secret finally worked and the drain actually
    // fired). Run the blocking drain on a detached worker thread instead, so
    // the render loop never stalls. s_busy keeps exactly one upload in flight
    // at a time (no thread pile-up); over successive frames it round-robins
    // every game's queue and drains backlogs one manifest per completed run.
    static std::atomic<bool> s_busy{false};
    if (s_busy.exchange(true)) return;  // an upload is already running
    s_next_ms = now_ms + 1500;          // >= 1.5s before the next upload starts

    static size_t s_rr = 0;
    if (s_rr >= games_.size()) s_rr = 0;
    const FM2K::FM2KGameInfo& game = games_[s_rr++];

    // Snapshot the game dir on THIS thread (we own games_); pass by value to
    // the worker so it never touches launcher state. fs::path .string() does
    // the wide->UTF-8 round-trip; game dirs are ASCII in practice.
    std::filesystem::path exe = fm2k::utf8path::Utf8ToWide(game.exe_path);
    std::string game_dir = exe.parent_path().string();

    std::thread([game_dir]() {
        fm2k::upload_queue::ProcessorConfig cfg;
        cfg.game_dir   = game_dir;
        cfg.upload_url = fm2k::kLogUploadUrl;   // global constexpr -- thread-safe
        cfg.secret     = fm2k::kLogUploadSecret;
        cfg.enabled    = true;
        // Upload ONE manifest per run, then release the slot. The 1.5s
        // throttle + round-robin in PollUploadQueue pace the rest, so a
        // backlog trickles out over time instead of flooding the connection
        // in one burst (which is what spiked netplay ping even off-thread).
        fm2k::upload_queue::Process(cfg);
        s_busy.store(false);
    }).detach();
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
    // Pre-v0.2.45: a WS disconnect HERE silently dropped the outcome
    // for the rest of the set (long pause → keepalive timeout → WS drop
    // is the classic trigger; multiple users reported wins not counting
    // after long pauses). We now FALL THROUGH on disconnect — CSV mirror
    // still writes, hub send is queued onto hs.pending_match_results,
    // and the K::Connected handler drains the queue on reconnect.
    const bool ws_connected = hs.client.IsConnected();
    if (!ws_connected) {
        static bool s_warned_disconnect = false;
        if (!s_warned_disconnect) {
            s_warned_disconnect = true;
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "PollMatchOutcome: hub WS disconnected — outcomes will be "
                "queued and replayed on reconnect");
        }
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

                // Desync: GekkoNet caught state checksum divergence
                // between peers and the hook terminated the game on the
                // first occurrence (prevents the cascading-corruption
                // crash users were hitting at character_state_machine
                // 0x4125FC after thousands of frames of bad sim state).
                // No W/L/D record — sim diverged, outcome undefined.
                if (outcome_u8 == FM2K_MATCH_OUTCOME_DESYNC) {
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Hub: DESYNC — game terminated on first divergence "
                        "(no record). Inspect FM2K_P*_desync_f*.log for "
                        "the per-region diff.");
                    hs.status_line = "desync detected — match not recorded";
                    FireSystemNotification(
                        "Desync detected",
                        "Game state diverged between peers. Match was "
                        "stopped before sim corruption caused a crash. "
                        "Re-launch to start a fresh session.");
                    if (on_session_stop) on_session_stop();
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
                // Session counter (Patrick's bug — wanted current-session
                // record in the titlebar in addition to the overall). Only
                // committed outcomes count; disconnect doesn't, matching
                // the hub's ledger semantics.
                bool session_changed = false;
                switch (outcome_u8) {
                    case FM2K_MATCH_OUTCOME_SELF_WON: ++hs.session_wins;   session_changed = true; break;
                    case FM2K_MATCH_OUTCOME_PEER_WON: ++hs.session_losses; session_changed = true; break;
                    case FM2K_MATCH_OUTCOME_DRAW:     ++hs.session_draws;  session_changed = true; break;
                    default: break;
                }
                if (session_changed) UpdateWindowTitleWithRecord();
                if (outcome_str) {
                    // Local CSV mirror first — runs even if the hub
                    // send queues silently because we're disconnected.
                    // Same data, written to %APPDATA%\FM2K_Rollback\
                    // results.csv. CCCaster-equivalent for offline
                    // history.
                    AppendResultsCsvRow(outcome_str,
                                        p1_char_id, p2_char_id,
                                        p1_char_name, p2_char_name);

                    // C10 schema-2: pull session_id + match_index +
                    // rounds[] from shared mem v10. Fields are 0/empty
                    // on legacy hook builds and the schema-2 overload
                    // automatically falls back to schema-1 wire shape
                    // when session_id == 0 && rounds.empty().
                    const uint64_t session_id =
                        data->match_session_id;
                    const uint8_t  match_index =
                        data->match_index_in_session;
                    std::vector<fm2k::HubClient::RoundJson> rounds_json;
                    const uint8_t rc =
                        std::min<uint8_t>(data->match_rounds_count, 8);
                    rounds_json.reserve(rc);
                    for (uint8_t i = 0; i < rc; ++i) {
                        const FM2KRoundResult& r = data->match_rounds[i];
                        rounds_json.push_back(fm2k::HubClient::RoundJson{
                            r.winner_idx,
                            r.p1_hp_remaining,
                            r.p2_hp_remaining,
                            r.frames_elapsed,
                        });
                    }

                    if (ws_connected) {
                        hs.client.MatchResult(hs.current_match_token, outcome_str,
                                              p1_char_id, p2_char_id,
                                              p1_char_name, p2_char_name,
                                              stage_id, stage_name,
                                              session_id, match_index,
                                              rounds_json);
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Hub: match_result sent token=%.8s... outcome=%s "
                            "p1=%u(\"%s\") p2=%u(\"%s\") stage=%u(\"%s\") "
                            "session=0x%016llx match_idx=%u rounds=%u "
                            "pid=%lu seq=%u",
                            hs.current_match_token.c_str(), outcome_str,
                            p1_char_id, p1_char_name.c_str(),
                            p2_char_id, p2_char_name.c_str(),
                            stage_id, stage_name.c_str(),
                            (unsigned long long)session_id,
                            (unsigned)match_index, (unsigned)rc,
                            (unsigned long)pid, (unsigned)seq);
                        // Refresh our cached W/L/D so the UI updates
                        // immediately for the next room render. Also
                        // refresh the recent-matches list so the just-
                        // committed match shows up at the top.
                        hs.client.QueryRecord();
                        hs.client.RequestRecentMatches(50);
                    } else {
                        // WS dropped — queue everything for K::Connected
                        // to drain. CSV mirror already written above so
                        // the local record is intact even if the hub
                        // stays down forever.
                        HubState::PendingMatchResult q;
                        q.token         = hs.current_match_token;
                        q.outcome       = outcome_str;
                        q.p1_char_id    = p1_char_id;
                        q.p2_char_id    = p2_char_id;
                        q.p1_char_name  = p1_char_name;
                        q.p2_char_name  = p2_char_name;
                        q.stage_id      = stage_id;
                        q.stage_name    = stage_name;
                        q.session_id    = session_id;
                        q.match_index   = match_index;
                        q.rounds        = rounds_json;
                        hs.pending_match_results.push_back(std::move(q));
                        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "Hub: match_result QUEUED (WS disconnected) "
                            "token=%.8s... outcome=%s — pending=%zu, "
                            "will replay on reconnect",
                            hs.current_match_token.c_str(), outcome_str,
                            hs.pending_match_results.size());
                    }
                    hs.match_result_sent = true;

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

    // Periodic pre-match STUN refresh (every 20 s while connected) so
    // the hub-stored external NAT port stays alive even on quiet
    // lobbies. Cheap — one UDP packet up, one back, ≤800 ms timeout.
    // Skipped while we're in an active match (the in-game hook owns the
    // socket then) and while the user is mid-challenge (we'd briefly
    // bind/release the port the preflight punch is about to need).
    if (hs.client.IsConnected() && !hs.my_id.empty() &&
        hs.current_match_token.empty()) {
        uint32_t now_ms = static_cast<uint32_t>(SDL_GetTicks());
        if (hs.last_stun_refresh_ms == 0 ||
            (now_ms - hs.last_stun_refresh_ms) > 20000) {
            const char* hub_host_env = std::getenv("FM2K_HUB_HOST");
            std::string hub_host = (hub_host_env && hub_host_env[0])
                                 ? hub_host_env
                                 : (hub_host_[0] ? hub_host_ : "hub.2dfm.org");
            fm2k::NatClassifyResult nat = fm2k::LauncherStunClassify(
                static_cast<uint16_t>(network_config_.local_port),
                hub_host, 7711, hs.my_id);
            hs.last_stun_refresh_ms = now_ms;
            // Re-send nat_type only on a change (it's stable on a given
            // network; "unknown"/"blocked" can flap on a dropped ack, so we
            // still update so the hub reflects the latest reading).
            if (nat.nat_type != hs.last_nat_type) {
                hs.client.SendNatType(nat.nat_type);
                hs.last_nat_type = nat.nat_type;
            }
        }
    }

    // UPnP port-mapper poll (Phase 1). Snapshot the mapper status once per
    // frame and, on a state TRANSITION, act exactly once. The mapper itself
    // is off-thread; this is the cheap UI-thread side. port_mapper_ is only
    // non-null for non-loopback online sessions (set at the Connected
    // event), so this is implicitly skipped for LOCAL / local-dev hubs.
    if (port_mapper_ && hs.client.IsConnected()) {
        fm2k::PortMapper::Status st = port_mapper_->Snapshot();
        const int cur_state = static_cast<int>(st.state);
        if (cur_state != port_mapper_last_state_) {
            port_mapper_last_state_ = cur_state;
            if (st.state == fm2k::PortMapper::State::Mapped &&
                st.ext_udp_port > 0 && !st.ext_ip.empty()) {
                // RE-SEND udp_addr carrying the external endpoint. The hub
                // accepts udp_addr updates at any time (STUN re-sends the
                // same way), applies the D5 precedence (UPnP ext_port
                // outranks STUN-learned), and CGNAT-guards ext_ip against
                // the WS-source IP (D6). We still send "127.0.0.1" as the
                // base ip exactly like the connect-time send -- the hub
                // ignores it for the udp_addr IP (it uses the WS source);
                // ext_ip is the field that carries the WAN address.
                hs.client.SendUdpAddrUpnp(
                    "127.0.0.1",
                    network_config_.local_port,
                    network_config_.local_port,
                    st.ext_ip,
                    static_cast<int>(st.ext_udp_port),
                    /*upnp=*/true);
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[upnp] mapping live -- re-sent udp_addr ext=%s:%u to hub",
                    st.ext_ip.c_str(),
                    static_cast<unsigned>(st.ext_udp_port));
            } else if (st.state == fm2k::PortMapper::State::Cgnat) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[upnp] IGD behind CGNAT/double-NAT -- not advertising a "
                    "UPnP port (punch + relay still cover us)");
            } else if (st.state == fm2k::PortMapper::State::NoIgd) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[upnp] no UPnP-capable router found -- direct reach via "
                    "punch only");
            }
        }
    }

    // Drain hub events into local state once per frame.
    hs.client.Poll([&](const fm2k::HubEvent& ev) {
        using K = fm2k::HubEvent::Kind;
        switch (ev.kind) {
            case K::Connected:
                hs.my_id = ev.user_id;
                hs.rooms = ev.rooms;
                hs.status_line = "connected";
                // Drain any match results queued while the WS was down.
                // PollMatchOutcome falls through to push here when
                // hs.client.IsConnected() is false (long pause →
                // keepalive timeout → drop is the classic case; pre-
                // v0.2.45 those outcomes were silently lost for the
                // rest of the set). Local CSV was already written at
                // outcome time, so this is purely hub catch-up.
                if (!hs.pending_match_results.empty()) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Hub: replaying %zu queued match_result(s) on reconnect",
                        hs.pending_match_results.size());
                    for (const auto& q : hs.pending_match_results) {
                        hs.client.MatchResult(
                            q.token, q.outcome,
                            q.p1_char_id, q.p2_char_id,
                            q.p1_char_name, q.p2_char_name,
                            q.stage_id, q.stage_name,
                            q.session_id, q.match_index,
                            q.rounds);
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "  replayed token=%.8s... outcome=%s",
                            q.token.c_str(), q.outcome.c_str());
                    }
                    hs.pending_match_results.clear();
                    // Refresh the record + recent-matches after the
                    // catch-up so the UI shows the corrected totals
                    // right away.
                    hs.client.QueryRecord();
                    hs.client.RequestRecentMatches(50);
                }
                // Process-wide FM2K_HUB_USER_ID — companion to FM2K_HUB_UDP_ADDR
                // set at hub-connect time. Together they unlock the hook's
                // SendStunProbe call, so any spawned game (player or spec)
                // gets STUN'd and hub's user.udp_addr reflects this hook's
                // actual external UDP mapping.
                ::SetEnvironmentVariableA("FM2K_HUB_USER_ID", hs.my_id.c_str());
                // Tell the hub our planned UDP listen so it can relay
                // it to a peer in match_start. Both launchers register
                // their already-configured network_config_.local_port.
                // For LAN/internet, replace "127.0.0.1" with the hub-
                // observed reflexive IP (Phase 2 — STUN responder).
                // Spec hook binds its TCP listener to the same port as UDP
                // (convention enforced in spectator_tcp.cpp's Start). Send
                // both so the hub can forward spec_tcp_port in the
                // spectator_incoming event for the host's TCP punch.
                hs.client.SendUdpAddr("127.0.0.1", network_config_.local_port,
                                      network_config_.local_port);

                // UPnP automatic port mapping (Phase 1 NAT reachability).
                // Kick off an asynchronous router mapping of our game UDP
                // port so peers can reach us directly. This runs ALONGSIDE
                // the SendUdpAddr above -- we never block on it; if UPnP is
                // slow or absent the SendUdpAddr already gave the hub our
                // STUN-learned endpoint, so behavior is exactly today's. On
                // success the per-frame poll in RenderHubPanel re-sends
                // udp_addr with the external endpoint (see below).
                //
                // Skip entirely for a loopback hub: that's the local-dev /
                // multi-instance test path (the netcfg hint literally says
                // "use 127.0.0.1 when running your own hub.py"), where two
                // launchers on one machine must NOT both grab the same
                // router mapping. Real online sessions always run against a
                // non-loopback hub.
                {
                    const char* hub_host_env = std::getenv("FM2K_HUB_HOST");
                    std::string hh = (hub_host_env && hub_host_env[0])
                                   ? hub_host_env
                                   : (hub_host_[0] ? hub_host_ : "hub.2dfm.org");
                    const bool loopback_hub =
                        (hh == "127.0.0.1" || hh == "localhost" || hh == "::1");
                    if (!loopback_hub) {
                        if (!port_mapper_) {
                            port_mapper_ = std::make_unique<fm2k::PortMapper>();
                        }
                        // Reset the re-send latch so a reconnect re-arms the
                        // one-shot Mapped transition send.
                        port_mapper_last_state_ = -1;
                        port_mapper_->StartAsync(
                            static_cast<uint16_t>(network_config_.local_port));
                    } else {
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "[upnp] loopback hub (%s) -- skipping UPnP mapping",
                            hh.c_str());
                    }
                }
                // Pre-match STUN. The in-game hook does its own STUN at
                // launch but match_start fires immediately on accept and
                // the hook's STUN doesn't arrive at the hub for several
                // seconds (after game spawn + hook init). Without a
                // pre-match STUN, hub.peer_dict() falls back to the
                // launcher-reported local port, which only matches the
                // external NAT mapping on port-preserving cone NATs —
                // every other client gets the wrong port and punches a
                // closed door. Doing this here means the hub has the
                // correct (ip, ext_port) before any challenge fires.
                {
                    const char* hub_host_env = std::getenv("FM2K_HUB_HOST");
                    std::string hub_host = (hub_host_env && hub_host_env[0])
                                         ? hub_host_env
                                         : (hub_host_[0] ? hub_host_ : "hub.2dfm.org");
                    // The probe doubles as NAT classification (Phase 2a): it
                    // still STUNs :7711 (registering user.udp_addr exactly as
                    // before) and additionally probes :7714 to derive
                    // cone/symmetric. Report the result to the hub so it lands
                    // in peer_dict for the next match_start.
                    fm2k::NatClassifyResult nat = fm2k::LauncherStunClassify(
                        static_cast<uint16_t>(network_config_.local_port),
                        hub_host, 7711, hs.my_id);
                    hs.last_stun_refresh_ms = static_cast<uint32_t>(SDL_GetTicks());
                    hs.client.SendNatType(nat.nat_type);
                    hs.last_nat_type = nat.nat_type;
                }
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
                    // Same-box dev test loopback fallback only fires when
                    // there is NO hub relay configured. With a relay
                    // available, falsely flipping peer_ip to 127.0.0.1
                    // sends HELLO into our own loopback while the relay
                    // sits idle — both peers stall at handshake. Trust
                    // the public peer_ip on real cross-NAT matches and
                    // let the hook's NAT traversal use the relay.
                    const bool have_relay = !ev.match.relay_ip.empty()
                                         && ev.match.relay_port > 0;
                    if (!public_reachable && !have_relay) {
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
                                "Hub: probe didn't get a reply. Spawning "
                                "game anyway — in-game NAT traversal "
                                "(STUN + punch + relay) will retry on its own.");
                        }
                    } else if (!public_reachable && have_relay) {
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Hub: public probe failed; relay configured "
                            "(%s:%u). Hook NAT path will fall through to "
                            "the relay — keeping peer=%s:%d as remote.",
                            ev.match.relay_ip.c_str(),
                            (unsigned)ev.match.relay_port,
                            peer_ip.c_str(), peer_port);
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
                    // Pre-resolve the hub host to a dotted-quad IP here
                    // so the in-game hook's Netplay_Init -> SendStunProbe
                    // -> ResolveHostA path short-circuits instead of
                    // doing DNS inside DllMain. DllMain DNS lookups can
                    // hang for 5-15 s on flaky resolvers / IPv6-fallback
                    // paths, which blows past the inject timeout and
                    // looks like a Defender block. Doing the lookup in
                    // the launcher (a normal thread, not a loader-lock
                    // context) lets us survive slow DNS without timing
                    // out the inject. Falls back to the hostname only
                    // if resolution fails — at least then the hook gets
                    // its own attempt.
                    const char* hub_host_env = std::getenv("FM2K_HUB_HOST");
                    const std::string hub_host_str =
                        (hub_host_env && hub_host_env[0])
                            ? hub_host_env : "hub.2dfm.org";
                    std::string hub_udp;
                    {
                        addrinfo hints{};
                        hints.ai_family   = AF_INET;
                        hints.ai_socktype = SOCK_DGRAM;
                        addrinfo* res = nullptr;
                        if (getaddrinfo(hub_host_str.c_str(), nullptr,
                                        &hints, &res) == 0 && res) {
                            char ip_str[INET_ADDRSTRLEN] = {};
                            const sockaddr_in* sin =
                                reinterpret_cast<const sockaddr_in*>(res->ai_addr);
                            if (inet_ntop(AF_INET, &sin->sin_addr,
                                          ip_str, sizeof(ip_str))) {
                                hub_udp = std::string(ip_str) + ":7711";
                                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                    "Hub: pre-resolved %s -> %s for FM2K_HUB_UDP_ADDR "
                                    "(keeps DllMain off the DNS path)",
                                    hub_host_str.c_str(), ip_str);
                            }
                            freeaddrinfo(res);
                        }
                        if (hub_udp.empty()) {
                            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                                "Hub: getaddrinfo('%s') failed; passing hostname "
                                "to game (hook will retry, may stall DllMain)",
                                hub_host_str.c_str());
                            hub_udp = hub_host_str + ":7711";
                        }
                    }
                    ::SetEnvironmentVariableA("FM2K_HUB_UDP_ADDR",   hub_udp.c_str());
                    ::SetEnvironmentVariableA("FM2K_HUB_USER_ID",    hs.my_id.c_str());
                    ::SetEnvironmentVariableA("FM2K_HUB_MATCH_TOKEN", ev.match.token.c_str());
                    // TCP-STUN endpoint — same hub host, port+2 (UDP-STUN
                    // is +0, UDP-relay is +1). Hook's PerformTcpStun reads
                    // this; absent → hook skips TCP-STUN and the spec
                    // falls back to local listener port (works on port-
                    // preserving NATs only).
                    if (!hub_udp.empty()) {
                        const auto colon = hub_udp.rfind(':');
                        std::string tcp_stun;
                        if (colon != std::string::npos) {
                            // hub_udp port is :7711 by convention; +2 = 7713
                            tcp_stun = hub_udp.substr(0, colon) + ":7713";
                        }
                        if (!tcp_stun.empty()) {
                            ::SetEnvironmentVariableA(
                                "FM2K_HUB_TCP_STUN_ADDR", tcp_stun.c_str());
                        }
                    }

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
                        // Pre-resolve the relay host to a dotted-quad for the
                        // same reason as FM2K_HUB_UDP_ADDR above: ConfigureRelay
                        // -> ResolveHostA -> getaddrinfo runs inside the hook's
                        // Netplay_Init (DllMain context). A hostname here would
                        // put a DNS lookup on the loader-lock path; resolving it
                        // in the launcher (a normal thread) keeps DllMain off
                        // DNS. If relay_ip is already a dotted-quad, getaddrinfo
                        // short-circuits via the numeric path. On failure we fall
                        // back to the hostname so the hook still gets a try.
                        std::string relay_host = ev.match.relay_ip;
                        bool is_dotted_quad = false;
                        {
                            in_addr probe{};
                            is_dotted_quad =
                                (inet_pton(AF_INET, relay_host.c_str(), &probe) == 1);
                        }
                        if (!is_dotted_quad) {
                            addrinfo hints{};
                            hints.ai_family   = AF_INET;
                            hints.ai_socktype = SOCK_DGRAM;
                            addrinfo* res = nullptr;
                            if (getaddrinfo(relay_host.c_str(), nullptr,
                                            &hints, &res) == 0 && res) {
                                char ip_str[INET_ADDRSTRLEN] = {};
                                const sockaddr_in* sin =
                                    reinterpret_cast<const sockaddr_in*>(res->ai_addr);
                                if (inet_ntop(AF_INET, &sin->sin_addr,
                                              ip_str, sizeof(ip_str))) {
                                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                        "Relay: pre-resolved %s -> %s for "
                                        "FM2K_HUB_RELAY_ADDR (keeps DllMain off DNS)",
                                        relay_host.c_str(), ip_str);
                                    relay_host = ip_str;
                                }
                                freeaddrinfo(res);
                            } else {
                                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                                    "Relay: getaddrinfo('%s') failed; passing "
                                    "hostname to game (hook will retry)",
                                    relay_host.c_str());
                            }
                        }
                        std::string relay_addr = relay_host + ":" +
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
                    on_spectate_match(ev.spectate.host_ip, ev.spectate.host_port,
                                      ev.spectate.session_kind,
                                      ev.spectate.spec_transport);
                }
                break;
            }
            case K::SpectateDenied:
                hs.status_line = "spectate denied: " + ev.error;
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Hub: %s", hs.status_line.c_str());
                break;
            case K::SpectatorIncoming: {
                // We're the host of an in-progress match; hub forwarded a
                // spectator's external UDP+TCP addr. Forward to game-instance
                // shared mem → hook's TickHostMaintenance fires both:
                //   * UDP heartbeat burst (existing — opens NAT for the
                //     spectator's first SPEC_JOIN_REQ replies)
                //   * TCP simultaneous-open punch (new in v0.2.35 — opens
                //     NAT for inbound TCP from spec:tcp_port to our
                //     listener port, the path the INPUT_BATCH stream uses)
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Hub: spectator_incoming nick=%s addr=%s udp:%d tcp:%d — punching",
                    ev.spectator_incoming.spec_nick.c_str(),
                    ev.spectator_incoming.spec_udp_ip.c_str(),
                    ev.spectator_incoming.spec_udp_port,
                    ev.spectator_incoming.spec_tcp_port);
                if (on_spectator_punch_target) {
                    on_spectator_punch_target(
                        ev.spectator_incoming.spec_udp_ip,
                        ev.spectator_incoming.spec_udp_port,
                        ev.spectator_incoming.spec_tcp_port,
                        ev.spectator_incoming.spec_user_id);
                }
                break;
            }
            case K::SpecRelayBinary: {
                // Phase 3: hub forwarded spec data bytes to us. Hand to
                // the launcher controller which writes them into the
                // inbound shared-mem ring for the running spec game.
                // Dispatch via the same callback pattern as
                // on_spectator_punch_target so launcher controls the
                // mapping lifecycle.
                if (on_spec_relay_bytes && !ev.spec_relay_bytes.empty()) {
                    on_spec_relay_bytes(ev.spec_relay_bytes);
                }
                break;
            }
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
        const char* def   = (env_h && env_h[0]) ? env_h : "hub.2dfm.org";
        std::snprintf(hub_host_, sizeof(hub_host_), "%s", def);
    }
    // Delay override panel. Both peers exchange their delay candidate
    // over the control channel and adopt max(both), so delay is always
    // identical on both sides (#24). This combo picks how THIS peer's
    // candidate is sized:
    //   index 0 "computed (avg ping)"  -> FM2K_DELAY_MODE=0, mean RTT
    //   index 1 "computed (peak ping)" -> FM2K_DELAY_MODE=1, worst RTT
    //   index 2..18 manual 0..16       -> FM2K_LOCAL_DELAY=N
    // FM2K_LOCAL_DELAY is cleared for the two computed modes so the hook
    // computes from RTT; a manual pick still rides the exchange, so a
    // peer who pins a high value pulls the other peer up to match.
    static int s_delay_override = 0;
    {
        // Manual delay range: 0..16. 0 is opt-in for sub-1ms LAN /
        // loopback / hot-seat play — GekkoNet's prediction-0 mode applies
        // input same frame, but ANY jitter on the link will rollback
        // every frame. Users on actual internet should leave this on
        // computed. 16 = 160 ms, basically the upper limit of playable
        // delay-only netcode.
        const char* delay_items[] = {
            "computed (avg ping)",
            "computed (peak ping)",
            "0",  "1",  "2",  "3",  "4",  "5",  "6",  "7",  "8",
            "9", "10", "11", "12", "13", "14", "15", "16",
        };
        ImGui::PushItemWidth(-120);
        ImGui::Combo("Delay", &s_delay_override, delay_items,
                     IM_ARRAYSIZE(delay_items));
        ImGui::PopItemWidth();
        ImGui::SetItemTooltip(
            "Input delay (frames at 100 Hz). Both peers exchange their "
            "pick and adopt the higher one, so delay is always the "
            "same on both sides. \"computed (avg ping)\" sizes delay to "
            "mean RTT -- lower delay, but spikes can cause rollbacks. "
            "\"computed (peak ping)\" sizes to the worst RTT seen -- "
            "higher delay, rides out jitter. Pin 0..16 to force a "
            "manual value. 0 = same-frame input, only safe on near-"
            "zero-latency links (LAN / loopback / hotseat). 16 = 160 "
            "ms, upper limit of playable delay-only netcode.");
        if (s_delay_override >= 2) {
            // Manual override: index 2 -> "0", index 3 -> "1", ...
            char buf[8];
            std::snprintf(buf, sizeof(buf), "%d", s_delay_override - 2);
            ::SetEnvironmentVariableA("FM2K_LOCAL_DELAY", buf);
        } else {
            ::SetEnvironmentVariableA("FM2K_LOCAL_DELAY", nullptr);
        }
        // Computed-delay formula: index 0 = avg ping, index 1 = peak.
        // Harmless when a manual value is pinned (the hook ignores mode
        // once FM2K_LOCAL_DELAY is set).
        ::SetEnvironmentVariableA("FM2K_DELAY_MODE",
                                  s_delay_override == 1 ? "1" : "0");
    }

    // Runahead and prediction window are intentionally NOT exposed here.
    // Neither is a free "pick a number" knob:
    //   - prediction window must be IDENTICAL on both peers (GekkoNet
    //     derives the desync-detection checkpoint frame from it) -- a
    //     mismatch causes false desyncs, so it stays fixed at 16.
    //   - runahead's correct value is exactly the input delay; the hook
    //     auto-tracks the negotiated local_delay.
    // FM2K_PREDICTION_WINDOW / FM2K_RUNAHEAD env vars still force-pin
    // either value for dev bisecting (set on BOTH peers for prediction).

    // Stealth / ghost mode (persisted to dev_flags.ini). When on, the hub keeps
    // your match + characters out of the lobby and public stats -- for testing
    // unreleased builds without leaking them. Rendered in BOTH connect states so
    // it can be toggled LIVE: SetStealth() rides the next hello while
    // disconnected, and sends a live "set_stealth" update while connected (hub
    // flips us in/out of the lobby immediately, no reconnect).
    {
        static bool s_stealth_loaded = false;
        if (!s_stealth_loaded) {
            s_stealth_loaded = true;
            hs.stealth = LoadDevFlagInt("stealth_mode", 0) != 0;
            hs.client.SetStealth(hs.stealth);
        }
    }
    if (ImGui::Checkbox("Stealth mode (hide my match + characters from the lobby)", &hs.stealth)) {
        SaveDevFlagInt("stealth_mode", hs.stealth ? 1 : 0);
        hs.client.SetStealth(hs.stealth);  // rides hello if disconnected; live update if connected
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
        ImGui::TextDisabled(T("hub_server"), hub_host_[0] ? hub_host_ : "hub.2dfm.org");
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
            const std::string hub_host = (hub_host_[0] != '\0') ? hub_host_ : "hub.2dfm.org";
            ::SetEnvironmentVariableA("FM2K_HUB_HOST", hub_host.c_str());
            // TCP-STUN endpoint — same hub host, port 7713 (UDP-STUN at
            // 7711, UDP-relay at 7712). Set process-wide here so every
            // spawned game (player AND spectator) inherits and can run
            // its outbound TCP-STUN probe at hook init. Without this,
            // the spec hook logs "FM2K_HUB_TCP_STUN_ADDR unset — skipping"
            // and falls back to local listener port for cross-NAT punch
            // — which fails on non-port-preserving NATs.
            ::SetEnvironmentVariableA("FM2K_HUB_TCP_STUN_ADDR",
                                      (hub_host + ":7713").c_str());
            // FM2K_HUB_UDP_ADDR — set at connect time (hub_host known here).
            // FM2K_HUB_USER_ID is set on Connected (hello_ack) where my_id
            // first lands; both are required by the hook's STUN probe
            // (nat_traversal.cpp::SendStunProbe). Used to be set only
            // inside the match_start handler — meaning a spec instance
            // launched before joining any match wouldn't STUN, so hub's
            // user.udp_addr stayed at whatever earlier game STUN landed
            // (or empty), and spectator_incoming forwarded the wrong UDP
            // port to the host. The punch went nowhere.
            ::SetEnvironmentVariableA("FM2K_HUB_UDP_ADDR",
                                      (hub_host + ":7711").c_str());
            // Pull the cached Discord hub_token. Hub will reject the
            // hello with `auth_required` if missing/expired and the
            // launcher will surface the error in status_line.
            const auto cached = fm2k::discord_auth::LoadCached();
            // v0.2.8 routes WSS through Caddy on 443 by default; legacy
            // 2dfm.sytes.net hosts (set by users on older configs) keep
            // the direct-WS-on-7711 path so cutover is transparent.
            const bool use_legacy = (hub_host.find("sytes.net") != std::string::npos);
            const uint16_t      ws_port = use_legacy ? 7711  : 443;
            const char*         ws_path = use_legacy ? "/"   : "/ws";
            const bool          ws_tls  = !use_legacy;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Hub: connecting to %s%s:%u (%sWS) auth=%s",
                        hub_host.c_str(), ws_path, (unsigned)ws_port,
                        ws_tls ? "WSS via " : "",
                        cached.valid ? "present" : "missing");
            hs.client.SetStealth(hs.stealth);  // ensure the hello reflects the current toggle
            hs.client.Connect(hub_host, ws_port, ws_path, hs.my_nick,
                              cached.hub_token, ws_tls);
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
        // One-line UPnP port status (Phase 1). Reflects the off-thread
        // mapper's current state so the user can see whether they're
        // directly reachable. Only shown when we actually started a mapper
        // (non-loopback online session).
        if (port_mapper_) {
            fm2k::PortMapper::Status st = port_mapper_->Snapshot();
            switch (st.state) {
                case fm2k::PortMapper::State::Mapped:
                    ImGui::TextColored(ImVec4(0.3f, 0.8f, 0.4f, 1.0f),
                        "Port: open via UPnP (ext %u)",
                        static_cast<unsigned>(st.ext_udp_port));
                    break;
                case fm2k::PortMapper::State::Discovering:
                    ImGui::TextDisabled("Port: checking router (UPnP)...");
                    break;
                case fm2k::PortMapper::State::Cgnat:
                    ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.3f, 1.0f),
                        "Port: router behind CGNAT (UPnP can't help)");
                    break;
                case fm2k::PortMapper::State::NoIgd:
                    ImGui::TextDisabled("Port: closed (no UPnP router)");
                    break;
                case fm2k::PortMapper::State::Failed:
                    ImGui::TextDisabled("Port: closed (UPnP unavailable)");
                    break;
                case fm2k::PortMapper::State::Idle:
                default:
                    break;
            }
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
                // Spectate via the hub — RequestSpectate asks the hub to
                // grant us this match's host UDP addr; on grant we get a
                // K::SpectateGranted event which dispatches into
                // on_spectate_match (FM2K_RollbackClient.cpp) and ends up
                // in LaunchRemoteSpectator with default mode="current"
                // (CURRENT_MATCH snapshot-join path). The bf=8000+
                // input-replay drift that gated this button before is
                // sidestepped now: snapshot-join skips replay entirely
                // and consumes only post-anchor INPUTs. Tooltip kept
                // ambient — see docs/dev/spectator_smoke_test.md for the
                // observable checklist.
                if (ImGui::SmallButton(T("btn_spectate"))) {
                    if (hs.client.IsConnected()) {
                        // a->id is the user we're requesting to spectate;
                        // hub maps it to their current match and replies
                        // with the host's NAT-traversed UDP addr.
                        hs.client.RequestSpectate(a->id);
                        hs.status_line = "spectate request sent: " + a->nick;
                    }
                }
                ImGui::SetItemTooltip("Watch this match (FULL_SESSION — "
                                      "replays from session start; snapshot-join "
                                      "still baking)");
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
            // red 600 — distinct from gold without being garish); guest
            // (open-access non-patron, when the hub gate is lifted) gets grey
            // (0x9E9E9E, Material grey 500) — clearly not a paying tier.
            // Anything else (legacy hub, missing field) renders in the default
            // text color so stale clients don't turn invisible.
            const ImVec4 kTierTester(0x2C / 255.0f, 0x7B / 255.0f, 0xDB / 255.0f, 1.0f);
            const ImVec4 kTierThanks(0xFF / 255.0f, 0xBF / 255.0f, 0x03 / 255.0f, 1.0f);
            const ImVec4 kTierMonte (0xE5 / 255.0f, 0x39 / 255.0f, 0x35 / 255.0f, 1.0f);
            const ImVec4 kTierGuest (0x9E / 255.0f, 0x9E / 255.0f, 0x9E / 255.0f, 1.0f);
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
                } else if (u.tier == "guest") {
                    ImGui::TextColored(kTierGuest, "%s", u.nick.c_str());
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
                    // Self row: show overall W/L/D + a per-session
                    // counter that resets on launcher restart (Patrick
                    // asked for current-session record visibility).
                    // The "vs me" cell wouldn't make sense for self.
                    if (hs.my_wins >= 0) {
                        ImGui::Text("%d-%d-%d", hs.my_wins, hs.my_losses, hs.my_draws);
                    } else {
                        ImGui::TextDisabled("—");
                    }
                    if (hs.session_wins + hs.session_losses + hs.session_draws > 0) {
                        ImGui::SameLine();
                        ImGui::TextDisabled("(+%d-%d-%d)",
                                            hs.session_wins,
                                            hs.session_losses,
                                            hs.session_draws);
                        if (ImGui::IsItemHovered()) {
                            ImGui::SetTooltip("Current launcher-session "
                                              "wins-losses-draws (resets "
                                              "on launcher restart)");
                        }
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
                        EnsureRandomStageLoaded();  // per-game
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

