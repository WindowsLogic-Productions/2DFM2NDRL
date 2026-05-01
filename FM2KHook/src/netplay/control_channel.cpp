// Control Channel - Multiplexed UDP Socket Implementation
#include "control_channel.h"
#include "globals.h"
#include "gekkonet.h"
#include "spectator_node.h"
#include "nat_traversal.h"
#include <SDL3/SDL_log.h>
#include <vector>
#include <cstring>
#include <chrono>

// =============================================================================
// INTERNAL STATE
// =============================================================================

static SOCKET g_socket = INVALID_SOCKET;
static sockaddr_in g_local_sockaddr = {};
static sockaddr_in g_remote_sockaddr = {};
static bool g_socket_initialized = false;

// Sequence numbers for reliable messaging
static uint16_t g_send_seq = 0;
static uint16_t g_recv_seq = 0;
static uint16_t g_recv_ack = 0;

// Connection state
static bool g_connected = false;
static uint32_t g_last_recv_time = 0;
static uint32_t g_last_ping_time = 0;
static uint32_t g_rtt_ms = 0;
static uint32_t g_ping_send_time = 0;

// Player ID (set from globals)
static uint8_t g_local_player_id = 0;

// Callback for received control messages
static ControlMsgCallback g_msg_callback = nullptr;

// Buffer for receiving packets
static constexpr size_t RECV_BUFFER_SIZE = 2048;
static char g_recv_buffer[RECV_BUFFER_SIZE];

// Queue for GekkoNet packets (filtered from receive)
static std::vector<std::pair<std::vector<char>, sockaddr_in>> g_gekko_packet_queue;

// GekkoNet result pointers for adapter (actual results are heap-allocated)
static std::vector<GekkoNetResult*> g_gekko_result_ptrs;

// Get current time in milliseconds
static uint32_t GetTimeMs() {
    auto now = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch());
    return static_cast<uint32_t>(ms.count());
}

// =============================================================================
// SOCKET MANAGEMENT
// =============================================================================

bool NetSocket_Init(uint16_t local_port, const char* remote_addr) {
    if (g_socket_initialized) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "NetSocket: Already initialized");
        return true;
    }

    // Initialize Winsock
    WSADATA wsa_data;
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "NetSocket: WSAStartup failed: %d", WSAGetLastError());
        return false;
    }

    // Create UDP socket
    g_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_socket == INVALID_SOCKET) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "NetSocket: socket() failed: %d", WSAGetLastError());
        WSACleanup();
        return false;
    }

    // Set non-blocking
    u_long mode = 1;
    if (ioctlsocket(g_socket, FIONBIO, &mode) != 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "NetSocket: ioctlsocket() failed: %d", WSAGetLastError());
        closesocket(g_socket);
        g_socket = INVALID_SOCKET;
        WSACleanup();
        return false;
    }

    // SO_REUSEADDR — allows rebinding to a port still in TIME_WAIT
    // from a previous run. Without this, restarting the launcher
    // within ~30s of a previous session hits WSAEADDRINUSE (10048).
    BOOL reuse = TRUE;
    setsockopt(g_socket, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    // Bind to local port
    g_local_sockaddr.sin_family = AF_INET;
    g_local_sockaddr.sin_addr.s_addr = INADDR_ANY;
    g_local_sockaddr.sin_port = htons(local_port);

    if (bind(g_socket, (sockaddr*)&g_local_sockaddr, sizeof(g_local_sockaddr)) == SOCKET_ERROR) {
        int err = WSAGetLastError();
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "NetSocket: bind() failed on port %d (err=%d). %s",
            (int)local_port, err,
            err == WSAEADDRINUSE
                ? "Port already in use — another launcher instance is on this port. "
                  "If running two launchers on the same machine, configure each "
                  "with a different FM2K_LOCAL_PORT (e.g. 7000 and 7001)."
                : "");
        closesocket(g_socket);
        g_socket = INVALID_SOCKET;
        WSACleanup();
        return false;
    }

    // Parse remote address (format: "ip:port"). Empty/missing is allowed for
    // pure hosts: we'll learn the peer from the first inbound HELLO.
    g_remote_sockaddr = {};
    g_remote_sockaddr.sin_family = AF_INET;
    bool have_remote = false;
    if (remote_addr && remote_addr[0]) {
        std::string addr_str(remote_addr);
        size_t colon_pos = addr_str.find(':');
        if (colon_pos != std::string::npos) {
            std::string ip = addr_str.substr(0, colon_pos);
            uint16_t port = static_cast<uint16_t>(std::stoi(addr_str.substr(colon_pos + 1)));
            if (inet_pton(AF_INET, ip.c_str(), &g_remote_sockaddr.sin_addr) == 1 && port != 0) {
                g_remote_sockaddr.sin_port = htons(port);
                have_remote = true;
            }
        }
        if (!have_remote) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "NetSocket: Ignoring invalid remote '%s', waiting for peer HELLO",
                        remote_addr);
            g_remote_sockaddr.sin_port = 0;
            g_remote_sockaddr.sin_addr.s_addr = 0;
        }
    }

    g_socket_initialized = true;
    g_local_player_id = static_cast<uint8_t>(g_player_index);
    g_last_recv_time = GetTimeMs();

    if (have_remote) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "NetSocket: Initialized on port %d, remote=%s",
                    local_port, remote_addr);
    } else {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "NetSocket: Initialized on port %d, awaiting peer HELLO", local_port);
    }

    return true;
}

void NetSocket_Shutdown() {
    if (!g_socket_initialized) return;

    if (g_socket != INVALID_SOCKET) {
        closesocket(g_socket);
        g_socket = INVALID_SOCKET;
    }

    WSACleanup();
    g_socket_initialized = false;
    g_connected = false;

    // Clear queues
    g_gekko_packet_queue.clear();
    g_gekko_result_ptrs.clear();

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "NetSocket: Shutdown");
}

bool NetSocket_IsInitialized() {
    return g_socket_initialized;
}

SOCKET NetSocket_GetHandle() {
    return g_socket;
}

const sockaddr_in* NetSocket_GetRemoteAddr() {
    return &g_remote_sockaddr;
}

// =============================================================================
// RAW SEND/RECEIVE
// =============================================================================

static void RawSend(const void* data, size_t len) {
    if (g_socket == INVALID_SOCKET) return;

    // Relay mode: wrap every gameplay byte in a 0xCF envelope and ship
    // to the hub-supplied relay endpoint. The relay forwards by
    // session_id to the other peer, who unwraps in their own
    // RawReceive. Direct g_remote_sockaddr is bypassed entirely.
    if (::fm2k::nat::IsRelayMode()) {
        const sockaddr_in* relay = ::fm2k::nat::GetRelayAddr();
        if (!relay) return;
        // 18-byte header (0xCF 0x01 + 16-byte session_id) + payload.
        // Most FM2K control / GekkoNet packets are well under 1500B so
        // a 2KB stack buffer is plenty.
        uint8_t wrapped[2048];
        size_t wrapped_len = ::fm2k::nat::WrapForRelay(
            reinterpret_cast<const uint8_t*>(data), len,
            wrapped, sizeof(wrapped));
        if (wrapped_len == 0) return;  // payload too large
        int result = sendto(g_socket,
                            reinterpret_cast<const char*>(wrapped),
                            (int)wrapped_len, 0,
                            reinterpret_cast<const sockaddr*>(relay),
                            sizeof(*relay));
        if (result == SOCKET_ERROR) {
            int err = WSAGetLastError();
            if (err != WSAEWOULDBLOCK) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                    "NetSocket: relay sendto failed: %d", err);
            }
        }
        return;
    }

    if (g_remote_sockaddr.sin_port == 0) return;  // peer address not known yet (host waiting for client)

    int result = sendto(g_socket, (const char*)data, (int)len, 0,
                        (sockaddr*)&g_remote_sockaddr, sizeof(g_remote_sockaddr));
    if (result == SOCKET_ERROR) {
        int err = WSAGetLastError();
        if (err != WSAEWOULDBLOCK) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "NetSocket: sendto failed: %d", err);
        }
    }
}

// Receive all pending packets, filter control vs GekkoNet
static void RawReceive() {
    if (g_socket == INVALID_SOCKET) return;

    while (true) {
        sockaddr_in from_addr;
        int from_len = sizeof(from_addr);

        int recv_len = recvfrom(g_socket, g_recv_buffer, RECV_BUFFER_SIZE, 0,
                                (sockaddr*)&from_addr, &from_len);

        if (recv_len == SOCKET_ERROR) {
            int err = WSAGetLastError();
            // WSAEWOULDBLOCK = no data available (normal)
            // WSAECONNRESET (10054) = remote port not open yet (normal during connection)
            if (err != WSAEWOULDBLOCK && err != WSAECONNRESET) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "NetSocket: recvfrom failed: %d", err);
            }
            break;  // No more data
        }

        if (recv_len == 0) break;

        g_last_recv_time = GetTimeMs();

        // Relay-envelope unwrap. If this is a 0xCF packet for our
        // configured relay session, strip the 18-byte header and treat
        // the inner bytes as if they arrived directly from the peer.
        // Skip peer-address learning for the inner packet — the actual
        // sockaddr is the relay, which we don't want latched as peer.
        bool from_relay = false;
        const uint8_t* eff_data = reinterpret_cast<const uint8_t*>(g_recv_buffer);
        size_t eff_len = static_cast<size_t>(recv_len);
        const uint8_t* inner = nullptr;
        size_t inner_len = 0;
        if (recv_len >= 1 && static_cast<uint8_t>(g_recv_buffer[0]) == 0xCF) {
            if (::fm2k::nat::UnwrapFromRelay(eff_data, eff_len, &inner, &inner_len)) {
                eff_data = inner;
                eff_len  = inner_len;
                from_relay = true;
            } else {
                continue;  // bad envelope or wrong session — drop
            }
        }
        const uint8_t first = eff_len ? eff_data[0] : 0;

        // Check if this is a control packet (magic byte 0xCC)
        if (eff_len >= 1 && first == CTRL_MAGIC) {
            // Peer-address learning. Skip when the packet came in via
            // the relay envelope — the actual sockaddr is the relay,
            // and latching the relay as g_remote_sockaddr would later
            // cause RawSend (in non-relay mode) to send to the relay
            // instead of the peer. In relay mode we don't need a
            // peer addr at all (RawSend's relay branch ignores it).
            if (!from_relay && !g_connected &&
                (g_remote_sockaddr.sin_addr.s_addr != from_addr.sin_addr.s_addr ||
                 g_remote_sockaddr.sin_port != from_addr.sin_port)) {
                char ip_buf[INET_ADDRSTRLEN] = {};
                inet_ntop(AF_INET, &from_addr.sin_addr, ip_buf, sizeof(ip_buf));
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "NetSocket: Learned peer address %s:%u (was %u)",
                            ip_buf, ntohs(from_addr.sin_port),
                            ntohs(g_remote_sockaddr.sin_port));
                g_remote_sockaddr = from_addr;
            }

            // Control packet - process immediately
            if (eff_len >= sizeof(CtrlPacketHeader)) {
                const CtrlPacket* packet = reinterpret_cast<const CtrlPacket*>(eff_data);

                // Update ack
                if (packet->header.seq > g_recv_seq) {
                    g_recv_seq = packet->header.seq;
                }
                g_recv_ack = packet->header.ack;

                // Handle PING/PONG internally before callback
                if (packet->header.type == CtrlMsg::PING) {
                    // Respond with PONG containing the sender's timestamp
                    CtrlPacket pong = {};
                    pong.header.type = CtrlMsg::PONG;
                    pong.data.sync.frame = packet->data.sync.frame;  // Echo back sender's time
                    ControlChannel_Send(pong);
                } else if (packet->header.type == CtrlMsg::PONG) {
                    // Calculate RTT from echoed timestamp
                    ControlChannel_HandlePong(packet->data.sync.frame);
                }

                // Forward all messages to callback (including PING/PONG for logging)
                if (g_msg_callback) {
                    g_msg_callback(packet, from_addr);
                }
            }
        } else if (eff_len >= 1 && first == 0xCD) {
            // NAT-layer datagram (0xCD) — STUN ack or peer punch probe.
            // Defined in nat_traversal.h. Returning here keeps the byte
            // out of GekkoNet's queue and the spectator path.
            ::fm2k::nat::HandleDatagram(eff_data, eff_len, from_addr);
        } else if (eff_len >= 1 && first == SPEC_DATA_MAGIC) {
            // Spectator-tree datagram (0xCE) — variable-length payload.
            SpectatorNode_HandleSpecData(eff_data, eff_len, from_addr);
        } else {
            // GekkoNet packet - queue for adapter
            std::vector<char> pkt_data(
                reinterpret_cast<const char*>(eff_data),
                reinterpret_cast<const char*>(eff_data) + eff_len);
            g_gekko_packet_queue.push_back({std::move(pkt_data), from_addr});
        }
    }
}

// =============================================================================
// CONTROL CHANNEL
// =============================================================================

SOCKET ControlChannel_GetSocket() {
    return g_socket;
}

void ControlChannel_LatchPeerAddr(const sockaddr_in& peer) {
    char ip[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
    if (g_remote_sockaddr.sin_addr.s_addr != peer.sin_addr.s_addr ||
        g_remote_sockaddr.sin_port        != peer.sin_port) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "ControlChannel: peer addr latched -> %s:%u (was %u)",
            ip, (unsigned)ntohs(peer.sin_port),
            (unsigned)ntohs(g_remote_sockaddr.sin_port));
    }
    g_remote_sockaddr = peer;
}

void ControlChannel_Poll() {
    if (!g_socket_initialized) return;

    // Receive all pending packets
    RawReceive();

    // Check for timeout
    uint32_t now = GetTimeMs();
    if (g_connected && (now - g_last_recv_time) > PING_TIMEOUT_MS) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "ControlChannel: Connection timed out");
        g_connected = false;
    }

    // Send periodic ping
    if (g_connected && (now - g_last_ping_time) > PING_INTERVAL_MS) {
        ControlChannel_SendPing();
        g_last_ping_time = now;
    }
}

void ControlChannel_Send(const CtrlPacket& packet) {
    if (!g_socket_initialized) return;

    // Make a copy to modify header
    CtrlPacket pkt = packet;
    pkt.header.magic = CTRL_MAGIC;
    pkt.header.seq = ++g_send_seq;
    pkt.header.ack = g_recv_seq;
    pkt.header.player_id = g_local_player_id;

    RawSend(&pkt, sizeof(pkt));
}

void ControlChannel_SendTo(const CtrlPacket& packet, const sockaddr_in& dest) {
    if (!g_socket_initialized) return;
    if (g_socket == INVALID_SOCKET) return;
    if (dest.sin_port == 0) return;

    CtrlPacket pkt = packet;
    pkt.header.magic     = CTRL_MAGIC;
    pkt.header.seq       = ++g_send_seq;
    pkt.header.ack       = g_recv_seq;
    pkt.header.player_id = g_local_player_id;

    sendto(g_socket, reinterpret_cast<const char*>(&pkt), sizeof(pkt), 0,
           reinterpret_cast<const sockaddr*>(&dest), sizeof(dest));
}

bool ControlChannel_IsConnected() {
    return g_connected;
}

uint32_t ControlChannel_GetLastRecvMs() {
    return GetTimeMs() - g_last_recv_time;
}

uint32_t ControlChannel_GetRttMs() {
    return g_rtt_ms;
}

void ControlChannel_SetCallback(ControlMsgCallback callback) {
    g_msg_callback = callback;
}

// =============================================================================
// CONTROL CHANNEL - CONVENIENCE FUNCTIONS
// =============================================================================

void ControlChannel_SendHello(uint8_t player_id, uint32_t game_hash) {
    CtrlPacket pkt = {};
    pkt.header.type = CtrlMsg::HELLO;
    pkt.data.hello.version = NETPLAY_PROTOCOL_VERSION;
    pkt.data.hello.player_id = player_id;
    pkt.data.hello.game_hash = game_hash;
    ControlChannel_Send(pkt);

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "ControlChannel: Sent HELLO (player=%d, hash=0x%08X)",
                player_id, game_hash);
}

void ControlChannel_SendHelloAck(uint8_t player_id) {
    CtrlPacket pkt = {};
    pkt.header.type = CtrlMsg::HELLO_ACK;
    pkt.data.hello.version = NETPLAY_PROTOCOL_VERSION;
    pkt.data.hello.player_id = player_id;
    ControlChannel_Send(pkt);

    g_connected = true;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "ControlChannel: Sent HELLO_ACK, connected!");
}

void ControlChannel_SendPing() {
    CtrlPacket pkt = {};
    pkt.header.type = CtrlMsg::PING;
    pkt.data.sync.frame = GetTimeMs();  // Include send time for RTT calculation
    ControlChannel_Send(pkt);
    g_ping_send_time = GetTimeMs();
}

void ControlChannel_SendCursor(uint8_t x, uint8_t y) {
    CtrlPacket pkt = {};
    pkt.header.type = CtrlMsg::CSS_CURSOR;
    pkt.data.cursor.x = x;
    pkt.data.cursor.y = y;
    ControlChannel_Send(pkt);
}

void ControlChannel_SendCharSelect(uint8_t slot, uint8_t color) {
    CtrlPacket pkt = {};
    pkt.header.type = CtrlMsg::CSS_CHAR_SELECT;
    pkt.data.character.slot = slot;
    pkt.data.character.color = color;
    ControlChannel_Send(pkt);
}

void ControlChannel_SendCharLock(uint8_t slot, uint8_t color) {
    CtrlPacket pkt = {};
    pkt.header.type = CtrlMsg::CSS_LOCK;
    pkt.data.character.slot = slot;
    pkt.data.character.color = color;
    ControlChannel_Send(pkt);

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "ControlChannel: Sent CSS_LOCK (slot=%d, color=%d)",
                slot, color);
}

void ControlChannel_SendCharUnlock() {
    CtrlPacket pkt = {};
    pkt.header.type = CtrlMsg::CSS_UNLOCK;
    ControlChannel_Send(pkt);

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "ControlChannel: Sent CSS_UNLOCK");
}

void ControlChannel_SendCSSStart() {
    CtrlPacket pkt = {};
    pkt.header.type = CtrlMsg::CSS_START;
    ControlChannel_Send(pkt);

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "ControlChannel: Sent CSS_START");
}

void ControlChannel_SendBattleReady() {
    CtrlPacket pkt = {};
    pkt.header.type = CtrlMsg::BATTLE_READY;
    ControlChannel_Send(pkt);

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "ControlChannel: Sent BATTLE_READY");
}

void ControlChannel_SendChat(const char* text) {
    if (!text) return;
    CtrlPacket pkt = {};
    pkt.header.type = CtrlMsg::CHAT;
    // Truncate to 23 chars + guaranteed NUL terminator.
    std::strncpy(pkt.data.chat.text, text, sizeof(pkt.data.chat.text) - 1);
    pkt.data.chat.text[sizeof(pkt.data.chat.text) - 1] = '\0';
    ControlChannel_Send(pkt);

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "ControlChannel: Sent CHAT \"%s\"", pkt.data.chat.text);
}

void ControlChannel_SendBattleAck() {
    CtrlPacket pkt = {};
    pkt.header.type = CtrlMsg::BATTLE_ACK;
    ControlChannel_Send(pkt);

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "ControlChannel: Sent BATTLE_ACK");
}

void ControlChannel_SendBattleEntering(uint32_t swap_frame) {
    CtrlPacket pkt = {};
    pkt.header.type = CtrlMsg::BATTLE_ENTERING;
    pkt.data.sync.frame = swap_frame;
    ControlChannel_Send(pkt);

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "ControlChannel: Sent BATTLE_ENTERING (swap_frame=%u)", swap_frame);
}

void ControlChannel_SendBattleStart(uint32_t start_frame) {
    CtrlPacket pkt = {};
    pkt.header.type = CtrlMsg::BATTLE_START;
    pkt.data.sync.frame = start_frame;
    ControlChannel_Send(pkt);

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "ControlChannel: Sent BATTLE_START (frame=%u)",
                start_frame);
}

void ControlChannel_SendBattleEnd(uint32_t swap_frame) {
    CtrlPacket pkt = {};
    pkt.header.type = CtrlMsg::BATTLE_END;
    pkt.data.sync.frame = swap_frame;
    ControlChannel_Send(pkt);

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "ControlChannel: Sent BATTLE_END (swap_frame=%u)", swap_frame);
}

void ControlChannel_SendDisconnect() {
    CtrlPacket pkt = {};
    pkt.header.type = CtrlMsg::DISCONNECT;
    ControlChannel_Send(pkt);

    g_connected = false;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "ControlChannel: Sent DISCONNECT");
}

// =============================================================================
// GEKKONET CUSTOM ADAPTER
// Shares socket with control channel, filters control packets
// =============================================================================

// Adapter send function - sends directly via our socket.
//
// Pre-spectator era this ignored `addr` and sent to the single peer via
// RawSend → g_remote_sockaddr. With spectator support GekkoNet now
// addresses multiple destinations (player + spectators), so we honor the
// supplied address. Format on the wire is "ip:port" (matches what
// MultiplexAdapter_Receive constructs from inbound sockaddr_in via
// inet_ntoa+ntohs). Falls back to RawSend(g_remote_sockaddr) when the
// addr string can't be parsed (defensive).
static void MultiplexAdapter_Send(GekkoNetAddress* addr, const char* data, int length) {
    if (g_socket == INVALID_SOCKET) return;
    if (!addr || !addr->data || addr->size <= 0) {
        RawSend(data, length);
        return;
    }

    // Parse "ip:port" — addr->data may not be null-terminated, copy out.
    char addr_buf[64] = {};
    int n = (addr->size < (int)sizeof(addr_buf) - 1) ? addr->size : (int)sizeof(addr_buf) - 1;
    std::memcpy(addr_buf, addr->data, n);
    char* colon = std::strrchr(addr_buf, ':');
    if (!colon) {
        RawSend(data, length);
        return;
    }
    *colon = '\0';
    const char* ip_str = addr_buf;
    const int   port   = std::atoi(colon + 1);
    if (port <= 0 || port > 65535) {
        RawSend(data, length);
        return;
    }

    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port   = htons(static_cast<uint16_t>(port));
    if (inet_pton(AF_INET, ip_str, &dst.sin_addr) != 1) {
        RawSend(data, length);
        return;
    }

    // In relay mode all gameplay traffic must still go through the relay
    // wrapper. RawSend handles that path internally; we keep the explicit
    // peer address only for direct (non-relay) sends. TODO: spectators
    // through a relay are a separate problem (the relay only knows about
    // one session_id pair); for now spectators go direct only.
    if (::fm2k::nat::IsRelayMode()) {
        RawSend(data, length);
        return;
    }

    int result = sendto(g_socket, data, length, 0,
                        reinterpret_cast<const sockaddr*>(&dst), sizeof(dst));
    if (result == SOCKET_ERROR) {
        int err = WSAGetLastError();
        if (err != WSAEWOULDBLOCK) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                "MultiplexAdapter: sendto(%s:%d) failed: %d", ip_str, port, err);
        }
    }
}

// Adapter receive function - returns queued GekkoNet packets
static GekkoNetResult** MultiplexAdapter_Receive(int* length) {
    // Clear previous pointer array (results were freed by GekkoNet)
    g_gekko_result_ptrs.clear();

    // Process any pending packets first (in case Poll wasn't called)
    RawReceive();

    // Debug logging removed - too verbose for production

    // Convert queued packets to GekkoNet format
    // IMPORTANT: Each result must be heap-allocated because GekkoNet will free them!
    for (auto& [pkt_data, from_addr] : g_gekko_packet_queue) {
        // Allocate the result struct on heap - GekkoNet will free it
        GekkoNetResult* result = (GekkoNetResult*)malloc(sizeof(GekkoNetResult));

        // Create address string
        char addr_str[64];
        snprintf(addr_str, sizeof(addr_str), "%s:%d",
                 inet_ntoa(from_addr.sin_addr), ntohs(from_addr.sin_port));

        result->addr.data = _strdup(addr_str);  // GekkoNet will free this
        result->addr.size = (int)strlen(addr_str);
        result->data = (char*)malloc(pkt_data.size());
        memcpy(result->data, pkt_data.data(), pkt_data.size());
        result->data_len = (int)pkt_data.size();

        g_gekko_result_ptrs.push_back(result);
    }

    // Clear the queue
    g_gekko_packet_queue.clear();

    *length = (int)g_gekko_result_ptrs.size();
    return g_gekko_result_ptrs.empty() ? nullptr : g_gekko_result_ptrs.data();
}

// Adapter free function
static void MultiplexAdapter_Free(void* data_ptr) {
    free(data_ptr);
}

// Static adapter instance
static GekkoNetAdapter g_multiplex_adapter = {
    .send_data = MultiplexAdapter_Send,
    .receive_data = MultiplexAdapter_Receive,
    .free_data = MultiplexAdapter_Free
};

GekkoNetAdapter* CreateMultiplexAdapter() {
    return &g_multiplex_adapter;
}

void DestroyMultiplexAdapter() {
    // Clear any remaining queued packets
    g_gekko_packet_queue.clear();
    g_gekko_result_ptrs.clear();  // Note: results themselves are freed by GekkoNet
}

// Handle incoming PONG to calculate RTT
void ControlChannel_HandlePong(uint32_t sent_time) {
    g_rtt_ms = GetTimeMs() - sent_time;
}

// Mark connection as established (called when HELLO_ACK received)
void ControlChannel_SetConnected(bool connected) {
    g_connected = connected;
    if (connected) {
        g_last_recv_time = GetTimeMs();
    }
}
