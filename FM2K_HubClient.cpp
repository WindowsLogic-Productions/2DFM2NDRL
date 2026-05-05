// FM2K Hub client — WinHTTP WebSocket transport.
//
// One I/O thread does the WS handshake then spawns a sender thread.
// The I/O thread itself owns the receive loop. Both push events
// onto a thread-safe inbox; the launcher's UI thread drains via
// HubClient::Poll() once per frame.
//
// JSON encode/decode is deliberately minimal — the message catalog
// in docs/FM2K_Matchmaking_Design.md §15.2 is small enough that
// hand-rolled extractors are simpler than vendoring a JSON lib.
// If that catalog grows, swap in nlohmann/json.

// WinHTTP WebSocket APIs (WinHttpWebSocketCompleteUpgrade etc.) are
// gated on _WIN32_WINNT >= 0x0602 (Windows 8). Project-wide setting
// is 0x0601 (Win7) for compatibility; bump only this TU.
#ifdef _WIN32_WINNT
#  undef _WIN32_WINNT
#endif
#define _WIN32_WINNT 0x0602
#ifdef WINVER
#  undef WINVER
#endif
#define WINVER 0x0602

#include "FM2K_HubClient.h"
#include <winhttp.h>

#include <SDL3/SDL_log.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#pragma comment(lib, "winhttp.lib")

namespace fm2k {

// ============================================================
// Minimal JSON helpers
// ============================================================

namespace {

std::string EscapeJsonString(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char esc[8];
                    std::snprintf(esc, sizeof(esc), "\\u%04x", static_cast<unsigned>(c));
                    out += esc;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

// Find `"key":` and return the position right after the colon (skipping whitespace).
// Returns std::string::npos if not found at outermost scope. Naive — doesn't
// guard against nested objects with the same key. Fine for our flat protocol.
size_t FindKey(const std::string& s, const std::string& key) {
    std::string needle = "\"" + key + "\"";
    size_t p = 0;
    while (true) {
        p = s.find(needle, p);
        if (p == std::string::npos) return std::string::npos;
        size_t after = p + needle.size();
        while (after < s.size() && (s[after] == ' ' || s[after] == '\t')) ++after;
        if (after < s.size() && s[after] == ':') {
            ++after;
            while (after < s.size() && (s[after] == ' ' || s[after] == '\t')) ++after;
            return after;
        }
        p = after;
    }
}

// Hex-nibble lookup for \uXXXX decode. Returns -1 on non-hex.
static int JsonHexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Append `cp` (any 21-bit code point) to `out` as UTF-8 bytes.
static void JsonAppendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out.push_back((char)cp);
    } else if (cp < 0x800) {
        out.push_back((char)(0xC0 | (cp >> 6)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back((char)(0xE0 | (cp >> 12)));
        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    } else {
        out.push_back((char)(0xF0 | (cp >> 18)));
        out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    }
}

std::string GetStr(const std::string& s, const std::string& key) {
    size_t p = FindKey(s, key);
    if (p == std::string::npos || p >= s.size() || s[p] != '"') return {};
    ++p;
    std::string out;
    while (p < s.size() && s[p] != '"') {
        if (s[p] == '\\' && p + 1 < s.size()) {
            char n = s[p + 1];
            switch (n) {
                case 'n': out += '\n'; p += 2; break;
                case 'r': out += '\r'; p += 2; break;
                case 't': out += '\t'; p += 2; break;
                case 'b': out += '\b'; p += 2; break;
                case 'f': out += '\f'; p += 2; break;
                case '"': out += '"';  p += 2; break;
                case '\\':out += '\\'; p += 2; break;
                case '/': out += '/';  p += 2; break;
                case 'u': {
                    // \uXXXX → UTF-8. Without this, Discord/hub-supplied
                    // nicks containing non-ASCII (e.g. "é" → "é")
                    // displayed as the literal "u00e9" in the UI.
                    if (p + 5 >= s.size()) { out += n; p += 2; break; }
                    int h0 = JsonHexNibble(s[p+2]);
                    int h1 = JsonHexNibble(s[p+3]);
                    int h2 = JsonHexNibble(s[p+4]);
                    int h3 = JsonHexNibble(s[p+5]);
                    if (h0 < 0 || h1 < 0 || h2 < 0 || h3 < 0) {
                        out += n; p += 2; break;
                    }
                    uint32_t cu = (uint32_t)((h0<<12)|(h1<<8)|(h2<<4)|h3);
                    p += 6;  // consumed \uXXXX
                    // Surrogate pair: high \uD800..\uDBFF + low \uDC00..\uDFFF
                    // combine into one code point.
                    if (cu >= 0xD800u && cu <= 0xDBFFu &&
                        p + 5 < s.size() && s[p] == '\\' && s[p+1] == 'u')
                    {
                        int l0 = JsonHexNibble(s[p+2]);
                        int l1 = JsonHexNibble(s[p+3]);
                        int l2 = JsonHexNibble(s[p+4]);
                        int l3 = JsonHexNibble(s[p+5]);
                        if (l0 >= 0 && l1 >= 0 && l2 >= 0 && l3 >= 0) {
                            uint32_t lo = (uint32_t)((l0<<12)|(l1<<8)|(l2<<4)|l3);
                            if (lo >= 0xDC00u && lo <= 0xDFFFu) {
                                uint32_t cp = 0x10000u +
                                    ((cu - 0xD800u) << 10) +
                                    (lo - 0xDC00u);
                                JsonAppendUtf8(out, cp);
                                p += 6;
                                break;
                            }
                        }
                    }
                    JsonAppendUtf8(out, cu);
                    break;
                }
                default:  out += n; p += 2; break;
            }
        } else {
            out += s[p++];
        }
    }
    return out;
}

int GetInt(const std::string& s, const std::string& key, int def = 0) {
    size_t p = FindKey(s, key);
    if (p == std::string::npos) return def;
    return std::atoi(s.c_str() + p);
}

// Pull a substring containing the JSON value following `key`. Returns empty
// if not found. Handles only object/array values where braces match — used
// for nested {peer:{...}}.
std::string GetSub(const std::string& s, const std::string& key) {
    size_t p = FindKey(s, key);
    if (p == std::string::npos) return {};
    char open = s[p];
    if (open != '{' && open != '[') return {};
    char close = (open == '{') ? '}' : ']';
    int depth = 0;
    bool in_str = false;
    bool esc = false;
    size_t start = p;
    for (; p < s.size(); ++p) {
        char c = s[p];
        if (esc) { esc = false; continue; }
        if (c == '\\') { esc = true; continue; }
        if (c == '"') { in_str = !in_str; continue; }
        if (in_str) continue;
        if (c == open) ++depth;
        else if (c == close) {
            --depth;
            if (depth == 0) return s.substr(start, p - start + 1);
        }
    }
    return {};
}

// Split a JSON array of objects into per-object substrings. Doesn't
// allocate any structured tree; we just hand back the chunks for
// further GetStr/GetInt extraction.
std::vector<std::string> SplitObjectArray(const std::string& arr) {
    std::vector<std::string> out;
    if (arr.size() < 2 || arr.front() != '[') return out;
    int depth = 0;
    bool in_str = false;
    bool esc = false;
    size_t start = std::string::npos;
    for (size_t i = 1; i + 1 < arr.size(); ++i) {
        char c = arr[i];
        if (esc) { esc = false; continue; }
        if (c == '\\') { esc = true; continue; }
        if (c == '"') { in_str = !in_str; continue; }
        if (in_str) continue;
        if (c == '{') {
            if (depth == 0) start = i;
            ++depth;
        } else if (c == '}') {
            --depth;
            if (depth == 0 && start != std::string::npos) {
                out.push_back(arr.substr(start, i - start + 1));
                start = std::string::npos;
            }
        }
    }
    return out;
}

HubUser ParseUser(const std::string& obj) {
    HubUser u;
    u.id          = GetStr(obj, "id");
    u.nick        = GetStr(obj, "nick");
    u.room_id     = GetStr(obj, "room_id");
    u.status      = GetStr(obj, "status");
    u.opponent_id = GetStr(obj, "opponent_id");
    u.rtt_ms      = GetInt(obj, "rtt_ms", 0);
    u.tier        = GetStr(obj, "tier");
    return u;
}

HubRoom ParseRoom(const std::string& obj) {
    HubRoom r;
    r.id         = GetStr(obj, "id");
    r.name       = GetStr(obj, "name");
    r.user_count = GetInt(obj, "user_count", 0);
    return r;
}

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

}  // namespace

// ============================================================
// HubClient
// ============================================================

HubClient::HubClient() = default;

HubClient::~HubClient() {
    Disconnect();
}

bool HubClient::Connect(const std::string& host, uint16_t port,
                        const std::string& path, const std::string& nick,
                        const std::string& hub_token) {
    if (running_.load()) return false;  // already connecting / connected
    // A previous failed Connect leaves io_ in a finished-but-joinable
    // state — IoThread returned, but std::thread doesn't auto-detach.
    // Reassigning over a joinable thread calls std::terminate(); join
    // first to clean up. The thread is already done so this is instant.
    if (io_.joinable()) {
        io_.join();
    }
    // Stash the token for IoThread → hello-send to read. Member state
    // because std::thread argument forwarding caps at 4 here without
    // pulling in another tuple wrapper.
    hub_token_ = hub_token;
    running_.store(true);
    io_ = std::thread(&HubClient::IoThread, this, host, port, path, nick);
    return true;
}

void HubClient::Disconnect() {
    if (!running_.exchange(false)) return;
    out_cv_.notify_all();
    if (ws_ != nullptr) {
        // Closing the handle interrupts WinHttpWebSocketReceive in the IO thread.
        WinHttpCloseHandle(ws_);
        ws_ = nullptr;
    }
    if (io_.joinable()) io_.join();
    CleanupHandles();
    connected_.store(false);
}

void HubClient::Poll(const std::function<void(const HubEvent&)>& on_event) {
    std::deque<HubEvent> drained;
    {
        std::lock_guard<std::mutex> lk(in_mtx_);
        drained.swap(inbox_);
    }
    for (auto& ev : drained) on_event(ev);
}

void HubClient::EnqueueOut(std::string msg) {
    {
        std::lock_guard<std::mutex> lk(out_mtx_);
        outbox_.push_back(std::move(msg));
    }
    out_cv_.notify_one();
}

void HubClient::EmitEvent(HubEvent ev) {
    std::lock_guard<std::mutex> lk(in_mtx_);
    inbox_.push_back(std::move(ev));
}

// ----- public outbound helpers — all just queue a JSON string -----

void HubClient::SendUdpAddr(const std::string& ip, int port) {
    std::string m = "{\"type\":\"udp_addr\",\"ip\":\"" + EscapeJsonString(ip)
                  + "\",\"port\":" + std::to_string(port) + "}";
    EnqueueOut(std::move(m));
}

void HubClient::ListRooms() {
    EnqueueOut("{\"type\":\"list_rooms\"}");
}

void HubClient::JoinRoom(const std::string& game_id, const std::string& display_name) {
    std::string m = "{\"type\":\"join_room\",\"game_id\":\"" + EscapeJsonString(game_id) + "\"";
    if (!display_name.empty()) {
        m += ",\"name\":\"" + EscapeJsonString(display_name) + "\"";
    }
    m += "}";
    EnqueueOut(std::move(m));
}

void HubClient::LeaveRoom() {
    EnqueueOut("{\"type\":\"leave_room\"}");
}

void HubClient::Challenge(const std::string& target_id) {
    EnqueueOut("{\"type\":\"challenge\",\"target_id\":\"" + EscapeJsonString(target_id) + "\"}");
}

void HubClient::CancelChallenge(const std::string& target_id) {
    EnqueueOut("{\"type\":\"cancel_challenge\",\"target_id\":\"" + EscapeJsonString(target_id) + "\"}");
}

void HubClient::AcceptChallenge(const std::string& challenger_id) {
    EnqueueOut("{\"type\":\"accept_challenge\",\"challenger_id\":\"" + EscapeJsonString(challenger_id) + "\"}");
}

void HubClient::DeclineChallenge(const std::string& challenger_id) {
    EnqueueOut("{\"type\":\"decline_challenge\",\"challenger_id\":\"" + EscapeJsonString(challenger_id) + "\"}");
}

void HubClient::MatchEnded() {
    EnqueueOut("{\"type\":\"match_ended\"}");
}

void HubClient::RequestSpectate(const std::string& target_id) {
    EnqueueOut("{\"type\":\"spectate_request\",\"target_id\":\"" +
               EscapeJsonString(target_id) + "\"}");
}

// ----- transport: I/O thread -----

void HubClient::IoThread(std::string host, uint16_t port,
                         std::string path, std::string nick) {
    auto fail = [&](const char* where) {
        DWORD err = GetLastError();
        // Map the most common WinHTTP failure codes to text the user
        // can act on. "WinHttpSendRequest failed (err=12029)" by
        // itself is opaque — the user keeps hitting these and asking
        // "what's going wrong?". Spelling out the cause + likely fix
        // surfaces the answer in the UI status_line.
        const char* hint = "";
        switch (err) {
            case 12002: hint = " (timeout — host unreachable or slow)"; break;
            case 12007: hint = " (name not resolved — typo in Host field?)"; break;
            case 12029: hint = " (cannot connect — host reached but TCP refused. "
                               "If hub is local, set Host to 127.0.0.1.)"; break;
            case 12030: hint = " (connection reset — hub closed unexpectedly)"; break;
            case 12152: hint = " (invalid server response — wrong protocol on port?)"; break;
            default: break;
        }
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "HubClient: %s failed (err=%lu)%s",
                     where, (unsigned long)err, hint);
        HubEvent ev;
        ev.kind = HubEvent::Kind::Error;
        ev.error = std::string(where) + " (err=" + std::to_string(err) + ")" + hint;
        EmitEvent(std::move(ev));
        ev.kind = HubEvent::Kind::Disconnected;
        EmitEvent(std::move(ev));
        running_.store(false);
        connected_.store(false);
    };

    session_ = WinHttpOpen(L"FM2K-Launcher/0.1",
                           WINHTTP_ACCESS_TYPE_NO_PROXY,
                           WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session_) { fail("WinHttpOpen"); return; }

    std::wstring whost = Widen(host);
    conn_ = WinHttpConnect(session_, whost.c_str(), port, 0);
    if (!conn_) { fail("WinHttpConnect"); return; }

    std::wstring wpath = Widen(path.empty() ? "/" : path);
    req_ = WinHttpOpenRequest(conn_, L"GET", wpath.c_str(), nullptr,
                              WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!req_) { fail("WinHttpOpenRequest"); return; }

    // Required to upgrade.
    if (!WinHttpSetOption(req_, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0)) {
        fail("WinHttpSetOption(UPGRADE)"); return;
    }

    if (!WinHttpSendRequest(req_, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        fail("WinHttpSendRequest"); return;
    }
    if (!WinHttpReceiveResponse(req_, nullptr)) {
        fail("WinHttpReceiveResponse"); return;
    }

    ws_ = WinHttpWebSocketCompleteUpgrade(req_, 0);
    if (!ws_) { fail("WinHttpWebSocketCompleteUpgrade"); return; }

    // Request handle no longer needed once we have the WS handle.
    WinHttpCloseHandle(req_);
    req_ = nullptr;

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "HubClient: WebSocket upgraded → %s:%u%s",
                host.c_str(), (unsigned)port, path.c_str());

    // Send hello first. Includes hub_token when the user has signed
    // in with Discord; the hub uses it to validate against the patron
    // role list before accepting the connection.
    {
        std::string hello = "{\"type\":\"hello\",\"nick\":\""
                            + EscapeJsonString(nick) + "\"";
        if (!hub_token_.empty()) {
            hello += ",\"hub_token\":\"" + EscapeJsonString(hub_token_) + "\"";
        }
        hello += "}";
        DWORD r = WinHttpWebSocketSend(ws_,
            WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
            hello.data(), (DWORD)hello.size());
        if (r != ERROR_SUCCESS) { fail("WinHttpWebSocketSend(hello)"); return; }
    }

    // Sender side-thread — drains outbox, sleeps on cv when empty.
    std::thread sender([this]() {
        while (running_.load()) {
            std::string msg;
            {
                std::unique_lock<std::mutex> lk(out_mtx_);
                out_cv_.wait(lk, [&]() { return !outbox_.empty() || !running_.load(); });
                if (!running_.load()) return;
                msg = std::move(outbox_.front());
                outbox_.pop_front();
            }
            DWORD r = WinHttpWebSocketSend(ws_,
                WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                msg.data(), (DWORD)msg.size());
            if (r != ERROR_SUCCESS) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "HubClient: WS send failed (err=%lu)", (unsigned long)r);
                running_.store(false);
                break;
            }
        }
    });

    // Receive loop — assembles fragmented UTF-8 messages and dispatches.
    std::string assembly;
    BYTE buf[4096];
    while (running_.load()) {
        DWORD bytes = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE bt;
        DWORD r = WinHttpWebSocketReceive(ws_, buf, sizeof(buf), &bytes, &bt);
        if (r != ERROR_SUCCESS) break;

        if (bt == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) break;

        if (bt == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE ||
            bt == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE) {
            assembly.append(reinterpret_cast<char*>(buf), bytes);
            if (bt == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE) {
                OnMessage(assembly);
                assembly.clear();
            }
        }
        // Binary frames not used by our protocol — drop silently.
    }

    running_.store(false);
    out_cv_.notify_all();
    sender.join();

    HubEvent ev;
    ev.kind = HubEvent::Kind::Disconnected;
    EmitEvent(std::move(ev));
    connected_.store(false);
}

void HubClient::CleanupHandles() {
    if (ws_)      { WinHttpCloseHandle(ws_);      ws_      = nullptr; }
    if (req_)     { WinHttpCloseHandle(req_);     req_     = nullptr; }
    if (conn_)    { WinHttpCloseHandle(conn_);    conn_    = nullptr; }
    if (session_) { WinHttpCloseHandle(session_); session_ = nullptr; }
}

// ----- inbound dispatch -----

void HubClient::OnMessage(const std::string& msg) {
    std::string type = GetStr(msg, "type");
    HubEvent ev;

    if (type == "ping") {
        EnqueueOut("{\"type\":\"pong\"}");
        return;
    }

    if (type == "hello_ack") {
        connected_.store(true);
        ev.kind = HubEvent::Kind::Connected;
        ev.user_id = GetStr(msg, "user_id");
        std::string rooms_arr = GetSub(msg, "rooms");
        if (!rooms_arr.empty()) {
            for (auto& obj : SplitObjectArray(rooms_arr)) ev.rooms.push_back(ParseRoom(obj));
        }
        EmitEvent(std::move(ev));
        return;
    }

    if (type == "room_list") {
        ev.kind = HubEvent::Kind::RoomList;
        std::string rooms_arr = GetSub(msg, "rooms");
        for (auto& obj : SplitObjectArray(rooms_arr)) ev.rooms.push_back(ParseRoom(obj));
        EmitEvent(std::move(ev));
        return;
    }

    if (type == "room_joined") {
        ev.kind = HubEvent::Kind::RoomJoined;
        std::string room_obj = GetSub(msg, "room");
        if (!room_obj.empty()) {
            HubRoom r = ParseRoom(room_obj);
            ev.room_id = r.id;
            ev.rooms.push_back(std::move(r));
        }
        std::string users_arr = GetSub(msg, "users");
        for (auto& uobj : SplitObjectArray(users_arr)) ev.users.push_back(ParseUser(uobj));
        EmitEvent(std::move(ev));
        return;
    }

    if (type == "room_left") {
        ev.kind = HubEvent::Kind::RoomLeft;
        EmitEvent(std::move(ev));
        return;
    }

    if (type == "user_joined") {
        ev.kind = HubEvent::Kind::UserJoined;
        ev.room_id = GetStr(msg, "room_id");
        ev.user = ParseUser(GetSub(msg, "user"));
        EmitEvent(std::move(ev));
        return;
    }

    if (type == "user_left") {
        ev.kind = HubEvent::Kind::UserLeft;
        ev.room_id = GetStr(msg, "room_id");
        ev.user_id = GetStr(msg, "user_id");
        EmitEvent(std::move(ev));
        return;
    }

    if (type == "user_status") {
        ev.kind = HubEvent::Kind::UserStatus;
        ev.user = ParseUser(GetSub(msg, "user"));
        EmitEvent(std::move(ev));
        return;
    }

    if (type == "user_rtt") {
        ev.kind = HubEvent::Kind::UserRtt;
        ev.user_id = GetStr(msg, "user_id");
        ev.rtt_ms  = GetInt(msg, "rtt_ms", 0);
        EmitEvent(std::move(ev));
        return;
    }

    if (type == "challenge_received") {
        ev.kind = HubEvent::Kind::ChallengeReceived;
        ev.challenge.from_id   = GetStr(msg, "from_id");
        ev.challenge.from_nick = GetStr(msg, "from_nick");
        ev.challenge.room_id   = GetStr(msg, "room_id");
        EmitEvent(std::move(ev));
        return;
    }

    if (type == "challenge_failed") {
        ev.kind = HubEvent::Kind::ChallengeFailed;
        ev.error = GetStr(msg, "reason");
        EmitEvent(std::move(ev));
        return;
    }

    if (type == "challenge_cancelled") {
        ev.kind = HubEvent::Kind::ChallengeCancelled;
        ev.user_id = GetStr(msg, "by_id");
        EmitEvent(std::move(ev));
        return;
    }

    if (type == "challenge_declined") {
        ev.kind = HubEvent::Kind::ChallengeDeclined;
        ev.user_id = GetStr(msg, "by_id");
        EmitEvent(std::move(ev));
        return;
    }

    if (type == "match_start") {
        ev.kind = HubEvent::Kind::MatchStart;
        ev.match.token = GetStr(msg, "token");
        ev.match.role  = GetStr(msg, "role");
        std::string peer_obj = GetSub(msg, "peer");
        ev.match.peer = ParseUser(peer_obj);
        std::string udp = GetSub(peer_obj, "udp_addr");
        if (!udp.empty()) {
            // udp_addr is either a [ip, port] tuple or null. We sent a
            // tuple via udp_addr; the hub forwards the (ip, port) pair.
            // hub.py serializes Python tuple → JSON array, so parse as
            // ["1.2.3.4", 12345].
            if (udp.front() == '[' || udp.front() == '"') {
                // Pull first quoted string and first integer in the substring.
                size_t a = udp.find('"');
                size_t b = (a == std::string::npos) ? a : udp.find('"', a + 1);
                if (a != std::string::npos && b != std::string::npos) {
                    ev.match.peer_udp_ip = udp.substr(a + 1, b - a - 1);
                }
                size_t c = udp.find(',');
                if (c != std::string::npos) ev.match.peer_udp_port = std::atoi(udp.c_str() + c + 1);
            }
        }
        std::string ws_addr = GetSub(peer_obj, "ws_addr");
        ev.match.peer_ws_addr = ws_addr;

        // Optional relay fallback advertised by the hub.
        std::string relay_obj = GetSub(msg, "relay");
        if (!relay_obj.empty()) {
            std::string addr_arr = GetSub(relay_obj, "addr");
            if (!addr_arr.empty()) {
                size_t a = addr_arr.find('"');
                size_t b = (a == std::string::npos) ? a : addr_arr.find('"', a + 1);
                if (a != std::string::npos && b != std::string::npos) {
                    ev.match.relay_ip = addr_arr.substr(a + 1, b - a - 1);
                }
                size_t c = addr_arr.find(',');
                if (c != std::string::npos) {
                    ev.match.relay_port = std::atoi(addr_arr.c_str() + c + 1);
                }
            }
            ev.match.relay_session_id = GetStr(relay_obj, "session_id");
        }
        EmitEvent(std::move(ev));
        return;
    }

    if (type == "spectate_grant") {
        ev.kind = HubEvent::Kind::SpectateGranted;
        ev.spectate.target_id     = GetStr(msg, "target_id");
        ev.spectate.target_nick   = GetStr(msg, "target_nick");
        ev.spectate.opponent_id   = GetStr(msg, "opponent_id");
        ev.spectate.opponent_nick = GetStr(msg, "opponent_nick");
        ev.spectate.host_ip       = GetStr(msg, "host_ip");
        ev.spectate.host_port     = GetInt(msg, "host_port", 0);
        EmitEvent(std::move(ev));
        return;
    }

    if (type == "spectate_denied") {
        ev.kind = HubEvent::Kind::SpectateDenied;
        ev.spectate.target_id = GetStr(msg, "target_id");
        ev.error              = GetStr(msg, "reason");
        EmitEvent(std::move(ev));
        return;
    }

    if (type == "peer_disconnected") {
        ev.kind = HubEvent::Kind::PeerDisconnected;
        EmitEvent(std::move(ev));
        return;
    }

    if (type == "error") {
        // Server-issued error — most commonly auth_required when the
        // launcher connects without a valid Discord hub_token. Surface
        // both reason and detail so the UI can show something useful.
        const std::string reason = GetStr(msg, "reason");
        const std::string detail = GetStr(msg, "detail");
        std::string combined = reason;
        if (!detail.empty()) {
            if (!combined.empty()) combined += ": ";
            combined += detail;
        }
        if (combined.empty()) combined = "hub error";
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "HubClient: hub error — %s", combined.c_str());
        ev.kind  = HubEvent::Kind::Error;
        ev.error = std::move(combined);
        EmitEvent(std::move(ev));
        return;
    }

    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "HubClient: unhandled message type='%s'", type.c_str());
}

}  // namespace fm2k
