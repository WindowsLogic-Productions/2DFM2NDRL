// Control Channel -- RAW UDP send/receive (split from control_channel.cpp).
// RawSend wraps relay/direct egress; RawReceive demuxes control (0xCC) / NAT
// (0xCD) / spectator-UDP (0xCE) / GekkoNet packets. Shares state via
// control_channel_internal.h. CONTRACT: run under g_poll_mutex (RawReceive
// mutates g_gekko_packet_queue / g_recv_buffer). ENGINE-AGNOSTIC.
#include "control_channel.h"
#include "control_channel_internal.h"
#include "globals.h"            // g_player_index (peer-addr learning context)
#include "nat_traversal.h"      // fm2k::nat relay wrap/unwrap + 0xCD datagrams
#include <SDL3/SDL_log.h>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>
#include <ws2tcpip.h>
#include <winsock2.h>
#include <windows.h>

// =============================================================================
// RAW SEND/RECEIVE
// =============================================================================

void RawSend(const void* data, size_t len) {
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
// Race detector (FM2K_RACE_DETECT=1). RawReceive mutates the shared
// g_gekko_packet_queue / g_recv_buffer. It is meant to run only under
// g_poll_mutex; the MM-timer path holds it, but MultiplexAdapter_Receive
// (the bug) did not. This guard increments an atomic on entry and
// decrements on exit -- if two threads are ever inside concurrently the
// depth exceeds 1, which is the smoking-gun data race that corrupts
// GekkoNet's heap. Deterministic proof for the A/B: unfixed build trips
// this under clumsy within seconds; fixed build (lock added) never does.
struct RawReceiveRaceGuard {
    static std::atomic<int>      s_depth;
    static std::atomic<uint32_t> s_first_tid;
    bool armed;
    RawReceiveRaceGuard() {
        static int cached = -1;
        if (cached < 0) {
            const char* v = std::getenv("FM2K_RACE_DETECT");
            cached = (v && v[0] == '1') ? 1 : 0;
        }
        armed = (cached == 1);
        if (!armed) return;
        const uint32_t tid = (uint32_t)GetCurrentThreadId();
        const int d = s_depth.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (d == 1) {
            s_first_tid.store(tid, std::memory_order_relaxed);
        } else {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                "[RACE-DETECT] RawReceive concurrent entry: depth=%d "
                "this_tid=%u other_tid=%u -- g_gekko_packet_queue mutated "
                "from two threads WITHOUT g_poll_mutex (heap-corruption bug)",
                d, tid, s_first_tid.load(std::memory_order_relaxed));
        }
    }
    ~RawReceiveRaceGuard() {
        if (armed) s_depth.fetch_sub(1, std::memory_order_acq_rel);
    }
};
std::atomic<int>      RawReceiveRaceGuard::s_depth{0};
std::atomic<uint32_t> RawReceiveRaceGuard::s_first_tid{0};

void RawReceive() {
    if (g_socket == INVALID_SOCKET) return;
    RawReceiveRaceGuard _race_guard;

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
                } else if (packet->header.type == CtrlMsg::DELAY_PROPOSAL) {
                    // Stash the peer's delay candidate (#24). Battle
                    // start adopts max(local, this) so both peers run
                    // identical input delay.
                    const int d = packet->data.delay_proposal.delay;
                    if (d >= 0 && d <= 16) {
                        g_remote_delay_candidate.store(
                            d, std::memory_order_relaxed);
                    }
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
        } else if (eff_len >= 1 && first == 0xCE) {
            // Spectator UDP input accelerator (Phase F). Narrow handler:
            // accepts ONLY UDP_INPUT_BATCH from the current upstream;
            // everything else is dropped there. Keeps the datagram out of
            // GekkoNet's queue (old builds without this branch fed 0xCE
            // to gekko, which discards on bad magic -- harmless but noisy).
            extern void SpectatorNode_HandleUdpInputDatagram(
                const uint8_t* buf, size_t len, const sockaddr_in& from);
            SpectatorNode_HandleUdpInputDatagram(
                reinterpret_cast<const uint8_t*>(eff_data), eff_len, from_addr);
        } else {
            // GekkoNet packet - queue for adapter
            std::vector<char> pkt_data(
                reinterpret_cast<const char*>(eff_data),
                reinterpret_cast<const char*>(eff_data) + eff_len);
            // Source-address stamping. GekkoNet drops any inbound packet
            // whose source string doesn't byte-match the address handed to
            // gekko_add_actor (netplay.cpp:1928 / 2520). For DIRECT packets
            // that's from_addr (the genuine peer source) -- unchanged, the
            // path stays bit-identical to before (preserves D9). For RELAY
            // packets from_addr is the relay (e.g. 127.0.0.1:7712), NOT the
            // peer, so stamping it would make every relayed gekko input get
            // dropped and stall CSS lockstep forever. Stamp g_remote_sockaddr
            // instead: it is the configured remote (peer-addr learning is
            // skipped for relayed packets at ~507, so it stays exactly as
            // FM2K_REMOTE_ADDR was parsed). The actor string CANNOT diverge
            // from this -- both the actor string (NetSocket_GetRemoteAddr ->
            // inet_ntop) and the receive string (MultiplexAdapter_Receive ->
            // inet_ntoa) are derived from this same g_remote_sockaddr, and
            // it was set via inet_pton (dotted-decimal only, no hostnames),
            // so the two formattings produce identical "ip:port" text.
            const sockaddr_in& stamp = from_relay ? g_remote_sockaddr : from_addr;
            g_gekko_packet_queue.push_back({std::move(pkt_data), stamp});
        }
    }
}
