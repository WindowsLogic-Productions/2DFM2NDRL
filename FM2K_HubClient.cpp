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
    // Treat a literal `null` as "absent" — same as the field being
    // omitted. Without this, atoi("null") returns 0 and a server
    // sending null for an unknown char/stage_id would silently parse
    // as "char/stage 0", which then resolves to the slot-0 entry of
    // the local KGT and produces a phantom row in the lobby panel.
    if (p + 4 <= s.size() && s.compare(p, 4, "null") == 0) return def;
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
                        const std::string& hub_token, bool secure) {
    if (running_.load()) return false;  // already connecting / connected
    // A previous failed Connect leaves io_ in a finished-but-joinable
    // state — IoThread returned, but std::thread doesn't auto-detach.
    // Reassigning over a joinable thread calls std::terminate(); join
    // first to clean up. The thread is already done so this is instant.
    if (io_.joinable()) {
        io_.join();
    }
    // Stash the token + secure flag for IoThread to read. Member state
    // because std::thread argument forwarding caps at 4 here without
    // pulling in another tuple wrapper.
    hub_token_  = hub_token;
    use_tls_    = secure;
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

void HubClient::Challenge(const std::string& target_id,
                          const MatchSettings& s) {
    // Build a flat match_settings object. Skip kUnset (-1) fields so
    // the wire payload stays compact and the hub forwards a smaller
    // blob to the target. Older launchers without the optional
    // fields just see the existing challenge_received shape.
    std::string m = "{\"type\":\"challenge\",\"target_id\":\"" +
                    EscapeJsonString(target_id) + "\"";
    bool any = false;
    auto add = [&](const char* key, int v) {
        if (v == -1) return;
        m += (any ? "," : ",\"match_settings\":{");
        any = true;
        m += "\"";
        m += key;
        m += "\":";
        m += std::to_string(v);
    };
    add("player0_cpu",      s.player0_cpu);
    add("player1_cpu",      s.player1_cpu);
    add("game_speed",       s.game_speed);
    add("hit_judge",        s.hit_judge);
    add("game_information", s.game_information);
    add("stage_nb",         s.stage_nb);
    add("joystick",         s.joystick);
    add("time",             s.time);
    add("exit_flag",        s.exit_flag);
    add("vs_mode",          s.vs_mode);
    add("vs_single_play",   s.vs_single_play);
    add("vs_team_play",     s.vs_team_play);
    // Random-stage extension (#56). random_seed == 0 means off; we
    // skip emitting any random_* fields when off so older hubs see
    // the same payload they did pre-#56.
    if (s.random_seed != 0) {
        // Cast to int for the JSON int writer; reinterpret the same
        // bits on the receiver (uint32_t = static_cast<uint32_t>(int)).
        m += (any ? "," : ",\"match_settings\":{");
        any = true;
        m += "\"random_seed\":" + std::to_string((int)s.random_seed);
    }
    add("random_stage_min", s.random_stage_min);
    add("random_stage_max", s.random_stage_max);
    if (any) m += "}";
    m += "}";
    EnqueueOut(std::move(m));
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

void HubClient::MatchResult(const std::string& match_id,
                            const std::string& outcome) {
    EnqueueOut("{\"type\":\"match_result\",\"match_id\":\"" +
               EscapeJsonString(match_id) + "\",\"outcome\":\"" +
               EscapeJsonString(outcome) + "\"}");
}

void HubClient::MatchResult(const std::string& match_id,
                            const std::string& outcome,
                            uint32_t p1_char_id, uint32_t p2_char_id) {
    MatchResult(match_id, outcome, p1_char_id, p2_char_id,
                std::string{}, std::string{});
}

void HubClient::MatchResult(const std::string& match_id,
                            const std::string& outcome,
                            uint32_t p1_char_id, uint32_t p2_char_id,
                            const std::string& p1_char_name,
                            const std::string& p2_char_name) {
    MatchResult(match_id, outcome, p1_char_id, p2_char_id,
                p1_char_name, p2_char_name,
                0xFFFFFFFFu, std::string{});
}

void HubClient::MatchResult(const std::string& match_id,
                            const std::string& outcome,
                            uint32_t p1_char_id, uint32_t p2_char_id,
                            const std::string& p1_char_name,
                            const std::string& p2_char_name,
                            uint32_t stage_id,
                            const std::string& stage_name) {
    std::string m = "{\"type\":\"match_result\",\"match_id\":\"" +
                    EscapeJsonString(match_id) + "\",\"outcome\":\"" +
                    EscapeJsonString(outcome) + "\"";
    if (p1_char_id != 0xFFFFFFFFu) {
        m += ",\"p1_char_id\":" + std::to_string(p1_char_id);
    }
    if (p2_char_id != 0xFFFFFFFFu) {
        m += ",\"p2_char_id\":" + std::to_string(p2_char_id);
    }
    if (!p1_char_name.empty()) {
        m += ",\"p1_char_name\":\"" + EscapeJsonString(p1_char_name) + "\"";
    }
    if (!p2_char_name.empty()) {
        m += ",\"p2_char_name\":\"" + EscapeJsonString(p2_char_name) + "\"";
    }
    if (stage_id != 0xFFFFFFFFu) {
        m += ",\"stage_id\":" + std::to_string(stage_id);
    }
    if (!stage_name.empty()) {
        m += ",\"stage_name\":\"" + EscapeJsonString(stage_name) + "\"";
    }
    m += "}";
    EnqueueOut(std::move(m));
}

void HubClient::MatchResult(const std::string& match_id,
                            const std::string& outcome,
                            uint32_t p1_char_id, uint32_t p2_char_id,
                            const std::string& p1_char_name,
                            const std::string& p2_char_name,
                            uint32_t stage_id,
                            const std::string& stage_name,
                            uint64_t session_id,
                            uint8_t  match_index_in_session,
                            const std::vector<RoundJson>& rounds) {
    // Fall back to schema-1 if no session metadata. Keeps the wire light
    // for legacy paths (e.g. early hook builds where session_id == 0).
    if (session_id == 0 && match_index_in_session == 0 && rounds.empty()) {
        MatchResult(match_id, outcome, p1_char_id, p2_char_id,
                    p1_char_name, p2_char_name, stage_id, stage_name);
        return;
    }

    std::string m = "{\"type\":\"match_result\",\"schema\":2,\"match_id\":\"" +
                    EscapeJsonString(match_id) + "\",\"outcome\":\"" +
                    EscapeJsonString(outcome) + "\"";
    if (p1_char_id != 0xFFFFFFFFu) {
        m += ",\"p1_char_id\":" + std::to_string(p1_char_id);
    }
    if (p2_char_id != 0xFFFFFFFFu) {
        m += ",\"p2_char_id\":" + std::to_string(p2_char_id);
    }
    if (!p1_char_name.empty()) {
        m += ",\"p1_char_name\":\"" + EscapeJsonString(p1_char_name) + "\"";
    }
    if (!p2_char_name.empty()) {
        m += ",\"p2_char_name\":\"" + EscapeJsonString(p2_char_name) + "\"";
    }
    if (stage_id != 0xFFFFFFFFu) {
        m += ",\"stage_id\":" + std::to_string(stage_id);
    }
    if (!stage_name.empty()) {
        m += ",\"stage_name\":\"" + EscapeJsonString(stage_name) + "\"";
    }
    // session_id as a 16-char hex string for compactness + readability in
    // the matches.json log (the high 32 bits are unix epoch seconds, low
    // 32 bits a random nonce — see SpectatorNode_AppendSessionId).
    char sid_buf[32];
    std::snprintf(sid_buf, sizeof(sid_buf), "%016llx",
                  static_cast<unsigned long long>(session_id));
    m += ",\"session_id\":\"";
    m += sid_buf;
    m += "\"";
    if (match_index_in_session > 0) {
        m += ",\"match_index_in_session\":" +
             std::to_string(static_cast<unsigned>(match_index_in_session));
    }
    if (!rounds.empty()) {
        m += ",\"rounds\":[";
        for (size_t i = 0; i < rounds.size(); ++i) {
            if (i > 0) m += ",";
            const RoundJson& r = rounds[i];
            const char* who = (r.winner_idx == 0) ? "p1"
                            : (r.winner_idx == 1) ? "p2" : "draw";
            m += "{\"winner\":\"";
            m += who;
            m += "\",\"frames\":" + std::to_string(r.frames_elapsed);
            m += ",\"p1_hp_left\":" + std::to_string(r.p1_hp_remaining);
            m += ",\"p2_hp_left\":" + std::to_string(r.p2_hp_remaining);
            m += "}";
        }
        m += "]";
    }
    m += "}";
    EnqueueOut(std::move(m));
}

void HubClient::QueryRecord(const std::string& opponent_id,
                            const std::string& game_id) {
    std::string m = "{\"type\":\"query_record\"";
    if (!opponent_id.empty())
        m += ",\"opponent_id\":\"" + EscapeJsonString(opponent_id) + "\"";
    if (!game_id.empty())
        m += ",\"game_id\":\"" + EscapeJsonString(game_id) + "\"";
    m += "}";
    EnqueueOut(std::move(m));
}

void HubClient::RequestRecentMatches(int limit) {
    if (limit < 1) limit = 50;
    EnqueueOut("{\"type\":\"recent_matches\",\"limit\":" +
               std::to_string(limit) + "}");
}

void HubClient::RequestCurrentMatches() {
    EnqueueOut("{\"type\":\"current_matches\"}");
}

void HubClient::ReportMatchProgress(const std::string& match_id,
                                    uint32_t p1_char_id, uint32_t p2_char_id,
                                    const std::string& p1_char_name,
                                    const std::string& p2_char_name,
                                    uint32_t stage_id,
                                    const std::string& stage_name) {
    std::string m = "{\"type\":\"match_progress\",\"match_id\":\"" +
                    EscapeJsonString(match_id) + "\"";
    if (p1_char_id != 0xFFFFFFFFu) {
        m += ",\"p1_char_id\":" + std::to_string(p1_char_id);
    }
    if (p2_char_id != 0xFFFFFFFFu) {
        m += ",\"p2_char_id\":" + std::to_string(p2_char_id);
    }
    if (!p1_char_name.empty()) {
        m += ",\"p1_char_name\":\"" + EscapeJsonString(p1_char_name) + "\"";
    }
    if (!p2_char_name.empty()) {
        m += ",\"p2_char_name\":\"" + EscapeJsonString(p2_char_name) + "\"";
    }
    if (stage_id != 0xFFFFFFFFu) {
        m += ",\"stage_id\":" + std::to_string(stage_id);
    }
    if (!stage_name.empty()) {
        m += ",\"stage_name\":\"" + EscapeJsonString(stage_name) + "\"";
    }
    m += "}";
    EnqueueOut(std::move(m));
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
    DWORD req_flags = use_tls_ ? WINHTTP_FLAG_SECURE : 0;
    req_ = WinHttpOpenRequest(conn_, L"GET", wpath.c_str(), nullptr,
                              WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                              req_flags);
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
        // Dev-only: FM2K_DEV_USER_SUFFIX lets a single Discord user run
        // multiple launcher instances with distinct lobby entries (for
        // testing). Hub honors this only for accounts on the
        // HUB_DEV_USER_IDS allowlist; everyone else's suffix is
        // ignored. Set differently per-launcher (e.g. =a in client A,
        // =b in client B) to see both in the lobby. Match records still
        // strip the suffix back to the bare dc_id, so stats aggregate
        // correctly regardless of how many dev launchers were involved.
        if (const char* dev_suffix = std::getenv("FM2K_DEV_USER_SUFFIX");
            dev_suffix && dev_suffix[0]) {
            hello += ",\"dev_suffix\":\"" + EscapeJsonString(dev_suffix) + "\"";
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
        // Optional match_settings sub-object (#54). Omitted by older
        // launchers; sentinel -1 across the struct flags "unknown" so
        // the accept modal falls back to the existing "this match
        // will use the host's defaults" wording.
        std::string ms = GetSub(msg, "match_settings");
        if (!ms.empty()) {
            auto& s = ev.challenge.settings;
            s.player0_cpu      = GetInt(ms, "player0_cpu",      -1);
            s.player1_cpu      = GetInt(ms, "player1_cpu",      -1);
            s.game_speed       = GetInt(ms, "game_speed",       -1);
            s.hit_judge        = GetInt(ms, "hit_judge",        -1);
            s.game_information = GetInt(ms, "game_information", -1);
            s.stage_nb         = GetInt(ms, "stage_nb",         -1);
            s.joystick         = GetInt(ms, "joystick",         -1);
            s.time             = GetInt(ms, "time",             -1);
            s.exit_flag        = GetInt(ms, "exit_flag",        -1);
            s.vs_mode          = GetInt(ms, "vs_mode",          -1);
            s.vs_single_play   = GetInt(ms, "vs_single_play",   -1);
            s.vs_team_play     = GetInt(ms, "vs_team_play",     -1);
            const int seed_signed = GetInt(ms, "random_seed", 0);
            s.random_seed         = static_cast<uint32_t>(seed_signed);
            s.random_stage_min    = GetInt(ms, "random_stage_min", -1);
            s.random_stage_max    = GetInt(ms, "random_stage_max", -1);
        }
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

        // Optional match_settings echoed by hub on match_start so both
        // peers (host + guest) apply the same authoritative config at
        // game-spawn time. Empty/absent on legacy hubs.
        std::string ms = GetSub(msg, "match_settings");
        if (!ms.empty()) {
            auto& s = ev.match.settings;
            s.player0_cpu      = GetInt(ms, "player0_cpu",      -1);
            s.player1_cpu      = GetInt(ms, "player1_cpu",      -1);
            s.game_speed       = GetInt(ms, "game_speed",       -1);
            s.hit_judge        = GetInt(ms, "hit_judge",        -1);
            s.game_information = GetInt(ms, "game_information", -1);
            s.stage_nb         = GetInt(ms, "stage_nb",         -1);
            s.joystick         = GetInt(ms, "joystick",         -1);
            s.time             = GetInt(ms, "time",             -1);
            s.exit_flag        = GetInt(ms, "exit_flag",        -1);
            s.vs_mode          = GetInt(ms, "vs_mode",          -1);
            s.vs_single_play   = GetInt(ms, "vs_single_play",   -1);
            s.vs_team_play     = GetInt(ms, "vs_team_play",     -1);
            // Random-stage fields (#56). random_seed parsed as 64-bit
            // signed because GetInt returns int; a 32-bit unsigned seed
            // (high bit set) would otherwise wrap negative. Re-cast to
            // uint32 on assign so xorshift seeding sees the same value
            // on both peers.
            const int seed_signed = GetInt(ms, "random_seed", 0);
            s.random_seed         = static_cast<uint32_t>(seed_signed);
            s.random_stage_min    = GetInt(ms, "random_stage_min", -1);
            s.random_stage_max    = GetInt(ms, "random_stage_max", -1);
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

    if (type == "spectator_incoming") {
        ev.kind = HubEvent::Kind::SpectatorIncoming;
        ev.spectator_incoming.spec_user_id  = GetStr(msg, "spec_user_id");
        ev.spectator_incoming.spec_nick     = GetStr(msg, "spec_nick");
        ev.spectator_incoming.spec_udp_ip   = GetStr(msg, "spec_udp_ip");
        ev.spectator_incoming.spec_udp_port = GetInt(msg, "spec_udp_port", 0);
        EmitEvent(std::move(ev));
        return;
    }

    if (type == "peer_disconnected") {
        ev.kind = HubEvent::Kind::PeerDisconnected;
        EmitEvent(std::move(ev));
        return;
    }

    if (type == "match_rotated") {
        // Hub minted a fresh in-flight match-id for the next FM2K
        // round in the same hub_session (no re-spawn — peers are
        // already in CSS). We just forward the new token; the
        // launcher updates current_match_token + resets per-match
        // flags so the next outcome publish commits cleanly.
        ev.kind = HubEvent::Kind::MatchRotated;
        ev.match.token = GetStr(msg, "new_token");
        EmitEvent(std::move(ev));
        return;
    }

    if (type == "record") {
        ev.kind = HubEvent::Kind::RecordReceived;
        ev.record.user_id     = GetStr(msg, "user_id");
        ev.record.opponent_id = GetStr(msg, "opponent_id");
        ev.record.game_id     = GetStr(msg, "game_id");
        ev.record.wins        = GetInt(msg, "wins", 0);
        ev.record.losses      = GetInt(msg, "losses", 0);
        ev.record.draws       = GetInt(msg, "draws", 0);
        std::string vs_arr    = GetSub(msg, "vs_breakdown");
        if (!vs_arr.empty()) {
            for (auto& obj : SplitObjectArray(vs_arr)) {
                HubEvent::VsRow row;
                row.opponent_id   = GetStr(obj, "opponent_id");
                row.opponent_nick = GetStr(obj, "opponent_nick");
                row.wins   = GetInt(obj, "wins", 0);
                row.losses = GetInt(obj, "losses", 0);
                row.draws  = GetInt(obj, "draws", 0);
                ev.record.vs_breakdown.push_back(std::move(row));
            }
        }
        EmitEvent(std::move(ev));
        return;
    }

    if (type == "recent_matches") {
        ev.kind = HubEvent::Kind::RecentMatchesReceived;
        std::string arr = GetSub(msg, "matches");
        if (!arr.empty()) {
            for (auto& obj : SplitObjectArray(arr)) {
                HubEvent::MatchRow row;
                row.id        = GetStr(obj, "id");
                row.p1_id     = GetStr(obj, "p1_id");
                row.p1_nick   = GetStr(obj, "p1_nick");
                row.p2_id     = GetStr(obj, "p2_id");
                row.p2_nick   = GetStr(obj, "p2_nick");
                row.game_id   = GetStr(obj, "game_id");
                row.winner_id = GetStr(obj, "winner_id");
                // finished_at is a float in JSON; GetInt lossy but
                // sufficient for sorting / display purposes.
                row.finished_at = (double)GetInt(obj, "finished_at", 0);
                // Char + stage fields — server sends null when peers
                // disagreed/omitted; GetInt returns the default (-1)
                // for null so the row stays clean for UI fallbacks.
                row.p1_char_id   = GetInt(obj, "p1_char_id", -1);
                row.p2_char_id   = GetInt(obj, "p2_char_id", -1);
                row.p1_char_name = GetStr(obj, "p1_char_name");
                row.p2_char_name = GetStr(obj, "p2_char_name");
                row.stage_id     = GetInt(obj, "stage_id", -1);
                row.stage_name   = GetStr(obj, "stage_name");
                ev.recent_matches.push_back(std::move(row));
            }
        }
        EmitEvent(std::move(ev));
        return;
    }

    // In-progress match payloads — used by the lobby panel. Snapshot
    // form (current_matches response, list of objects) and live forms
    // (match_in_progress_started/updated, single object) parse the
    // same shape via this helper.
    auto parse_in_progress = [](const std::string& obj) -> HubEvent::MatchInProgress {
        HubEvent::MatchInProgress r;
        r.token        = GetStr(obj, "token");
        r.p1_id        = GetStr(obj, "p1_id");
        r.p1_nick      = GetStr(obj, "p1_nick");
        r.p2_id        = GetStr(obj, "p2_id");
        r.p2_nick      = GetStr(obj, "p2_nick");
        r.game_id      = GetStr(obj, "game_id");
        r.started_at   = (double)GetInt(obj, "started_at", 0);
        r.p1_char_id   = GetInt(obj, "p1_char_id", -1);
        r.p2_char_id   = GetInt(obj, "p2_char_id", -1);
        r.p1_char_name = GetStr(obj, "p1_char_name");
        r.p2_char_name = GetStr(obj, "p2_char_name");
        r.stage_id     = GetInt(obj, "stage_id", -1);
        r.stage_name   = GetStr(obj, "stage_name");
        return r;
    };

    if (type == "current_matches") {
        ev.kind = HubEvent::Kind::CurrentMatchesReceived;
        std::string arr = GetSub(msg, "matches");
        if (!arr.empty()) {
            for (auto& obj : SplitObjectArray(arr)) {
                ev.current_matches.push_back(parse_in_progress(obj));
            }
        }
        EmitEvent(std::move(ev));
        return;
    }

    if (type == "match_in_progress_started" ||
        type == "match_in_progress_updated") {
        ev.kind = (type == "match_in_progress_started")
                  ? HubEvent::Kind::MatchInProgressStarted
                  : HubEvent::Kind::MatchInProgressUpdated;
        std::string sub = GetSub(msg, "match");
        if (!sub.empty()) {
            ev.current_match_update = parse_in_progress(sub);
        }
        EmitEvent(std::move(ev));
        return;
    }

    if (type == "match_in_progress_ended") {
        ev.kind = HubEvent::Kind::MatchInProgressEnded;
        ev.current_match_token = GetStr(msg, "token");
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
