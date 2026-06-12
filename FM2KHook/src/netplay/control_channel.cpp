// Control Channel - Multiplexed UDP Socket Implementation
#include "control_channel.h"
#include "delay_math.h"
#include "globals.h"
#include "gekkonet.h"
#include "spectator_node.h"
#include "spectator_tcp.h"
#include "nat_traversal.h"
#include "../ui/shared_mem.h"  // SharedMem_PublishMatchOutcome on ControlChannel timeout
#include <SDL3/SDL_log.h>
#include <vector>
#include <cstring>
#include <cstdlib>
#include <atomic>
#include <algorithm>
#include <chrono>
#include <mutex>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")

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
// When did g_connected go true? Used by ControlChannel_Poll's timeout
// check to apply a startup grace window — the first ~15s after
// connect we use a much wider timeout so cnc-ddraw / other slow boot
// paths don't false-DC on the second peer's late init. Reset to 0
// when g_connected drops back to false.
static uint32_t g_connected_at_ms = 0;

// Keepalive heartbeat (#fixes-modal-drag-disconnect 2026-05-05).
// Multimedia timer that fires on a worker thread, independent of the
// main loop's Windows message pump. When the user drags the title bar
// or opens the System menu, DefWindowProc enters a modal loop that
// blocks DispatchMessage; without this thread the main loop stops
// pumping pings and the peer's ControlChannel_Poll declares us
// disconnected after PING_TIMEOUT_MS. The MM timer keeps sending
// pings during modal mode so peers stay connected through window
// drags / brief stalls. Receive + state mutation stays on the main
// thread; only the outbound ping is fired from here.
static UINT g_keepalive_timer_id = 0;

// Serialises ControlChannel_Poll between the main thread and the
// MM-timer worker. The MM-timer drives Poll itself when the main
// thread is blocked (modal window drag / Alt-Tab inactivity / boot
// stalls), so the handshake + receive path keeps making progress.
// The lock is fine-grained — RawReceive + dispatch is non-blocking
// and finishes in microseconds — so contention between threads is a
// non-issue in practice.
static std::mutex g_poll_mutex;
// Tracks the last time the MAIN thread (not the MM timer) ran
// ControlChannel_Poll. Updated at the top of ControlChannel_Poll. The
// timeout-detection logic checks this value AGAINST g_last_recv_time
// to avoid declaring DC after the main thread itself has been frozen
// (modal drag pause); we extend the recv deadline by however long the
// main pump was asleep.
static uint32_t g_last_main_pump_ms = 0;
static uint32_t g_last_ping_time = 0;
static uint32_t g_rtt_ms = 0;
static uint32_t g_ping_send_time = 0;

// Worst-RTT accumulator (CCCaster-style). Tracks the maximum RTT observed
// over a rolling window so Netplay_StartBattleSession can pick a delay
// that covers the spike, not just the mean. Reset by ResetWorstRttMs()
// at the start of each session-config measurement window.
static uint32_t g_rtt_worst_ms = 0;
static uint32_t g_rtt_sample_count = 0;

// Input-delay negotiation (#24). g_delay_mode picks the formula (avg vs
// peak RTT); g_remote_delay_candidate holds the last value the peer
// advertised via DELAY_PROPOSAL (-1 = none received). Both are touched
// from the MM-timer thread (send) and the main thread (receive / battle
// start), so they're atomic.
static std::atomic<int> g_delay_mode{0};               // 0=avg, 1=peak
static std::atomic<int> g_remote_delay_candidate{-1};

// Recent RTT samples for delay sizing (#24). A single cold-start ping
// (peer still injecting / loading) or a scheduler hiccup easily round-
// trips in 200-300 ms even on a 10 ms link -- and an all-time-max
// worst-RTT latches that forever, which is exactly why worst-based
// delay was a bad idea. avg mode takes the mean of this window; peak
// mode takes the p90, which sizes above the median but discards the
// worst ~10% of outliers. Lock-free: written on the receive path, read
// from the timer thread / battle start -- a torn read just nudges one
// delay estimate by a sample, self-corrects next tick.
static constexpr int kRttRingCap = 64;
static uint32_t g_rtt_ring[kRttRingCap] = {};
static int      g_rtt_ring_count = 0;   // valid entries (caps at cap)
static int      g_rtt_ring_head  = 0;   // next write index

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

// MM-timer callback: fires on a worker thread every PING_INTERVAL_MS
// regardless of the message pump state. Two responsibilities:
//   1. Send a ping if we're connected and the main thread hasn't
//      pinged recently — keeps us alive across modal drags.
//   2. Drive ControlChannel_Poll itself when the main thread is
//      stuck (modal drag during HANDSHAKE, alt-tab during boot, slow
//      ddraw init). Without this, a window drag during the
//      "connecting…" phase leaves the handshake permanently stalled
//      because the main thread is blocked inside DefWindowProc and
//      Poll never runs. try_lock so the timer thread never blocks if
//      the main thread is mid-Poll already; concurrency is serialised
//      by g_poll_mutex.
//
// Forward decl — body is lower in this TU.
static void PollImplLocked();
static void CALLBACK KeepaliveTimerProc(UINT, UINT, DWORD_PTR, DWORD_PTR, DWORD_PTR) {
    if (!g_socket_initialized) return;
    // Outbound ping (post-handshake only).
    if (g_connected) {
        const uint32_t now_send = (uint32_t)timeGetTime();
        if (now_send - g_last_ping_time >= PING_INTERVAL_MS / 2) {
            ControlChannel_SendPing();
            // Piggyback the delay-candidate broadcast on the ping
            // cadence so both peers have each other's value well
            // before the CSS->battle transition (#24).
            ControlChannel_SendDelayProposal();
        }
    }
    // Inbound poll — runs whether connected or not so the handshake
    // makes forward progress even when the main thread is parked.
    std::unique_lock<std::mutex> lock(g_poll_mutex, std::try_to_lock);
    if (lock.owns_lock()) {
        PollImplLocked();
    }
}

static void KeepaliveTimer_Start() {
    if (g_keepalive_timer_id != 0) return;
    // TIME_PERIODIC + TIME_KILL_SYNCHRONOUS: callbacks fire every
    // PING_INTERVAL_MS, and timeKillEvent() blocks until any in-flight
    // callback returns (so shutdown doesn't race with sendto).
    g_keepalive_timer_id = timeSetEvent(
        PING_INTERVAL_MS, PING_INTERVAL_MS / 2,
        KeepaliveTimerProc, 0,
        TIME_PERIODIC | TIME_KILL_SYNCHRONOUS);
    if (g_keepalive_timer_id == 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "ControlChannel: timeSetEvent failed; modal-drag will still DC");
    }
}

static void KeepaliveTimer_Stop() {
    if (g_keepalive_timer_id != 0) {
        timeKillEvent(g_keepalive_timer_id);
        g_keepalive_timer_id = 0;
    }
}

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

    // Bind with retry. The port can be TRANSIENTLY held at game boot:
    // the launcher's pre-match STUN probe (LauncherStunProbe) binds the
    // same local port and releases it "shortly before" we bind -- a
    // designed-in handoff that a one-shot bind turns into a coin flip
    // (observed as intermittent err=10013 "stuck at connecting", both in
    // hub matches and the loopback harnesses). Retry for up to 10s
    // before declaring the port genuinely taken.
    {
        int bind_attempts = 0;
        for (;;) {
            if (bind(g_socket, (sockaddr*)&g_local_sockaddr,
                     sizeof(g_local_sockaddr)) != SOCKET_ERROR) {
                if (bind_attempts > 0) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "NetSocket: bound port %d after %d retries (%.1fs)",
                        (int)local_port, bind_attempts, bind_attempts * 0.25);
                }
                break;
            }
            const int err = WSAGetLastError();
            ++bind_attempts;
            if ((err == WSAEACCES || err == WSAEADDRINUSE) && bind_attempts < 40) {
                if (bind_attempts == 1) {
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "NetSocket: port %d busy (err=%d) -- retrying for up "
                        "to 10s (launcher STUN handoff / stale socket)",
                        (int)local_port, err);
                }
                Sleep(250);
                continue;
            }
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                "NetSocket: bind() failed on port %d (err=%d, %d attempts). %s",
                (int)local_port, err, bind_attempts,
                err == WSAEADDRINUSE || err == WSAEACCES
                    ? "Port already in use — another launcher instance is on this port. "
                      "If running two launchers on the same machine, configure each "
                      "with a different FM2K_LOCAL_PORT (e.g. 7000 and 7001)."
                    : "");
            closesocket(g_socket);
            g_socket = INVALID_SOCKET;
            WSACleanup();
            return false;
        }
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
    // 0 sentinel — the first ControlChannel_Poll call sees this and
    // skips its pump-stall ride-through. Otherwise the natural gap
    // between NetSocket_Init and the first main-loop tick (sometimes
    // hundreds of ms) gets mis-classified as a stall and resets the
    // recv deadline before HELLO/HELLO_ACK has even completed.
    g_last_main_pump_ms = 0;
    KeepaliveTimer_Start();

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

    KeepaliveTimer_Stop();

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

uint16_t NetSocket_GetLocalPort() {
    if (!g_socket_initialized) return 0;
    return ntohs(g_local_sockaddr.sin_port);
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

// Body of ControlChannel_Poll. Called either by the main thread (via
// ControlChannel_Poll, which acquires the mutex) or by the MM-timer
// worker (via KeepaliveTimerProc, also under the mutex). Touching this
// directly without holding g_poll_mutex is a bug.
static void PollImplLocked() {
    if (!g_socket_initialized) return;

    // Receive all pending packets
    RawReceive();

    // TCP path for the spectator INPUT_BATCH stream. Host-side: drain
    // the listener accept queue + read-discard from connected subs.
    // Spectator-side: drive the upstream connect to completion + drain
    // inbound bytes through the SpecDataHeader parser. Each call is
    // non-blocking and cheap when nothing is pending; safe to run on
    // both host and spectator processes (no-op when not applicable).
    SpectatorTCP::PollAccepts();
    SpectatorTCP::PollIncoming();
    SpectatorTCP::PollUpstream();

    // Pair freshly-accepted TCP clients to known subscriber slots and ship
    // deferred INITIAL_MATCH/backfill. Runs on host (which doesn't go
    // through SpectatorNode_TickHealth's spectator path); idempotent and
    // cheap when there are no subscribers.
    SpectatorNode_TickHostMaintenance();

    // Check for timeout. Pump-stall ride-through: if the main thread
    // paused longer than half the ping timeout (e.g. Win32 modal
    // title-drag, scheduler hiccup, init->first-frame gap), the
    // peer's pings probably arrived but went unread. Reset
    // g_last_recv_time to `now` so the timeout countdown effectively
    // restarts — the MM-timer kept OUR pings flowing, so this is
    // symmetric: neither side false-DCs the other across a stall.
    //
    // The first-call sentinel (g_last_main_pump_ms == 0) prevents a
    // false-stall on the very first Poll after init; otherwise the
    // gap from NetSocket_Init to the first main-loop iteration would
    // look like a 300-1000ms "stall" and reset the deadline before
    // the real timeout window has even started counting.
    uint32_t now = GetTimeMs();
    if (g_last_main_pump_ms != 0) {
        const uint32_t main_pause = now - g_last_main_pump_ms;
        if (main_pause > PING_TIMEOUT_MS / 2) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "ControlChannel: main pump stalled %ums — resetting recv deadline",
                (unsigned)main_pause);
            g_last_recv_time = now;
        }
    }
    g_last_main_pump_ms = now;
    // Startup grace window: the first ~15s after handshake we use a 10s
    // timeout instead of 1.5s. cnc-ddraw / other proxy DLLs add measurable
    // boot latency to the OTHER peer; if their hook hasn't started its
    // ping cycle by the time our 1.5s window expires we'd false-DC them.
    // After 15s both peers are in steady state and the tight timeout
    // catches real disconnects fast.
    constexpr uint32_t kStartupGraceWindowMs = 15000;
    constexpr uint32_t kStartupTimeoutMs     = 10000;
    const uint32_t timeout_ms =
        (g_connected_at_ms != 0 &&
         (now - g_connected_at_ms) < kStartupGraceWindowMs)
            ? kStartupTimeoutMs
            : (uint32_t)PING_TIMEOUT_MS;
    if (g_connected && (now - g_last_recv_time) > timeout_ms) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "ControlChannel: Connection timed out");
        g_connected = false;
        g_connected_at_ms = 0;
        // Surface the disconnect to the launcher even when the failure
        // happens BEFORE we reach the BATTLE GekkoNet session — the user
        // bug report ("closed client during CSS, host stayed frozen with
        // music playing") was the CSS phase + GekkoPlayerDisconnected
        // never firing because we hadn't created the battle session yet.
        // PublishMatchOutcome with DISCONNECT bumps match_outcome_seq;
        // the launcher's PollMatchOutcome reads it on the next frame and
        // calls on_session_stop to terminate the surviving game process.
        // No-op when shared mem isn't initialised.
        SharedMem_PublishMatchOutcome(FM2K_MATCH_OUTCOME_DISCONNECT);
    }

    // Send periodic ping
    if (g_connected && (now - g_last_ping_time) > PING_INTERVAL_MS) {
        ControlChannel_SendPing();
        g_last_ping_time = now;
    }
}

void ControlChannel_Poll() {
    // Public entry. Lock-then-delegate so the MM-timer's parallel poll
    // path (which uses try_lock to avoid blocking the timer worker)
    // can't race with us on RawReceive's dispatch into Netplay_*
    // handlers.
    std::lock_guard<std::mutex> lock(g_poll_mutex);
    PollImplLocked();
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

void ControlChannel_SendRawTo(const void* buf, size_t len, const sockaddr_in& dest) {
    if (!g_socket_initialized) return;
    if (g_socket == INVALID_SOCKET) return;
    if (dest.sin_port == 0) return;
    sendto(g_socket, reinterpret_cast<const char*>(buf), (int)len, 0,
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

uint32_t ControlChannel_GetWorstRttMs() {
    return g_rtt_worst_ms;
}

uint32_t ControlChannel_GetRttSampleCount() {
    return g_rtt_sample_count;
}

void ControlChannel_ResetWorstRttMs() {
    g_rtt_worst_ms     = g_rtt_ms;  // seed with the most recent sample so
                                    // we never report 0 if no PONGs land
                                    // in the next window
    g_rtt_sample_count = (g_rtt_ms > 0) ? 1 : 0;
}

void ControlChannel_SetCallback(ControlMsgCallback callback) {
    g_msg_callback = callback;
}

// --- Input-delay negotiation (#24) -------------------------------------

void ControlChannel_SetDelayMode(int mode) {
    g_delay_mode.store(mode == 1 ? 1 : 0, std::memory_order_relaxed);
}

int ControlChannel_GetDelayMode() {
    return g_delay_mode.load(std::memory_order_relaxed);
}

int ControlChannel_GetLocalDelayCandidate() {
    // A manual FM2K_LOCAL_DELAY override is the user's explicit choice
    // and is exchanged like any other candidate, so a peer who pins
    // delay 14 pulls the other peer up to 14 via the max() at battle
    // start instead of leaving them on a desynced computed value.
    if (const char* env = std::getenv("FM2K_LOCAL_DELAY"); env && env[0]) {
        int v = std::atoi(env);
        if (v >= 0 && v <= 16) return v;
    }
    // No override: size from the measured RTT window. avg mode uses the
    // mean (lower delay, spikes can rollback); peak mode uses the p90
    // (higher delay, rides out jitter). The window math is pure and
    // lives in delay_math.h -- see tests/delay_math_test.cpp.
    const uint32_t rtt = fm2k::delay::RttFromWindow(
        g_rtt_ring, g_rtt_ring_count,
        g_delay_mode.load(std::memory_order_relaxed));
    return fm2k::delay::DelayCandidateFromRtt(rtt);
}

int ControlChannel_GetRemoteDelayCandidate() {
    return g_remote_delay_candidate.load(std::memory_order_relaxed);
}

void ControlChannel_SendDelayProposal() {
    if (!g_connected) return;
    const int cand = ControlChannel_GetLocalDelayCandidate();
    if (cand < 0) return;  // nothing measured to propose yet
    CtrlPacket pkt = {};
    pkt.header.type = CtrlMsg::DELAY_PROPOSAL;
    pkt.data.delay_proposal.delay = (uint8_t)cand;
    pkt.data.delay_proposal.mode  =
        (uint8_t)g_delay_mode.load(std::memory_order_relaxed);
    ControlChannel_Send(pkt);
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
    g_connected_at_ms = GetTimeMs();
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

void ControlChannel_SendBattleEntering(uint32_t swap_frame, uint8_t epoch,
                                       uint8_t flags) {
    CtrlPacket pkt = {};
    pkt.header.type = CtrlMsg::BATTLE_ENTERING;
    pkt.data.sync.frame = swap_frame;
    pkt.data.sync.epoch = epoch;
    pkt.data.sync.flags = flags;
    ControlChannel_Send(pkt);

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "ControlChannel: Sent BATTLE_ENTERING (swap_frame=%u epoch=%u flags=0x%02X)",
                swap_frame, epoch, flags);
}

void ControlChannel_SendBattleStart(uint32_t start_frame) {
    CtrlPacket pkt = {};
    pkt.header.type = CtrlMsg::BATTLE_START;
    pkt.data.sync.frame = start_frame;
    ControlChannel_Send(pkt);

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "ControlChannel: Sent BATTLE_START (frame=%u)",
                start_frame);
}

void ControlChannel_SendBattleEnd(uint32_t swap_frame, uint8_t epoch,
                                  uint8_t flags) {
    CtrlPacket pkt = {};
    pkt.header.type = CtrlMsg::BATTLE_END;
    pkt.data.sync.frame = swap_frame;
    pkt.data.sync.epoch = epoch;
    pkt.data.sync.flags = flags;
    ControlChannel_Send(pkt);

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "ControlChannel: Sent BATTLE_END (swap_frame=%u epoch=%u flags=0x%02X)",
                swap_frame, epoch, flags);
}

void ControlChannel_SendHostConfig(uint32_t selected_stage,
                                   uint32_t round_count,
                                   uint32_t round_time_sec,
                                   uint32_t game_speed_pct,
                                   uint8_t  socd_mode)
{
    CtrlPacket pkt = {};
    pkt.header.type = CtrlMsg::HOST_CONFIG;
    pkt.data.host_config.selected_stage  = selected_stage;
    pkt.data.host_config.round_count     = round_count;
    pkt.data.host_config.round_time_sec  = round_time_sec;
    pkt.data.host_config.game_speed_pct  = game_speed_pct;
    pkt.data.host_config.socd_mode       = socd_mode;
    ControlChannel_Send(pkt);

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "ControlChannel: Sent HOST_CONFIG (stage=%u rounds=%u time=%u speed=%u socd=%u)",
                selected_stage, round_count, round_time_sec, game_speed_pct,
                (unsigned)socd_mode);
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
    // CRITICAL: hold g_poll_mutex for the whole body. This runs on the
    // MAIN thread (gekko's receive_data callback, fired from
    // gekko_network_poll / gekko_update_session). RawReceive() push_backs
    // onto g_gekko_packet_queue, and the MM-timer worker thread
    // (KeepaliveTimerProc -> PollImplLocked -> RawReceive) push_backs onto
    // the SAME vector under this mutex. Without taking it here, the two
    // threads mutate g_gekko_packet_queue / g_recv_buffer / g_gekko_result_ptrs
    // concurrently: a push_back reallocation on the timer thread frees the
    // buffer this thread is iterating, the freed block is recycled into
    // GekkoNet's own heap (the session_health std::map node pool), and the
    // received InputMsg bytes land inside a tree node -> AV the next time
    // SendSessionHealthCheck walks the map (registers full of the
    // "Gekko::InputMsg" serializer tag). This was the intermittent
    // netplay-only counterhit crash (babel, pkmncc, 0.2.71): counterhits
    // spawn extra effects -> heavier frames -> gekko polls harder -> the
    // race window widens. RawReceive's own contract ("touching this
    // without holding g_poll_mutex is a bug") was being violated here.
    // The timer thread uses try_lock, so it simply skips its poll on the
    // ticks we hold the lock -- no deadlock, no recursion (the main-thread
    // gekko_* callers do NOT hold g_poll_mutex).
    std::lock_guard<std::mutex> lock(g_poll_mutex);

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
    if (g_rtt_ms > g_rtt_worst_ms) g_rtt_worst_ms = g_rtt_ms;
    ++g_rtt_sample_count;
    // Feed the delay-sizing window (#24).
    g_rtt_ring[g_rtt_ring_head] = g_rtt_ms;
    g_rtt_ring_head = (g_rtt_ring_head + 1) % kRttRingCap;
    if (g_rtt_ring_count < kRttRingCap) ++g_rtt_ring_count;
}

// Mark connection as established (called when HELLO_ACK received)
void ControlChannel_SetConnected(bool connected) {
    g_connected = connected;
    if (connected) {
        g_last_recv_time = GetTimeMs();
    } else {
        // Drop the peer's delay candidate + our RTT window so a
        // reconnect renegotiates from scratch instead of reusing a
        // stale value or another peer's latency samples (#24).
        g_remote_delay_candidate.store(-1, std::memory_order_relaxed);
        g_rtt_ring_count = 0;
        g_rtt_ring_head  = 0;
    }
}
