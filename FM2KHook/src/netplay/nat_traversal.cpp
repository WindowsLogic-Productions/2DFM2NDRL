// FM2K NAT traversal — STUN probe + 0xCD demux. Phase-1 scaffold.
//
// Reads:
//   FM2K_HUB_UDP_ADDR    — "ip:port" of the hub's UDP STUN responder.
//                           Same address as the WebSocket lobby in
//                           Phase 1; can split later if needed.
//   FM2K_HUB_USER_ID     — 12-char hex id assigned by the lobby on
//                           hello_ack. Identifies this client to the
//                           hub when its STUN probe arrives.
//   FM2K_HUB_MATCH_TOKEN — 16 hex chars (8 bytes) — shared with peer
//                           via match_start. Used to authenticate
//                           inbound CTRL_PUNCH packets.
//
// Real burst-punch driver (port of bbbr_holepunch.cpp's burst+priority
// approach) is the next deliverable; see docs/FM2K_Matchmaking_Design.md
// §15.4. For now StartPunch/HandleDatagram log intent and don't yet
// open the pinhole.

#include "nat_traversal.h"
#include "control_channel.h"

#include <SDL3/SDL_log.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include <ws2tcpip.h>
#include <mmsystem.h>

namespace fm2k::nat {

namespace {

constexpr uint8_t MAGIC          = 0xCD;
constexpr uint8_t TAG_PROBE      = 0x01;
constexpr uint8_t TAG_ACK        = 0x02;
constexpr uint8_t TAG_CTRL_PUNCH = 0x10;

constexpr size_t USER_ID_LEN     = 12;
constexpr size_t MATCH_TOKEN_LEN = 16;

bool g_have_reflexive = false;
sockaddr_in g_reflexive{};

uint8_t g_match_token[MATCH_TOKEN_LEN] = {};
bool    g_match_token_set = false;

// Burst-punch state. The burst thread loops until either the 30
// packets are out OR g_punching latches false (set by a successful
// authenticated peer punch landing in HandleDatagram).
std::atomic<bool> g_punching{false};
std::thread       g_punch_thread;
sockaddr_in       g_punch_peer{};

constexpr int PUNCH_PACKETS  = 30;
constexpr int PUNCH_PERIOD_MS = 10;   // ~300 ms total burst

bool ParseHostPort(const std::string& s, std::string& host, uint16_t& port) {
    auto colon = s.rfind(':');
    if (colon == std::string::npos) return false;
    host = s.substr(0, colon);
    int p = std::atoi(s.c_str() + colon + 1);
    if (p <= 0 || p > 65535) return false;
    port = static_cast<uint16_t>(p);
    return true;
}

}  // namespace

bool SendStunProbe() {
    const char* hub_addr_str = std::getenv("FM2K_HUB_UDP_ADDR");
    const char* user_id      = std::getenv("FM2K_HUB_USER_ID");
    if (!hub_addr_str || !user_id) {
        return false;
    }

    std::string host;
    uint16_t port = 0;
    if (!ParseHostPort(hub_addr_str, host, port)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "NAT: invalid FM2K_HUB_UDP_ADDR='%s'", hub_addr_str);
        return false;
    }

    sockaddr_in hub{};
    hub.sin_family = AF_INET;
    hub.sin_port   = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &hub.sin_addr) != 1) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "NAT: inet_pton failed for hub host='%s'", host.c_str());
        return false;
    }

    uint8_t pkt[2 + USER_ID_LEN] = {MAGIC, TAG_PROBE};
    size_t n = std::strlen(user_id);
    if (n > USER_ID_LEN) n = USER_ID_LEN;
    std::memcpy(pkt + 2, user_id, n);
    // Pad with NUL is implicit — pkt was zero-initialized by the brace init.

    SOCKET sock = ControlChannel_GetSocket();
    if (sock == INVALID_SOCKET) return false;

    int sent = sendto(sock, reinterpret_cast<const char*>(pkt), sizeof(pkt), 0,
                      reinterpret_cast<sockaddr*>(&hub), sizeof(hub));
    if (sent != (int)sizeof(pkt)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "NAT: STUN probe sendto failed (err=%d)", WSAGetLastError());
        return false;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "NAT: STUN probe sent to %s:%u (user_id=%s)",
        host.c_str(), (unsigned)port, user_id);
    return true;
}

void StartPunch(uint32_t peer_ip_be, uint16_t peer_port,
                const uint8_t match_token[16]) {
    char ip_str[INET_ADDRSTRLEN] = {};
    in_addr ia{};
    ia.s_addr = peer_ip_be;
    inet_ntop(AF_INET, &ia, ip_str, sizeof(ip_str));

    if (match_token) {
        std::memcpy(g_match_token, match_token, MATCH_TOKEN_LEN);
        g_match_token_set = true;
    }

    // Stop any prior burst (e.g. user reconnecting / new match) before
    // launching a fresh one. join() is bounded — the loop checks
    // g_punching every iteration.
    if (g_punching.exchange(false) && g_punch_thread.joinable()) {
        g_punch_thread.join();
    }

    g_punch_peer = {};
    g_punch_peer.sin_family      = AF_INET;
    g_punch_peer.sin_addr.s_addr = peer_ip_be;
    g_punch_peer.sin_port        = htons(peer_port);

    g_punching.store(true);
    g_punch_thread = std::thread([ip_str_copy = std::string(ip_str), peer_port]() {
        // Boost only this thread's priority — process-wide boost would
        // starve the game's main loop. timeBeginPeriod(1) tightens
        // Sleep granularity so 10 ms means ~10 ms instead of ~16 ms
        // (Windows default scheduler tick).
        HANDLE th = GetCurrentThread();
        int prev_pri = GetThreadPriority(th);
        SetThreadPriority(th, THREAD_PRIORITY_TIME_CRITICAL);
        timeBeginPeriod(1);

        SOCKET sock = ControlChannel_GetSocket();
        if (sock == INVALID_SOCKET) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "NAT: punch aborted — control socket unavailable");
            timeEndPeriod(1);
            SetThreadPriority(th, prev_pri);
            g_punching.store(false);
            return;
        }

        uint8_t pkt[2 + MATCH_TOKEN_LEN] = {MAGIC, TAG_CTRL_PUNCH};
        std::memcpy(pkt + 2, g_match_token, MATCH_TOKEN_LEN);

        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "NAT: burst punch -> %s:%u (%d packets, ~%d ms)",
            ip_str_copy.c_str(), (unsigned)peer_port,
            PUNCH_PACKETS, PUNCH_PACKETS * PUNCH_PERIOD_MS);

        int sent_ok = 0;
        for (int i = 0; i < PUNCH_PACKETS; ++i) {
            if (!g_punching.load()) break;  // peer latched, stop early
            int sent = sendto(sock,
                              reinterpret_cast<const char*>(pkt), sizeof(pkt), 0,
                              reinterpret_cast<sockaddr*>(&g_punch_peer),
                              sizeof(g_punch_peer));
            if (sent == (int)sizeof(pkt)) ++sent_ok;
            Sleep(PUNCH_PERIOD_MS);
        }

        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "NAT: burst complete — %d/%d sent (%s)",
            sent_ok, PUNCH_PACKETS,
            g_punching.load() ? "no peer ack yet" : "peer-latch fired");

        timeEndPeriod(1);
        SetThreadPriority(th, prev_pri);
        g_punching.store(false);
    });
}

void HandleDatagram(const uint8_t* data, size_t len, const sockaddr_in& from) {
    if (len < 2 || data[0] != MAGIC) return;
    const uint8_t tag = data[1];

    char from_ip[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &from.sin_addr, from_ip, sizeof(from_ip));

    switch (tag) {
        case TAG_ACK: {
            // Hub's STUN response. data[2..5] = ip_be, data[6..7] = port_be.
            if (len < 2 + 4 + 2) return;
            uint32_t ip_be;
            uint16_t port_be;
            std::memcpy(&ip_be,   data + 2, 4);
            std::memcpy(&port_be, data + 6, 2);
            in_addr ia{};
            ia.s_addr = ip_be;
            char ip_str[INET_ADDRSTRLEN] = {};
            inet_ntop(AF_INET, &ia, ip_str, sizeof(ip_str));
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "NAT: STUN ack — reflexive %s:%u",
                ip_str, (unsigned)ntohs(port_be));
            g_reflexive = {};
            g_reflexive.sin_family = AF_INET;
            g_reflexive.sin_addr.s_addr = ip_be;
            g_reflexive.sin_port = port_be;
            g_have_reflexive = true;
            return;
        }
        case TAG_CTRL_PUNCH: {
            // Authenticated punch from peer. data[2..2+16) = match_token.
            if (len < 2 + MATCH_TOKEN_LEN) return;
            if (!g_match_token_set) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "NAT: CTRL_PUNCH from %s:%u dropped — no local token set",
                    from_ip, (unsigned)ntohs(from.sin_port));
                return;
            }
            if (std::memcmp(data + 2, g_match_token, MATCH_TOKEN_LEN) != 0) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "NAT: CTRL_PUNCH from %s:%u dropped — token mismatch",
                    from_ip, (unsigned)ntohs(from.sin_port));
                return;
            }
            // First authentic peer punch: latch the address into
            // control_channel's gameplay peer slot and signal the
            // burst thread to stop early. The 0xCC HELLO loop will
            // start hitting the right address from this point on, and
            // the existing peer-learning code in RawReceive's 0xCC
            // branch will keep tracking it across NAT remapping.
            ControlChannel_LatchPeerAddr(from);
            g_punching.store(false);
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "NAT: CTRL_PUNCH from %s:%u authenticated — peer latched",
                from_ip, (unsigned)ntohs(from.sin_port));
            return;
        }
        default:
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "NAT: unknown 0xCD tag=0x%02X from %s:%u (ignoring)",
                (unsigned)tag, from_ip, (unsigned)ntohs(from.sin_port));
            return;
    }
}

}  // namespace fm2k::nat
