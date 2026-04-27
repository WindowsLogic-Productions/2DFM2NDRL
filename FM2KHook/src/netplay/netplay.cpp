// CCCaster-Style Netplay Implementation
// - Control channel for CSS input sync using INPUT DELAY (not lockstep)
// - GekkoNet for battle mode rollback
// - Uses game's internal timer for frame counting
#include "netplay.h"
#include "control_channel.h"
#include "input.h"
#include "savestate.h"
#include "replay.h"
#include "spectator_node.h"
#include "nat_traversal.h"
#include "globals.h"
#include "gekkonet.h"
#include "../audio/sound_rollback.h"
#include <SDL3/SDL_log.h>
#include <ws2tcpip.h>
#include <cstdlib>
#include <string>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <cstdio>
#include <cstring>

// =============================================================================
// SIMPLIFIED STATE
// =============================================================================

enum class SimpleState : uint8_t {
    DISCONNECTED,   // No connection
    CONNECTED,      // Control channel connected
    BATTLE          // GekkoNet active for battle
};

static SimpleState g_simple_state = SimpleState::DISCONNECTED;

static NetplayState MapToLegacyState(SimpleState s) {
    switch (s) {
        case SimpleState::DISCONNECTED: return NetplayState::DISCONNECTED;
        case SimpleState::CONNECTED: return NetplayState::CSS_LOBBY;
        case SimpleState::BATTLE: return NetplayState::BATTLE_RUNNING;
        default: return NetplayState::DISCONNECTED;
    }
}

// =============================================================================
// CCCASTER-STYLE INPUT DELAY SYSTEM
// Inputs stored at (frame + delay), read at (frame)
// Both clients always advance - delay ensures inputs arrive in time
// =============================================================================

static constexpr int CSS_INPUT_DELAY = 6;           // 6 frames delay like CCCaster - never stall
static constexpr int CSS_INPUT_BUFFER_SIZE = 256;   // Ring buffer size
static constexpr int CSS_CONFIRM_LOCKOUT = 150;     // Block confirm for first N frames (moon selector workaround)

// Ring buffer for inputs indexed by frame
struct InputRingBuffer {
    uint16_t inputs[CSS_INPUT_BUFFER_SIZE];
    uint32_t end_frame;  // Highest frame we have input for + 1

    void Clear() {
        memset(inputs, 0, sizeof(inputs));
        end_frame = 0;
    }

    void Set(uint32_t frame, uint16_t input) {
        int slot = frame % CSS_INPUT_BUFFER_SIZE;
        inputs[slot] = input;
        if (frame >= end_frame) {
            end_frame = frame + 1;
        }
    }

    uint16_t Get(uint32_t frame) const {
        // Return 0 (neutral) if we don't have this frame yet
        if (frame >= end_frame) {
            return 0;
        }
        int slot = frame % CSS_INPUT_BUFFER_SIZE;
        return inputs[slot];
    }

    bool HasFrame(uint32_t frame) const {
        return frame < end_frame;
    }

    uint32_t GetEndFrame() const {
        return end_frame;
    }
};

// CSS state
static InputRingBuffer g_local_inputs;
static InputRingBuffer g_remote_inputs;
static uint32_t g_css_start_timer = 0;  // Value of game timer when BOTH clients ready
static uint32_t g_css_frame = 0;        // Current CSS frame (relative to start)
static bool g_css_active = false;       // Currently in CSS mode
static bool g_css_synced = false;       // BOTH clients ready, timer started
static bool g_remote_css_ready = false; // Remote has entered CSS
static bool g_local_css_ready = false;  // We've entered CSS

// Explicit read frame - the frame the game is currently processing
static uint32_t g_css_current_read_frame = 0;

// GekkoNet session
static GekkoSession* g_session = nullptr;
static bool g_session_ready = false;
static uint16_t g_p1_input = 0;
static uint16_t g_p2_input = 0;
static uint32_t g_netplay_frame = 0;

// Rollback tracking
static uint32_t g_rollback_count = 0;
static uint32_t g_last_rollback_frame = 0;
static uint32_t g_desync_count = 0;
static uint32_t g_last_desync_log_tick = 0;
static int g_local_delay = 1;  // Computed from RTT at battle start

// Highest frame number we've ever recorded into the replay/spectator stream.
// Reset on each Netplay_StartBattle (g_netplay_frame also resets to 0).
// Gates the GekkoAdvance recording path against runahead duplicates — each
// frame is advanced multiple times under runahead, but only the first
// monotonic forward crossing is "the" confirmed advance.
static uint32_t g_highest_recorded_frame = 0;

// Battle entry sync barrier - ensures both clients enter battle at same time
static bool g_local_battle_entered = false;   // We've detected battle mode
static bool g_remote_battle_entered = false;  // Remote has detected battle mode
static bool g_battle_synced = false;          // Both have entered, GekkoNet can start

// Handshake state
static bool g_received_hello = false;
static bool g_received_hello_ack = false;

// =============================================================================
// Note: We use our own frame counter instead of game timer
// The game timer at 0x470044 appears to be 0 during CSS
// =============================================================================

// =============================================================================
// CONTROL MESSAGE HANDLER
// =============================================================================

static void CheckFullyConnected() {
    if (g_received_hello && g_received_hello_ack) {
        if (g_simple_state != SimpleState::CONNECTED) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Netplay: Full handshake complete - CONNECTED!");
            g_simple_state = SimpleState::CONNECTED;
            ControlChannel_SetConnected(true);

            // Sync RNG immediately
            *(uint32_t*)FM2K::ADDR_RANDOM_SEED = 0x12345678;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Netplay: Synced RNG=0x12345678");
        }
    }
}

static void OnControlMessage(const CtrlPacket* packet, const sockaddr_in& from) {
    switch (packet->header.type) {
        case CtrlMsg::HELLO:
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Netplay: Received HELLO from player %d",
                packet->data.hello.player_id);
            ControlChannel_SendHelloAck(static_cast<uint8_t>(g_player_index));
            g_received_hello = true;
            CheckFullyConnected();
            break;

        case CtrlMsg::HELLO_ACK:
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Netplay: Received HELLO_ACK");
            g_received_hello_ack = true;
            CheckFullyConnected();
            break;

        case CtrlMsg::CSS_INPUT: {
            // Store remote input at the specified frame
            uint32_t frame = packet->data.css_input.frame;
            uint16_t input = packet->data.css_input.input;
            g_remote_inputs.Set(frame, input);
            // No logging - too spammy
            break;
        }

        case CtrlMsg::BATTLE_READY: {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Netplay: Received BATTLE_READY from remote");
            g_remote_css_ready = true;
            break;
        }

        case CtrlMsg::BATTLE_ENTERING:
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Netplay: Received BATTLE_ENTERING from remote - they're in battle mode");
            g_remote_battle_entered = true;
            break;

        case CtrlMsg::DISCONNECT:
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Netplay: Remote disconnected");
            ControlChannel_SetConnected(false);
            g_simple_state = SimpleState::DISCONNECTED;
            break;

        case CtrlMsg::CHAT: {
            // Inbound peer chat. Append to the chat log ring; launcher UI
            // reads via Netplay_PopChatMessage on its own cadence.
            const char* text = packet->data.chat.text;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Netplay: CHAT from remote: \"%s\"", text);
            Netplay_PushChatMessage(/*from_remote*/ true, text);
            break;
        }

        case CtrlMsg::SPEC_JOIN_REQ:
            SpectatorNode_HandleJoinReq(from);
            break;

        case CtrlMsg::SPEC_JOIN_ACK:
            SpectatorNode_HandleJoinAck(from);
            break;

        case CtrlMsg::SPEC_JOIN_REDIRECT:
            SpectatorNode_HandleJoinRedirect(
                from,
                packet->data.spec_redirect.redirect_ip,
                packet->data.spec_redirect.redirect_port);
            break;

        case CtrlMsg::SPEC_HEARTBEAT:
            SpectatorNode_HandleHeartbeat(from);
            break;

        case CtrlMsg::SPEC_LEAVE:
            SpectatorNode_HandleLeave(from);
            break;

        default:
            break;
    }
}

// =============================================================================
// LIFECYCLE
// =============================================================================

bool Netplay_Init(int player_index, uint16_t local_port, const char* remote_addr) {
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "Netplay: Init player=%d port=%d remote=%s",
        player_index, local_port, remote_addr);

    g_player_index = player_index;
    g_simple_state = SimpleState::DISCONNECTED;
    g_session = nullptr;
    g_session_ready = false;
    g_netplay_frame = 0;
    g_p1_input = 0;
    g_p2_input = 0;

    // Reset CSS state
    g_local_inputs.Clear();
    g_remote_inputs.Clear();
    g_css_start_timer = 0;
    g_css_frame = 0;
    g_css_current_read_frame = 0;
    g_css_active = false;
    g_css_synced = false;
    g_remote_css_ready = false;
    g_local_css_ready = false;

    // Reset battle sync state
    g_local_battle_entered = false;
    g_remote_battle_entered = false;
    g_battle_synced = false;

    // Reset handshake
    g_received_hello = false;
    g_received_hello_ack = false;

    // Initialize spectator tree node — accepts subscribers on the same
    // multiplexed UDP socket. Does not open a separate listen socket.
    SpectatorNode_Init();

    // Store network config
    g_local_port = local_port;
    strncpy(g_remote_addr, remote_addr, sizeof(g_remote_addr) - 1);

    // Initialize control channel
    ControlChannel_SetCallback(OnControlMessage);

    if (!NetSocket_IsInitialized()) {
        if (!NetSocket_Init(local_port, remote_addr)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Netplay: Failed to init socket");
            return false;
        }
    }

    // Hub-driven NAT traversal. If FM2K_HUB_* env vars are present,
    // the launcher started this match via the hub — fire a STUN probe
    // (so the hub can reflect our public mapping) and start the
    // peer-to-peer burst-punch using the supplied match_token. If the
    // env vars aren't set, this match was a legacy direct-connect:
    // we skip and rely on the existing 0xCC HELLO loop alone.
    {
        const char* hub_udp     = std::getenv("FM2K_HUB_UDP_ADDR");
        const char* hub_user_id = std::getenv("FM2K_HUB_USER_ID");
        const char* match_tok   = std::getenv("FM2K_HUB_MATCH_TOKEN");

        if (hub_udp && hub_user_id) {
            ::fm2k::nat::SendStunProbe();
        }

        // Read relay env vars early so the post-burst fallback in
        // StartPunch's worker thread can engage relay mode the
        // moment direct punch fails.
        ::fm2k::nat::ConfigureRelay();

        if (match_tok && remote_addr && *remote_addr) {
            // Decode the 32-hex-char match token into 16 binary bytes.
            uint8_t token_bytes[16] = {};
            size_t hex_len = std::strlen(match_tok);
            if (hex_len > 32) hex_len = 32;
            auto nibble = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return 10 + c - 'a';
                if (c >= 'A' && c <= 'F') return 10 + c - 'A';
                return -1;
            };
            for (size_t i = 0; i + 1 < hex_len; i += 2) {
                int hi = nibble(match_tok[i]);
                int lo = nibble(match_tok[i + 1]);
                if (hi < 0 || lo < 0) break;
                token_bytes[i / 2] = static_cast<uint8_t>((hi << 4) | lo);
            }

            // Parse "ip:port" from FM2K_REMOTE_ADDR (= remote_addr param).
            std::string addr(remote_addr);
            auto colon = addr.rfind(':');
            if (colon != std::string::npos) {
                std::string ip_s = addr.substr(0, colon);
                int port_i = std::atoi(addr.c_str() + colon + 1);
                in_addr peer_ia{};
                if (inet_pton(AF_INET, ip_s.c_str(), &peer_ia) == 1 &&
                    port_i > 0 && port_i <= 65535) {
                    ::fm2k::nat::StartPunch(peer_ia.s_addr,
                                             static_cast<uint16_t>(port_i),
                                             token_bytes);
                }
            }
        }
    }

    // Send HELLO
    ControlChannel_SendHello(static_cast<uint8_t>(player_index), 0);

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Netplay: Initialized, connecting...");
    return true;
}

bool Netplay_InitAsSpectator(uint16_t local_port, const char* host_addr) {
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Netplay (spectator): Init port=%u host=%s", local_port, host_addr ? host_addr : "(null)");

    g_player_index = 2;  // sentinel — not a player slot
    g_simple_state = SimpleState::CONNECTED;  // skip handshake; spectators don't HELLO
    g_session = nullptr;
    g_session_ready = false;
    g_netplay_frame = 0;

    // Wire up the control-channel callback. OnControlMessage already
    // dispatches SPEC_JOIN_ACK / SPEC_JOIN_REDIRECT / SPEC_HEARTBEAT / SPEC_LEAVE
    // into SpectatorNode_*, so the same handler covers spectator inbound packets.
    ControlChannel_SetCallback(OnControlMessage);

    if (!NetSocket_IsInitialized()) {
        if (!NetSocket_Init(local_port, host_addr)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "Netplay (spectator): socket init failed");
            return false;
        }
    }

    // Stand up the spectator node (initializes its state) and request to join
    // the host. The 0xCE INPUT_BATCH datagrams will start flowing back once
    // the host accepts.
    SpectatorNode_Init();

    const sockaddr_in* upstream = NetSocket_GetRemoteAddr();
    if (!upstream || upstream->sin_port == 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Netplay (spectator): no upstream addr latched");
        return false;
    }
    // Stash the original upstream as the fallback root. If our current
    // upstream later goes silent (e.g. an overflow-redirect relay died),
    // SpectatorNode_TickHealth will reconnect us here. The host always
    // serves as the root and is the always-on failback target.
    extern void SpectatorNode_SetRootAddr(const sockaddr_in& root);
    SpectatorNode_SetRootAddr(*upstream);

    SpectatorNode_RequestJoin(*upstream);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Netplay (spectator): SPEC_JOIN_REQ sent to host");
    return true;
}

void Netplay_Shutdown() {
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Netplay: Shutting down");

    if (g_session) {
        gekko_destroy(&g_session);
        g_session = nullptr;
    }

    if (ControlChannel_IsConnected()) {
        ControlChannel_SendDisconnect();
    }

    g_simple_state = SimpleState::DISCONNECTED;
    g_received_hello = false;
    g_received_hello_ack = false;
}

// =============================================================================
// STATE ACCESSORS
// =============================================================================

NetplayState Netplay_GetState() {
    return MapToLegacyState(g_simple_state);
}

void Netplay_SetState(NetplayState state) {
    (void)state;  // Ignored - we use simplified state
}

bool Netplay_IsConnected() {
    return g_simple_state >= SimpleState::CONNECTED;
}

bool Netplay_IsActive() {
    return g_simple_state == SimpleState::BATTLE && g_session != nullptr;
}

bool Netplay_IsSessionReady() {
    return g_session_ready;
}

// =============================================================================
// CSS PROCESSING - CCCaster-Style Input Delay
//
// Key insight: NEVER BLOCK THE GAME
// - Capture input every frame, store at frame + DELAY
// - Read input from frame (which we stored DELAY frames ago)
// - Send inputs to remote as fast as possible
// - Both clients advance at same rate, delay ensures sync
// =============================================================================

// Send last N CSS inputs as a batch for redundancy against packet loss.
// CCCaster-style: every packet carries recent history so dropped packets
// are recovered automatically from subsequent packets.
static void SendCSSInputBatch() {
    // Send last (DELAY + 2) frames of inputs to cover any gaps
    constexpr int BATCH_SIZE = CSS_INPUT_DELAY + 2;
    uint32_t end = g_local_inputs.GetEndFrame();
    uint32_t start = (end > BATCH_SIZE) ? end - BATCH_SIZE : 0;

    for (uint32_t f = start; f < end; f++) {
        ControlChannel_SendCSSInput(g_local_inputs.Get(f), f);
    }
}

void Netplay_PollCSS() {
    ControlChannel_Poll();
}

bool Netplay_CanAdvanceCSS() {
    // Not synced yet - let game run freely (pre-CSS or waiting for remote)
    if (!g_css_synced) {
        return true;
    }
    // Check if we have remote input for the frame we're about to process
    return g_remote_inputs.HasFrame(g_css_frame);
}

bool Netplay_ProcessCSS() {
    // Poll for incoming messages
    ControlChannel_Poll();

    // Not connected yet - let game run with local input
    if (g_simple_state < SimpleState::CONNECTED) {
        return true;
    }

    uint32_t now = GetTickCount();

    // Signal we're in CSS
    if (!g_css_active) {
        g_css_active = true;
        g_local_css_ready = true;
        ControlChannel_SendBattleReady();
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "CSS: Entered, signaling remote...");
    }

    // Keep resending BATTLE_READY until remote is synced AND sending CSS inputs.
    // Without this, if P1 syncs first and sends only one BATTLE_READY that gets
    // lost, P2 never syncs and gets stuck forever.
    static uint32_t last_ready_send = 0;
    bool remote_has_css_data = g_remote_inputs.GetEndFrame() > 0;
    if ((!g_remote_css_ready || !remote_has_css_data) && now - last_ready_send > 100) {
        ControlChannel_SendBattleReady();
        last_ready_send = now;
    }

    // Wait for both clients to be in CSS before starting frame count
    if (!g_remote_css_ready) {
        return true;  // Let game run but don't count CSS frames yet
    }

    // Initialize on first synced frame
    if (!g_css_synced) {
        g_css_synced = true;
        g_css_frame = 0;
        g_css_current_read_frame = 0;
        g_local_inputs.Clear();
        g_remote_inputs.Clear();

        // CRITICAL: Re-seed RNG now that both clients are synced.
        // Pre-CSS frames ran unsynchronized and diverged the RNG.
        // Stage selection uses RNG during CSS->battle transition,
        // so it MUST be identical from this point forward.
        *(uint32_t*)FM2K::ADDR_RANDOM_SEED = 0x12345678;

        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "CSS SYNCED: Both ready, delay=%d, RNG reseeded", CSS_INPUT_DELAY);
    }

    // ALWAYS capture local input and send to remote -- even during stalls.
    // This prevents deadlock where both sides wait for each other.
    uint16_t local_raw = Input_CaptureLocal();
    uint32_t store_frame = g_css_frame + CSS_INPUT_DELAY;
    g_local_inputs.Set(store_frame, local_raw);
    SendCSSInputBatch();  // Send batch for redundancy against packet loss

    // CCCaster-style stall: if remote input isn't ready, DON'T advance the game.
    // Our inputs are already sent above, so remote will eventually receive them.
    if (!g_remote_inputs.HasFrame(g_css_frame)) {
        return false;  // Stall - don't advance game
    }

    // Both sides have input -- advance
    g_css_current_read_frame = g_css_frame;
    g_css_frame++;

    // Forward this CSS-confirmed input pair to spectator subscribers. Same
    // pipe as battle inputs (INPUT_BATCH); spectator's local FM2K processes
    // it as a CSS frame, walks through CSS in lockstep with this host. We
    // need slot order (p1, p2), not local/remote order — translate based on
    // which player slot we are.
    {
        uint16_t local_in  = g_local_inputs.Get(g_css_current_read_frame);
        uint16_t remote_in = g_remote_inputs.Get(g_css_current_read_frame);
        uint16_t p1, p2;
        if (g_player_index == 0) { p1 = local_in;  p2 = remote_in; }
        else                     { p1 = remote_in; p2 = local_in;  }
        SpectatorNode_OnFrameConfirmed(p1, p2);
    }

    // Log occasionally
    if ((g_css_frame - 1) % 100 == 0) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "CSS: frame=%u local_end=%u remote_end=%u",
            g_css_frame - 1, g_local_inputs.GetEndFrame(), g_remote_inputs.GetEndFrame());
    }

    return true;
}

uint16_t Netplay_GetCSSInput(int player_id) {
    uint32_t read_frame = g_css_current_read_frame;

    uint16_t input;
    if (player_id == g_player_index) {
        input = g_local_inputs.Get(read_frame);
    } else {
        input = g_remote_inputs.Get(read_frame);

        // Warn if remote data is missing (shouldn't happen with delay=6 on localhost)
        static uint32_t miss_count = 0;
        if (!g_remote_inputs.HasFrame(read_frame) && miss_count < 5) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "CSS: Missing remote frame %u (remote_end=%u) - returning neutral",
                read_frame, g_remote_inputs.GetEndFrame());
            miss_count++;
        }
    }

    // CCCaster-style: Block confirm/cancel for first 150 frames (moon selector workaround)
    if (read_frame < CSS_CONFIRM_LOCKOUT) {
        // Mask out confirm (bit 8 = 0x100) and cancel (bit 9 = 0x200)
        // Note: FM2K uses different bit layout than MBAACC
        // Check what bits are confirm/cancel in FM2K and mask appropriately
        // For now, just mask bits 8-10 which are typically button presses
        input &= 0x0FF;  // Keep only direction bits
    }

    return input;
}

// =============================================================================
// BATTLE ENTRY SYNC BARRIER
// Ensures both clients enter battle mode at the same time
// =============================================================================

void Netplay_SignalBattleEntry() {
    if (g_local_battle_entered) {
        return;  // Already signaled
    }

    g_local_battle_entered = true;
    ControlChannel_SendBattleEntering();

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "BATTLE SYNC: Local entered battle mode, signaling remote...");
}

bool Netplay_IsBattleSynced() {
    // Check if sync just completed
    if (!g_battle_synced && g_local_battle_entered && g_remote_battle_entered) {
        g_battle_synced = true;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "BATTLE SYNC: Both clients in battle mode - sync complete!");
    }
    return g_battle_synced;
}

void Netplay_PollBattleSync() {
    // Poll control channel to receive BATTLE_ENTERING from remote
    ControlChannel_Poll();

    // Resend BATTLE_ENTERING until remote acknowledges
    static uint32_t last_send = 0;
    uint32_t now = GetTickCount();
    if (g_local_battle_entered && !g_remote_battle_entered && now - last_send > 50) {
        ControlChannel_SendBattleEntering();
        last_send = now;
    }
}

// =============================================================================
// GEKKONET SESSION - Battle Mode
// =============================================================================

bool Netplay_StartBattle() {
    if (g_session) {
        return true;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Netplay: Starting GekkoNet session");

    // Reset CSS state when entering battle
    g_css_active = false;
    g_css_synced = false;
    g_local_css_ready = false;
    g_remote_css_ready = false;

    // Per-player local delay via GekkoNet's native API. Each peer picks its
    // own value from its own RTT sample — no cross-peer negotiation. A laggy
    // player can add delay to smooth their experience without forcing it on
    // the opponent, which is the entire point of per-player delay.
    //   local_delay = ceil(one_way_ms / 10) + 1
    uint32_t rtt_ms = ControlChannel_GetRttMs();
    uint32_t one_way_ms = rtt_ms / 2;
    if (one_way_ms < 10) one_way_ms = 10;
    int local_delay = (int)((one_way_ms + 9) / 10) + 1;
    if (local_delay < 2)  local_delay = 2;
    if (local_delay > 15) local_delay = 15;

    int prediction_window = 8;

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "Netplay: RTT=%ums one_way=%ums -> local_delay=%d, prediction_window=%d",
        rtt_ms, one_way_ms, local_delay, prediction_window);

    GekkoConfig config = {};
    config.num_players = 2;
    config.max_spectators = 0;
    config.input_prediction_window = prediction_window;
    config.input_size = sizeof(uint16_t);
    config.state_size = sizeof(uint32_t);
    config.desync_detection = true;
    config.limited_saving = false;

    gekko_create(&g_session, GekkoGameSession);
    gekko_start(g_session, &config);

    auto adapter = CreateMultiplexAdapter();
    gekko_net_adapter_set(g_session, adapter);

    // Refresh remote address string from the actually-learned peer sockaddr.
    // g_remote_addr was written at Netplay_Init from the env var (often stale, e.g.
    // "127.0.0.1:7001" for a host before the client connects). The adapter formats
    // inbound packet addresses from the real sockaddr ("ip:port"), so the string we
    // give gekko_add_actor must match that same formatting or every packet is dropped.
    if (const sockaddr_in* learned = NetSocket_GetRemoteAddr()) {
        if (learned->sin_port != 0) {
            char ip_buf[INET_ADDRSTRLEN] = {};
            inet_ntop(AF_INET, (void*)&learned->sin_addr, ip_buf, sizeof(ip_buf));
            snprintf(g_remote_addr, sizeof(g_remote_addr), "%s:%u",
                     ip_buf, ntohs(learned->sin_port));
        }
    }

    for (int i = 0; i < 2; i++) {
        if (i == g_player_index) {
            gekko_add_actor(g_session, GekkoLocalPlayer, nullptr);
            gekko_set_local_delay(g_session, i, local_delay);
            g_local_delay = local_delay;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Netplay: Added local player at slot %d (delay=%d)", i, local_delay);
        } else {
            GekkoNetAddress addr = {};
            addr.data = (void*)g_remote_addr;
            addr.size = (int)strlen(g_remote_addr);
            gekko_add_actor(g_session, GekkoRemotePlayer, &addr);
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Netplay: Added remote player at slot %d -> %s", i, g_remote_addr);
        }
    }

    // Runahead: speculatively advance local frames past confirmed input to
    // hide input delay. GekkoNet rewinds runahead frames each tick and replays
    // them — free latency reduction at the cost of extra sim work. 4 is a safe
    // default; tune via UI once exposed. Zero disables.
    gekko_set_runahead(g_session, 4);

    *(uint32_t*)FM2K::ADDR_RANDOM_SEED = 0x12345678;
    SaveState_Init();
    SoundRollback::Init();

    // Start recording a replay of this match. Captures initial RNG +
    // char selects + per-frame inputs; on battle end we flush to disk and
    // cache for spectator streaming. Character slots / colors are read from
    // CSS state — TODO once CSS exposes them; pass 0s for now so replays
    // still work, just without char metadata.
    const uint32_t initial_seed = 0x12345678;
    const uint32_t initial_state_hash =
        SaveState_GetRegionChecksums().gameplay_fingerprint;
    Replay::Replay_BeginRecording(
        /*game_hash*/         0,  // TODO: plumb FM2K variant hash
        /*initial_rng_seed*/  initial_seed,
        /*initial_state_hash*/initial_state_hash,
        /*p1_char*/           0, /*p1_color*/ 0, /*p1_name*/ nullptr,
        /*p2_char*/           0, /*p2_color*/ 0, /*p2_name*/ nullptr);

    // Notify the spectator tree: start of a new match, push INITIAL_MATCH to
    // any currently-subscribed viewers so they reset and follow this match.
    SpectatorNode_OnMatchStart(
        /*game_hash*/         0,
        /*initial_rng_seed*/  initial_seed,
        /*initial_state_hash*/initial_state_hash,
        /*p1_char*/0, /*p1_color*/0,
        /*p2_char*/0, /*p2_color*/0);

    g_simple_state = SimpleState::BATTLE;
    g_session_ready = false;
    g_netplay_frame = 0;
    g_highest_recorded_frame = 0;  // monotonic dedup gate, reset per battle
    g_rollback_count = 0;
    g_last_rollback_frame = 0;
    g_desync_count = 0;
    g_last_desync_log_tick = 0;

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Netplay: GekkoNet session created (runahead=4)");
    return true;
}

// Single-instance determinism test: GekkoStressSession with both players local
// and no network adapter. GekkoNet rewinds every check_distance frames and
// compares checksums, flagging any sim divergence as a desync event.
bool Netplay_StartStressBattle() {
    if (g_session) {
        return true;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "Netplay: Starting GekkoStressSession (single-instance determinism test)");

    GekkoConfig config = {};
    config.num_players = 2;
    config.max_spectators = 0;
    config.input_prediction_window = 8;
    config.input_size = sizeof(uint16_t);
    config.state_size = sizeof(uint32_t);
    config.desync_detection = true;
    config.limited_saving = false;
    // StressSession-specific: force a rollback every 10 frames so GekkoNet
    // re-simulates from a saved state and compares checksums against the
    // original advance. Any mismatch fires GekkoDesyncDetected.
    config.check_distance = 10;

    // StressSession mode: ignores network, forces rollbacks from a single instance.
    gekko_create(&g_session, GekkoStressSession);
    gekko_start(g_session, &config);

    // Both actors local. No adapter set -> no network calls.
    for (int i = 0; i < 2; i++) {
        gekko_add_actor(g_session, GekkoLocalPlayer, nullptr);
        gekko_set_local_delay(g_session, i, 1);
    }
    g_local_delay = 1;

    // Deterministic initial RNG seed (matches Netplay_StartBattle).
    *(uint32_t*)FM2K::ADDR_RANDOM_SEED = 0x12345678;
    SaveState_Init();
    SoundRollback::Init();

    g_simple_state = SimpleState::BATTLE;
    g_session_ready = true;   // no handshake needed for stress
    g_netplay_frame = 0;
    g_highest_recorded_frame = 0;
    g_rollback_count = 0;
    g_last_rollback_frame = 0;
    g_desync_count = 0;
    g_last_desync_log_tick = 0;

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "Netplay: GekkoStressSession created (check_distance=%d, prediction_window=%d)",
        10, config.input_prediction_window);
    return true;
}

void Netplay_EndBattle() {
    if (g_session) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Netplay: Ending GekkoNet session");
        gekko_destroy(&g_session);
        g_session = nullptr;
    }

    // Finalize replay file for this match and cache the in-memory copy for
    // spectator streaming. No-op if recording wasn't active.
    Replay::Replay_EndRecording();

    // Tell the spectator tree the match is over — subscribers receive
    // MATCH_END and go idle until the next SpectatorNode_OnMatchStart.
    SpectatorNode_OnMatchEnd();

    // Stop any pending SFX "desired" entries and clear the channel map so
    // the next battle rescans the channel table (handles character-load
    // changes between matches).
    SoundRollback::OnBattleEnd();

    // Flush the rngtrace ring on clean battle exit too, so a desync-free
    // round still produces a trace to diff against a future desync run.
    SaveState_FlushRngTrace(g_player_index, "battle end");

    g_session_ready = false;
    g_simple_state = SimpleState::CONNECTED;

    // Reset CSS state for rematch
    g_css_active = false;
    g_css_synced = false;
    g_local_css_ready = false;
    g_remote_css_ready = false;

    // Reset battle sync state for next battle
    g_local_battle_entered = false;
    g_remote_battle_entered = false;
    g_battle_synced = false;
}

// =============================================================================
// BATTLE FRAME PROCESSING
// =============================================================================

// Frame pacing state (matches GekkoNet examples' handle_frame_time)
static LARGE_INTEGER g_perf_freq = {};
static LARGE_INTEGER g_frame_start = {};
static bool g_frame_timer_initialized = false;

// Called at the END of each frame to apply frame advantage throttle.
// The game already has its OWN 10ms frame limiter (timeGetTime in WinMain).
// We only add EXTRA delay when ahead of remote to prevent rollback cascade.
// Without this, we'd double-limit (game's 10ms + our 10ms = 20ms = 50fps).
static void HandleFrameTime(float frames_ahead) {
    // Only throttle when ahead -- the game handles base frame timing
    if (frames_ahead <= 0.5f) {
        return;  // Not ahead, let game's own limiter handle timing
    }

    // Scale extra delay proportionally to advantage:
    // 0.5-1.0 ahead: +0.16ms (1.6% of 10ms)
    // 1.0-2.0 ahead: +0.5ms
    // 2.0-4.0 ahead: +1.0ms
    // 4.0+ ahead:    +2.0ms
    DWORD extra_ms;
    if (frames_ahead > 4.0f) {
        extra_ms = 2;
    } else if (frames_ahead > 2.0f) {
        extra_ms = 1;
    } else if (frames_ahead > 1.0f) {
        // Sleep(0) yields timeslice, ~0.5ms effective
        Sleep(0);
        return;
    } else {
        // 0.5-1.0: minimal throttle, just yield
        Sleep(0);
        return;
    }

    Sleep(extra_ms);
}

bool Netplay_ProcessBattleInputPhase() {
    if (!g_session) return true;

    // In stress mode, skip the network poll (no adapter, no socket).
    if (!g_stress_mode) {
        gekko_network_poll(g_session);
    }

    uint16_t local_input = Input_CaptureLocal();
    if (g_stress_mode) {
        // Single-instance determinism test: add the same input for BOTH players.
        // StressSession replays each frame to detect any sim-state divergence
        // between the initial simulate pass and the rollback-re-simulate pass.
        // Using identical input on both slots keeps the test self-contained.
        gekko_add_local_input(g_session, 0, &local_input);
        gekko_add_local_input(g_session, 1, &local_input);
    } else {
        gekko_add_local_input(g_session, g_player_index, &local_input);
    }

    int event_count = 0;
    auto events = gekko_session_events(g_session, &event_count);
    for (int i = 0; i < event_count; i++) {
        auto event = events[i];
        switch (event->type) {
            case GekkoPlayerConnected:
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Netplay: GekkoNet player %d connected",
                    event->data.connected.handle);
                g_session_ready = true;
                break;

            case GekkoSessionStarted:
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Netplay: GekkoNet session started");
                g_session_ready = true;
                break;

            case GekkoPlayerSyncing:
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Netplay: GekkoNet syncing %u/%u",
                    event->data.syncing.current, event->data.syncing.max);
                break;

            case GekkoDesyncDetected: {
                g_desync_count++;
                uint32_t now_tick = GetTickCount();

                // Always log the first desync with full detail
                if (g_desync_count <= 5) {
                    auto& rc = SaveState_GetRegionChecksums();
                    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                        "DESYNC #%u f=%d: local=0x%08X remote=0x%08X",
                        g_desync_count,
                        event->data.desynced.frame,
                        event->data.desynced.local_checksum,
                        event->data.desynced.remote_checksum);
                    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                        "  SAVED: rng=0x%08X game=0x%08X obj=0x%08X char=0x%08X inp=0x%08X",
                        rc.rng, rc.game_state, rc.object_pool, rc.char_dynamic, rc.input_tracking);
                    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                        "  UNSAVED: eff1=0x%08X eff2=0x%08X shake=0x%08X",
                        rc.effect_sys1, rc.effect_sys2, rc.shake_effects);
                    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                        "  FINGERPRINT: gameplay=0x%08X (HP/pos/rng/timer only — "
                        "if this MATCHES across peers the desync is a memory-residue "
                        "false positive)",
                        rc.gameplay_fingerprint);
                } else if (now_tick - g_last_desync_log_tick > 1000) {
                    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                        "DESYNC #%u f=%d: local=0x%08X remote=0x%08X",
                        g_desync_count,
                        event->data.desynced.frame,
                        event->data.desynced.local_checksum,
                        event->data.desynced.remote_checksum);
                    g_last_desync_log_tick = now_tick;
                }

                // BBBR-style: Dump per-region CRC + hex to file on first desync
                if (g_desync_count == 1) {
                    SaveState_DumpDesyncDiagnostic(
                        event->data.desynced.frame,
                        event->data.desynced.local_checksum,
                        event->data.desynced.remote_checksum,
                        g_player_index);
                    // Also flush the in-memory rngtrace ring so we can diff
                    // the two peers' per-frame rng history offline.
                    SaveState_FlushRngTrace(g_player_index, "first desync");

                    // Stress mode is a determinism test - once we've caught a
                    // desync we have full diagnostic evidence already. Kill the
                    // game process immediately so the user can inspect the dump
                    // without having to manually close the window, and so
                    // subsequent divergences don't scribble over the diagnostic
                    // region's frozen state (the object pool / player slots /
                    // afterimage pool contents captured at the moment of the
                    // first divergence are the valuable evidence; later frames'
                    // values drift further from ground truth and obscure it).
                    if (g_stress_mode) {
                        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                            "STRESS: Desync detected on frame %d. "
                            "Dump written to FM2K_stress_desync_f%d.log. "
                            "Terminating game for clean inspection.",
                            event->data.desynced.frame,
                            event->data.desynced.frame);
                        // Flush any pending log output before exit
                        fflush(stdout);
                        fflush(stderr);
                        // Hard-exit the game process. We use TerminateProcess on
                        // the current process so we don't get stuck in atexit
                        // handlers running on the injected DLL's static objects
                        // (which can race with the hook shutdown path).
                        TerminateProcess(GetCurrentProcess(), 1);
                    }
                }
                break;
            }

            default:
                break;
        }
    }

    int update_count = 0;
    auto updates = gekko_update_session(g_session, &update_count);

    bool has_advance = false;
    // Track the advance-batch window for the Mike Z sound sync. Forward-only
    // batches have earliest == latest; rollback batches span the rewind range.
    uint32_t earliest_advance = UINT32_MAX;
    uint32_t latest_advance = g_netplay_frame;
    // Per-batch LoadEvent counter. GekkoNet's update sequence is:
    //   RewindRunahead -> [maybe HandleRollback] -> Advance -> Save
    //   -> [HandleRunahead emits Save + N speculative Advances]
    // RewindRunahead unconditionally emits a LoadEvent once runahead has
    // run at least once (to undo last tick's speculative advances). It
    // is NOT a real rollback. Real rollbacks emit a SECOND LoadEvent in
    // the same batch from HandleRollback. The first LoadEvent of every
    // batch is therefore the runahead rewind; only the 2nd+ should be
    // counted as user-visible rollbacks.
    int load_events_in_batch = 0;
    for (int i = 0; i < update_count; i++) {
        auto update = updates[i];
        switch (update->type) {
            case GekkoSaveEvent: {
                int frame = update->data.save.frame;
                SaveState_Save(frame);
                (void)SaveState_GetLastChecksum(frame);  // updates RegionChecksums via side effect
                // Use the gameplay fingerprint as GekkoNet's desync checksum.
                // If both peers see the same HP/pos/rng/timer values at this
                // frame, fingerprints match and GekkoNet stays happy even when
                // per-process memory residue differs. Full combined hash is
                // still computed and available for the diagnostic dump.
                uint32_t checksum = SaveState_GetRegionChecksums().gameplay_fingerprint;
                *update->data.save.state_len = sizeof(uint32_t);
                *update->data.save.checksum = checksum;
                memcpy(update->data.save.state, &frame, sizeof(uint32_t));
                break;
            }

            case GekkoLoadEvent: {
                int frame = update->data.load.frame;
                load_events_in_batch++;
                const bool is_runahead_rewind = (load_events_in_batch == 1);
                if (!is_runahead_rewind) {
                    g_rollback_count++;
                }
                g_last_rollback_frame = g_netplay_frame;
                // Rolling stats only — per-rollback SDL_LogInfo at 100 fps
                // chewed framerate under stress. Ring buffer + summary every
                // 100 events keeps the signal (rollback count, last frame,
                // typical rewind distance) without the cost.
                static uint32_t s_rb_window_start = 0;
                static uint32_t s_rb_window_rewind_sum = 0;
                static uint32_t s_rb_window_rewind_max = 0;
                if (g_rollback_count == 1) {
                    s_rb_window_start = 0;
                    s_rb_window_rewind_sum = 0;
                    s_rb_window_rewind_max = 0;
                }
                uint32_t rewind = g_netplay_frame - (uint32_t)frame;
                s_rb_window_rewind_sum += rewind;
                if (rewind > s_rb_window_rewind_max) s_rb_window_rewind_max = rewind;
                if (g_rollback_count % 100 == 0) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "ROLLBACK stats: total=%u, last-100 avg_rewind=%.1f max_rewind=%u (last frame=%d current=%u)",
                        g_rollback_count,
                        (double)s_rb_window_rewind_sum / 100.0,
                        s_rb_window_rewind_max,
                        frame, g_netplay_frame);
                    s_rb_window_rewind_sum = 0;
                    s_rb_window_rewind_max = 0;
                }
                SaveState_Load(frame);
                // Rewind the deterministic virtual clock to match the loaded
                // frame. Without this g_virtual_time_ms stays at its forward-sim
                // value, replay main_game_loop iterations read timeGetTime()
                // values AHEAD of where forward was at the same frame, and the
                // self-referential arithmetic at main_game_loop 0x405BA8/0x405BBB
                // (g_last_frame_time += N*10) writes a different byte pattern
                // into g_last_frame_time @ 0x447DD4 — which is what produced
                // the "AfterimagePool +0x4A4" divergence at f=9.
                extern uint32_t g_virtual_time_ms;
                g_virtual_time_ms = (uint32_t)frame * 10;
                // Also rewind g_netplay_frame so the per-advance increments
                // below produce the same frame numbers the forward pass did.
                g_netplay_frame = (uint32_t)frame;
                break;
            }

            case GekkoAdvanceEvent: {
                // Record the pre-sim frame for the Mike Z sound sync window.
                // `g_netplay_frame` here is the frame we're about to *produce*;
                // Hook_DispatchScriptSoundCommand tags desired[] with this same
                // value (via Netplay_GetFrame) so earliest/latest compare apples
                // to apples.
                if (g_netplay_frame < earliest_advance) {
                    earliest_advance = g_netplay_frame;
                }

                // Set inputs for THIS frame
                if (update->data.adv.inputs && update->data.adv.input_len >= sizeof(uint16_t) * 2) {
                    uint16_t* inputs = (uint16_t*)update->data.adv.inputs;
                    g_p1_input = inputs[0] & 0x7FF;
                    g_p2_input = inputs[1] & 0x7FF;
                }

                // Track rollback state (matching BBBR reference implementation).
                // During rollback replay, input edge detection globals must NOT
                // be overwritten - they get restored by LoadEvent and should only
                // be updated on the authoritative (non-rollback) frame.
                g_is_rolling_back = update->data.adv.rolling_back;

                // [REMOVED] per-slot fan-out at slot+0xDF79 / slot+0xDF7D.
                // main_game_loop's prologue writes these every iteration,
                // but IDA xref of g_p1_input_buffer_index_field (0x4DFD0D)
                // shows the only reader is check_game_continue, which is
                // a no-op when g_directplay_interface == NULL — and we
                // never initialize DirectPlay. KGT scripts and
                // update_game/process_game_inputs do not read these
                // fields. The writes were just trampling adjacent slot
                // memory the StudioS chars happen to use, breaking
                // their scripts.

                // Run a FULL game tick for EVERY AdvanceEvent (matching GekkoNet examples).
                // The game loop must NOT run its own tick - we handle everything here.
                if (original_process_game_inputs) {
                    original_process_game_inputs();
                }
                if (original_update_game) {
                    original_update_game();
                }

                g_is_rolling_back = false;
                g_netplay_frame++;
                has_advance = true;

                // Replay recording + spectator stream. Gate on TRUE
                // confirmed advances only:
                //
                //   running_ahead=true → speculative runahead with
                //     PREDICTED inputs. We must NOT record these — when
                //     the prediction turns out wrong, GekkoNet rolls back
                //     and replays with the real input, but our recorded
                //     session_history would already contain the wrong
                //     prediction → spectator processes wrong inputs →
                //     desync. (This was the running-ahead-after-resync
                //     bug.)
                //   rolling_back=true → mid-rollback replay. Inputs are
                //     correct (post-confirmation) but we already recorded
                //     this frame on its first forward pass.
                //   monotonic gate → belt-and-suspenders against any
                //     other re-advance pattern.
                //
                // Only !rolling_back && !running_ahead && new-frame ever
                // hits session_history.
                if (!update->data.adv.rolling_back &&
                    !update->data.adv.running_ahead &&
                    g_netplay_frame > g_highest_recorded_frame)
                {
                    g_highest_recorded_frame = g_netplay_frame;
                    Replay::Replay_RecordFrame(g_p1_input, g_p2_input);
                    SpectatorNode_OnFrameConfirmed(g_p1_input, g_p2_input);
                }

                // Shake safety cap removed — the real fix is letting the
                // render-path timer decrement persist (see carve-out in
                // main_loop_trampoline.cpp:RenderFrameWithSnapshot). With
                // that in place, the KGT-scripted `Duration` value the
                // character author wrote drives how long shake lasts, and
                // the timer naturally reaches 0 -> ProcessShakeEffect zeros
                // the visible offset -> shake ends at exactly the designed
                // frame count. No per-frame force-clear needed.

                // Advance the deterministic virtual clock used by Hook_timeGetTime.
                // Exactly one bump per AdvanceEvent.
                extern uint32_t g_virtual_time_ms;
                g_virtual_time_ms += 10;

                // NOTE: we intentionally do NOT write g_last_frame_time here.
                // Setting it equal to g_virtual_time_ms made
                // main_game_loop's frame-skip check
                //   Time (= timeGetTime = g_virtual_time_ms) >= g_last_frame_time + 10
                // always false, so the game stopped running new iterations
                // (session hang at the save just before the write would have
                // fired). Leave g_last_frame_time to main_game_loop's own
                // prologue writes; if the forward-vs-replay divergence at
                // 0x447DD4 reappears we'll mask it at save time instead of
                // trying to drive it from here.

                // Tell Hook_RenderGame there is an unrendered authoritative
                // tick. The render hook is allowed to draw exactly this one
                // sim state, then clears the flag. Rolled-back replay ticks
                // do not set the flag — we only render the final resolved
                // state of an advance, never the intermediate replay frames.
                extern bool g_frame_pending_render;
                if (!update->data.adv.rolling_back) {
                    g_frame_pending_render = true;
                }

                // Periodic status every 500 frames (~5 sec). Gate on
                // NON-rolling_back advances only — in stress mode g_netplay_frame
                // hits any given value N times (once forward, N-1 times on
                // replay) so a naive `% 500 == 0` fires on every replay
                // pass and floods the log. Also dedupe by last-logged frame
                // so rollback that re-hits a multiple-of-500 frame doesn't
                // re-log.
                if (!update->data.adv.rolling_back) {
                    static uint32_t last_status_frame = 0;
                    if (g_netplay_frame >= last_status_frame + 500) {
                        last_status_frame = g_netplay_frame;
                        float fa = g_session ? gekko_frames_ahead(g_session) : 0.0f;
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "BATTLE STATUS: frame=%u rb=%u desync=%u ahead=%.1f frame_time_ms=%u skip=%u",
                            g_netplay_frame, g_rollback_count, g_desync_count, fa,
                            *(uint32_t*)0x41E2F0,
                            *(uint32_t*)0x4246F4);
                    }

                    static uint32_t last_state_frame = 0;
                    if (g_netplay_frame >= last_state_frame + 1000) {
                        last_state_frame = g_netplay_frame;
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "STATE f=%u: rng=0x%08X game_timer=%u round_timer_ctr=%u render_fc=%u",
                            g_netplay_frame,
                            *(uint32_t*)0x41FB1C,
                            *(uint32_t*)0x470044,
                            *(uint32_t*)0x47008E,
                            *(uint32_t*)0x4456FC);
                    }
                }
                break;
            }

            default:
                break;
        }
    }

    // Mike Z sound sync: once per displayed frame, AFTER all advances have
    // completed and BEFORE render. Desired[] now reflects the authoritative
    // sim history for this frame; reconcile to actual DSound plays, applying
    // the rollback-window filter that prevents erased/re-triggered sounds.
    if (has_advance && earliest_advance != UINT32_MAX) {
        latest_advance = g_netplay_frame;
        SoundRollback::SyncAfterAdvance(earliest_advance, latest_advance);
    }

    return has_advance;
}

uint16_t Netplay_GetInput(int player_id) {
    return (player_id == 0) ? g_p1_input : g_p2_input;
}

// =============================================================================
// LEGACY API
// =============================================================================

bool Netplay_StartGekkoSession() {
    return Netplay_StartBattle();
}

void Netplay_StopGekkoSession() {
    Netplay_EndBattle();
}

void Netplay_PollGekkoNet() {
    if (g_session) {
        gekko_network_poll(g_session);
        int event_count = 0;
        auto events = gekko_session_events(g_session, &event_count);
        for (int i = 0; i < event_count; i++) {
            auto event = events[i];
            if (event->type == GekkoSessionStarted || event->type == GekkoPlayerConnected) {
                g_session_ready = true;
            }
        }
    }
}

void Netplay_ResetCSSState() {
    g_local_inputs.Clear();
    g_remote_inputs.Clear();
    g_css_start_timer = 0;
    g_css_frame = 0;
    g_css_current_read_frame = 0;
    g_css_active = false;
    g_css_synced = false;
    g_remote_css_ready = false;
    g_local_css_ready = false;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "CSS: Reset state for new sync");
}

bool Netplay_IsRemoteCSSReady() {
    return g_remote_css_ready;
}

bool Netplay_IsCSSFullySynced() {
    return g_css_synced;
}

void Netplay_SetLocalCSSReady(bool ready) {
    g_local_css_ready = ready;
    if (ready) {
        ControlChannel_SendBattleReady();
    }
}

void Netplay_ProcessMenu() {
    ControlChannel_Poll();
}

const CSSState& Netplay_GetCSSState() {
    static CSSState dummy;
    return dummy;
}

uint32_t Netplay_GetFrame() {
    return g_netplay_frame;
}

uint32_t Netplay_GetPingMs() {
    return ControlChannel_GetRttMs();
}

int Netplay_GetLocalDelay() {
    return g_local_delay;
}

// -----------------------------------------------------------------------------
// Chat ring. Small fixed-size SPSC-ish ring since both push and pop run on
// the launcher-UI side; the only cross-thread producer is OnControlMessage
// via the control-channel poller. Size 64 is plenty for a single match's
// worth of unread messages.
// -----------------------------------------------------------------------------
static constexpr size_t CHAT_RING_CAP = 64;
static ChatEntry g_chat_ring[CHAT_RING_CAP];
static size_t    g_chat_head = 0;
static size_t    g_chat_tail = 0;

void Netplay_PushChatMessage(bool from_remote, const char* text) {
    if (!text) return;
    ChatEntry e = {};
    e.from_remote  = from_remote;
    e.timestamp_ms = (uint64_t)GetTickCount64();
    std::strncpy(e.text, text, sizeof(e.text) - 1);
    e.text[sizeof(e.text) - 1] = '\0';

    size_t next = (g_chat_head + 1) % CHAT_RING_CAP;
    if (next == g_chat_tail) {
        // Ring full — drop oldest to keep the newest message visible.
        g_chat_tail = (g_chat_tail + 1) % CHAT_RING_CAP;
    }
    g_chat_ring[g_chat_head] = e;
    g_chat_head = next;
}

bool Netplay_PopChatMessage(ChatEntry* out) {
    if (g_chat_tail == g_chat_head) return false;
    if (out) *out = g_chat_ring[g_chat_tail];
    g_chat_tail = (g_chat_tail + 1) % CHAT_RING_CAP;
    return true;
}

void Netplay_SendChatMessage(const char* text) {
    if (!text) return;
    if (!Netplay_IsConnected()) return;
    ControlChannel_SendChat(text);
    // Echo into local ring so the sender sees their own message in the UI.
    Netplay_PushChatMessage(/*from_remote*/ false, text);
}

GekkoNetworkStats Netplay_GetNetworkStats() {
    GekkoNetworkStats stats = {};
    if (g_session) {
        // Get stats for the remote player (whichever slot isn't us)
        int remote_handle = (g_player_index == 0) ? 1 : 0;
        gekko_network_stats(g_session, remote_handle, &stats);
    }
    return stats;
}

uint32_t Netplay_GetDesyncCount() {
    return g_desync_count;
}

uint32_t Netplay_GetRollbackCount() {
    return g_rollback_count;
}

float Netplay_GetFramesAhead() {
    if (g_session) {
        return gekko_frames_ahead(g_session);
    }
    return 0.0f;
}

void Netplay_HandleFrameTime() {
    float ahead = g_session ? gekko_frames_ahead(g_session) : 0.0f;
    HandleFrameTime(ahead);
}
