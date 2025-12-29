// Netplay - State Machine + GekkoNet Integration
// Two-layer networking:
//   1. Control Channel (always active when connected) - CSS sync, transitions
//   2. GekkoNet Session (battle only) - full rollback netcode
#include "netplay.h"
#include "control_channel.h"
#include "input.h"
#include "savestate.h"
#include "globals.h"
#include "gekkonet.h"
#include <SDL3/SDL_log.h>
#include <cstring>
#include <chrono>

// =============================================================================
// NETPLAY STATE - Minimal state sent to GekkoNet, checksum from full savestate
// =============================================================================

// Only frame + RNG sent to GekkoNet - checksum comes from full SaveState
struct NetplayFrameState {
    uint32_t frame;
    uint32_t rng_seed;  // For RNG restoration after rollback
};

static_assert(sizeof(NetplayFrameState) == 8, "NetplayFrameState size changed!");

// Desync tracking
static bool g_desync_detected = false;

// =============================================================================
// INTERNAL STATE
// =============================================================================

// State machine
static NetplayState g_state = NetplayState::DISCONNECTED;
static CSSState g_css_state;

// GekkoNet session
static GekkoSession* g_session = nullptr;
static bool g_session_ready = false;

// Note: g_player_index comes from globals.h

// Synchronized inputs (from GekkoNet during battle)
static uint16_t g_p1_input = 0;
static uint16_t g_p2_input = 0;
static bool g_can_advance = false;

// CSS input synchronization
static uint16_t g_local_css_input = 0;   // Our captured input
static uint16_t g_remote_css_input = 0;  // Remote player's input
static uint32_t g_remote_css_frame = 0;  // Frame number of last received input
static uint32_t g_css_frame = 0;         // Current CSS frame counter
static bool g_css_input_received = false; // Have we received remote input for this frame?

// Frame counter
static uint32_t g_netplay_frame = 0;

// CSS tracking
static uint8_t g_last_local_action_state = 0;
static int g_cursor_send_timer = 0;

// Battle ready handshake
static bool g_local_battle_ready = false;
static bool g_remote_battle_ready = false;

// Connection retry
static uint32_t g_last_hello_time = 0;
static constexpr uint32_t HELLO_RETRY_INTERVAL_MS = 500;  // Retry every 500ms

// Forward declarations
static void ProcessConnectionRetry();
static uint32_t GetTimeMs();

// =============================================================================
// CONTROL MESSAGE HANDLER
// =============================================================================

static void OnControlMessage(const CtrlPacket* packet) {
    int remote_player = 1 - g_player_index;

    switch (packet->header.type) {
        case CtrlMsg::HELLO:
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Netplay: Received HELLO from player %d (version=%d)",
                        packet->data.hello.player_id, packet->data.hello.version);

            // Send acknowledgment
            ControlChannel_SendHelloAck(static_cast<uint8_t>(g_player_index));
            Netplay_SetState(NetplayState::SYNCED);
            break;

        case CtrlMsg::HELLO_ACK:
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Netplay: Received HELLO_ACK from player %d",
                        packet->data.hello.player_id);

            ControlChannel_SetConnected(true);
            Netplay_SetState(NetplayState::SYNCED);
            break;

        case CtrlMsg::PING:
            // Respond with PONG
            {
                CtrlPacket pong = {};
                pong.header.type = CtrlMsg::PONG;
                pong.data.sync.frame = packet->data.sync.frame;  // Echo back for RTT
                ControlChannel_Send(pong);
            }
            break;

        case CtrlMsg::PONG:
            ControlChannel_HandlePong(packet->data.sync.frame);
            break;

        case CtrlMsg::CSS_INPUT:
            // Store remote player's CSS input with frame info for lockstep
            g_remote_css_input = packet->data.css_input.input;
            g_remote_css_frame = packet->data.css_input.frame;
            g_css_input_received = true;
            // Log first few receives
            {
                static uint32_t css_recv_count = 0;
                if (++css_recv_count <= 5 || css_recv_count % 100 == 0) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "CSS_INPUT received: frame=%u input=0x%04x (recv #%u)",
                        g_remote_css_frame, g_remote_css_input, css_recv_count);
                }
            }
            break;

        case CtrlMsg::CSS_CURSOR:
            // Update remote player's cursor position [deprecated]
            g_css_state.cursor_x[remote_player] = packet->data.cursor.x;
            g_css_state.cursor_y[remote_player] = packet->data.cursor.y;
            break;

        case CtrlMsg::CSS_CHAR_SELECT:
            // Update remote player's character selection
            g_css_state.selected_char[remote_player] = packet->data.character.slot;
            g_css_state.selected_color[remote_player] = packet->data.character.color;
            break;

        case CtrlMsg::CSS_LOCK:
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Netplay: Remote player locked character (slot=%d, color=%d)",
                        packet->data.character.slot, packet->data.character.color);

            g_css_state.selected_char[remote_player] = packet->data.character.slot;
            g_css_state.selected_color[remote_player] = packet->data.character.color;
            g_css_state.locked[remote_player] = true;

            // Check if both ready
            if (g_css_state.locked[0] && g_css_state.locked[1]) {
                Netplay_SetState(NetplayState::CSS_BOTH_READY);
            } else {
                Netplay_SetState(remote_player == 0 ?
                    NetplayState::CSS_REMOTE_READY :
                    NetplayState::CSS_LOCAL_READY);
            }
            break;

        case CtrlMsg::CSS_UNLOCK:
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Netplay: Remote player unlocked character");

            g_css_state.locked[remote_player] = false;

            // Reset to lobby if we were ready
            if (g_state >= NetplayState::CSS_LOCAL_READY) {
                Netplay_SetState(NetplayState::CSS_LOBBY);
            }
            break;

        case CtrlMsg::BATTLE_READY:
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Netplay: Remote player is battle ready");

            g_remote_battle_ready = true;

            // Acknowledge
            ControlChannel_SendBattleAck();
            break;

        case CtrlMsg::BATTLE_ACK:
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Netplay: Remote acknowledged battle ready");

            g_remote_battle_ready = true;
            break;

        case CtrlMsg::BATTLE_START:
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Netplay: Battle starting at frame %u",
                        packet->data.sync.frame);

            g_netplay_frame = packet->data.sync.frame;
            Netplay_SetState(NetplayState::BATTLE_RUNNING);
            break;

        case CtrlMsg::BATTLE_END:
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Netplay: Battle ended");

            Netplay_SetState(NetplayState::BATTLE_END);
            break;

        case CtrlMsg::DISCONNECT:
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Netplay: Remote player disconnected");

            ControlChannel_SetConnected(false);
            Netplay_SetState(NetplayState::DISCONNECTED);
            break;

        default:
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Netplay: Unknown control message type: %d",
                        static_cast<int>(packet->header.type));
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
    g_css_state.Reset();
    g_state = NetplayState::DISCONNECTED;
    g_session = nullptr;
    g_session_ready = false;
    g_local_battle_ready = false;
    g_remote_battle_ready = false;
    g_netplay_frame = 0;

    // Initialize control channel callback
    ControlChannel_SetCallback(OnControlMessage);

    // Initialize socket (if not already done by dllmain)
    if (!NetSocket_IsInitialized()) {
        if (!NetSocket_Init(local_port, remote_addr)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Netplay: Failed to init socket");
            return false;
        }
    }

    // Start connection process
    Netplay_SetState(NetplayState::CONNECTING);

    // Send HELLO to initiate connection
    uint32_t game_hash = 0;  // TODO: Calculate from game files
    ControlChannel_SendHello(static_cast<uint8_t>(player_index), game_hash);

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Netplay: Initialized, connecting...");
    return true;
}

void Netplay_Shutdown() {
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Netplay: Shutting down");

    // Stop GekkoNet session if active
    Netplay_StopGekkoSession();

    // Send disconnect notification
    if (ControlChannel_IsConnected()) {
        ControlChannel_SendDisconnect();
    }

    // Don't shutdown socket here - that's done by NetSocket_Shutdown()

    g_state = NetplayState::DISCONNECTED;
    g_css_state.Reset();
}

// =============================================================================
// STATE MACHINE
// =============================================================================

NetplayState Netplay_GetState() {
    return g_state;
}

void Netplay_SetState(NetplayState state) {
    if (g_state != state) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Netplay: State %s -> %s",
                    NetplayStateToString(g_state),
                    NetplayStateToString(state));
        g_state = state;
    }
}

const CSSState& Netplay_GetCSSState() {
    return g_css_state;
}

// =============================================================================
// CSS PROCESSING
// =============================================================================

bool Netplay_ProcessCSS() {
    // Poll control channel for incoming messages
    ControlChannel_Poll();

    // If not connected yet, retry connection
    if (g_state == NetplayState::CONNECTING) {
        ProcessConnectionRetry();
        return false;  // Don't advance until connected
    }

    // If disconnected, nothing to do
    if (g_state == NetplayState::DISCONNECTED) {
        return true;  // Run offline
    }

    // Transition from SYNCED to CSS_LOBBY when we enter CSS mode
    if (g_state == NetplayState::SYNCED) {
        Netplay_SetState(NetplayState::CSS_LOBBY);
        g_css_frame = 0;
        g_css_input_received = false;
    }

    // ========================================================================
    // ASYNC CSS: Exchange inputs without blocking (like CCCaster)
    // No lockstep needed - CSS doesn't require frame-perfect sync
    // ========================================================================

    // Capture local input
    g_local_css_input = Input_CaptureLocal();

    // Send our input periodically (every few frames to reduce traffic)
    static uint32_t last_send_frame = 0;
    if (g_css_frame - last_send_frame >= 2) {  // Send every 2 frames
        ControlChannel_SendCSSInput(g_local_css_input, g_css_frame);
        last_send_frame = g_css_frame;
    }

    // Always advance - no waiting for remote input
    g_css_frame++;

    // Detect character lock/unlock changes for state machine
    uint8_t* p1_action = (uint8_t*)FM2K::ADDR_P1_ACTION_STATE;
    uint8_t* p2_action = (uint8_t*)FM2K::ADDR_P2_ACTION_STATE;
    uint8_t local_action_state = (g_player_index == 0) ? *p1_action : *p2_action;

    if (local_action_state != g_last_local_action_state) {
        if (local_action_state == 1) {
            // Character locked
            int32_t* stage_pos = (int32_t*)FM2K::ADDR_PLAYER_STAGE_POSITIONS;
            uint8_t slot = (g_player_index == 0) ?
                static_cast<uint8_t>(stage_pos[0]) :
                static_cast<uint8_t>(stage_pos[1]);

            ControlChannel_SendCharLock(slot, 0);

            g_css_state.selected_char[g_player_index] = slot;
            g_css_state.locked[g_player_index] = true;

            // Check if both ready
            if (g_css_state.locked[0] && g_css_state.locked[1]) {
                Netplay_SetState(NetplayState::CSS_BOTH_READY);
            } else {
                Netplay_SetState(g_player_index == 0 ?
                    NetplayState::CSS_LOCAL_READY :
                    NetplayState::CSS_REMOTE_READY);
            }
        } else {
            // Character unlocked
            ControlChannel_SendCharUnlock();

            g_css_state.locked[g_player_index] = false;

            // Reset to lobby if we were ready
            if (g_state >= NetplayState::CSS_LOCAL_READY) {
                Netplay_SetState(NetplayState::CSS_LOBBY);
                g_local_battle_ready = false;
                g_remote_battle_ready = false;
            }
        }
        g_last_local_action_state = local_action_state;
    }

    return true;  // OK to advance
}

// =============================================================================
// BATTLE PROCESSING
// =============================================================================

bool Netplay_ProcessBattle() {
    if (!g_session) return true;  // No session, let game run

    g_can_advance = false;

    // Poll network
    gekko_network_poll(g_session);

    // Capture and send local input
    uint16_t local_input = Input_CaptureLocal();
    gekko_add_local_input(g_session, g_player_index, &local_input);

    // Process session events
    int event_count = 0;
    auto events = gekko_session_events(g_session, &event_count);
    for (int i = 0; i < event_count; i++) {
        auto event = events[i];
        switch (event->type) {
            case PlayerConnected:
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Netplay: GekkoNet player %d connected (ProcessBattle)",
                            event->data.connected.handle);
                g_session_ready = true;
                break;

            case PlayerDisconnected:
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "Netplay: GekkoNet player %d disconnected",
                            event->data.disconnected.handle);
                break;

            case SessionStarted:
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Netplay: GekkoNet session started");
                g_session_ready = true;
                break;

            case PlayerSyncing: {
                unsigned char cur = event->data.syncing.current;
                unsigned char mx = event->data.syncing.max;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Netplay: GekkoNet syncing %u/%u (ProcessBattle)",
                            (unsigned)cur, (unsigned)mx);
                if (cur >= mx) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                "Netplay: Sync complete in ProcessBattle, marking ready");
                    g_session_ready = true;
                }
                break;
            }

            case DesyncDetected: {
                auto& d = event->data.desynced;
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                    "!!! DESYNC DETECTED !!! frame=%d local=0x%08X remote=0x%08X",
                    d.frame, d.local_checksum, d.remote_checksum);

                // Log current game state for diagnostics
                uint32_t rng = *(uint32_t*)FM2K::ADDR_RANDOM_SEED;
                uint32_t mode = *(uint32_t*)FM2K::ADDR_GAME_MODE;
                uint32_t p1_hp = *(uint32_t*)FM2K::ADDR_P1_HP;
                uint32_t p2_hp = *(uint32_t*)FM2K::ADDR_P2_HP;
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                    "DESYNC state: rng=0x%08X mode=%u p1_hp=%u p2_hp=%u",
                    rng, mode, p1_hp, p2_hp);

                // Mark desync and terminate
                g_desync_detected = true;
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                    "FATAL: Desync detected - terminating client!");

                // Stop session and exit
                Netplay_StopGekkoSession();
                ExitProcess(1);
                break;
            }

            default:
                break;
        }
    }

    // Process game events
    int update_count = 0;
    auto updates = gekko_update_session(g_session, &update_count);

    for (int i = 0; i < update_count; i++) {
        auto update = updates[i];
        switch (update->type) {
            case SaveEvent: {
                int frame = update->data.save.frame;

                // CRITICAL: Force RNG sync on initial frames (before game logic runs)
                // This ensures both clients start with identical state
                if (frame <= 0) {
                    *(uint32_t*)FM2K::ADDR_RANDOM_SEED = 0x12345678;
                }

                // Save full game state locally, get checksum from that
                SaveState_Save(frame);

                // Minimal state for GekkoNet (just frame + RNG for restoration)
                NetplayFrameState fs;
                fs.frame = frame;
                fs.rng_seed = *(uint32_t*)FM2K::ADDR_RANDOM_SEED;

                // Checksum from FULL saved state (not just this small struct)
                *update->data.save.state_len = sizeof(NetplayFrameState);
                *update->data.save.checksum = SaveState_GetLastChecksum(frame);
                memcpy(update->data.save.state, &fs, sizeof(NetplayFrameState));

                // Log first few saves
                static int save_log_count = 0;
                if (++save_log_count <= 5) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "SAVE #%d: frame=%d checksum=0x%08X",
                        save_log_count, frame, SaveState_GetLastChecksum(frame));
                }
                break;
            }

            case LoadEvent: {
                // Load state from GekkoNet buffer (only frame + RNG)
                NetplayFrameState fs;
                memcpy(&fs, update->data.load.state, sizeof(NetplayFrameState));

                // Load our full local state first
                SaveState_Load(fs.frame);

                // CRITICAL: Restore RNG from NETWORK state AFTER local load
                // This ensures both clients have identical RNG even if local saves diverged
                *(uint32_t*)FM2K::ADDR_RANDOM_SEED = fs.rng_seed;
                break;
            }

            case AdvanceEvent: {
                if (update->data.adv.inputs && update->data.adv.input_len >= sizeof(uint16_t) * 2) {
                    uint16_t* inputs = (uint16_t*)update->data.adv.inputs;
                    g_p1_input = inputs[0] & 0x7FF;
                    g_p2_input = inputs[1] & 0x7FF;
                }
                g_can_advance = true;
                g_netplay_frame++;
                break;
            }

            default:
                break;
        }
    }

    return g_can_advance;
}

// Helper to get current time in ms (from control_channel.cpp)
static uint32_t GetTimeMs() {
    auto now = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch());
    return static_cast<uint32_t>(ms.count());
}

// Process connection retry logic
static void ProcessConnectionRetry() {
    if (g_state != NetplayState::CONNECTING) return;

    uint32_t now = GetTimeMs();
    if (now - g_last_hello_time >= HELLO_RETRY_INTERVAL_MS) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Netplay: Retrying HELLO...");
        uint32_t game_hash = 0;  // TODO: Calculate from game files
        ControlChannel_SendHello(static_cast<uint8_t>(g_player_index), game_hash);
        g_last_hello_time = now;
    }
}

void Netplay_ProcessMenu() {
    // Keep control channel alive
    ControlChannel_Poll();

    // Retry connection if needed
    ProcessConnectionRetry();
}

// =============================================================================
// GEKKONET SESSION LIFECYCLE
// =============================================================================

bool Netplay_StartGekkoSession() {
    if (g_session) {
        // Already active
        return true;
    }

    // Send battle ready signal (only once)
    if (!g_local_battle_ready) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Netplay: Starting GekkoNet session");
        ControlChannel_SendBattleReady();
        g_local_battle_ready = true;
    }

    // Wait for remote to be ready
    if (!g_remote_battle_ready) {
        // Keep polling for their BATTLE_READY
        ControlChannel_Poll();
        return false;  // Not ready yet
    }

    // Both players ready - now initialize savestate and create session
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Netplay: Both ready, creating GekkoNet session");

    // CRITICAL: Synchronize RNG seed before battle starts
    // Both clients must start with identical RNG state
    // Use a deterministic seed based on player 0 being the "host"
    uint32_t sync_rng_seed = 0x12345678;  // Fixed seed for now - TODO: exchange via control channel
    *(uint32_t*)FM2K::ADDR_RANDOM_SEED = sync_rng_seed;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Netplay: Synchronized RNG seed to 0x%08X", sync_rng_seed);

    SaveState_Init();

    gekko_create(&g_session);

    GekkoConfig config{};
    config.num_players = 2;
    config.max_spectators = 0;
    config.input_prediction_window = 7;
    config.input_size = sizeof(uint16_t);
    config.state_size = sizeof(NetplayFrameState);
    config.desync_detection = true;
    config.limited_saving = false;

    gekko_start(g_session, &config);

    // Use our multiplexed adapter
    gekko_net_adapter_set(g_session, CreateMultiplexAdapter());

    // Add players in order
    const sockaddr_in* remote = NetSocket_GetRemoteAddr();
    char addr_str[64];
    snprintf(addr_str, sizeof(addr_str), "%s:%d",
             inet_ntoa(remote->sin_addr), ntohs(remote->sin_port));

    GekkoNetAddress addr;
    addr.data = addr_str;
    addr.size = strlen(addr_str);

    for (int i = 0; i < 2; i++) {
        if (i == g_player_index) {
            gekko_add_actor(g_session, LocalPlayer, nullptr);
            gekko_set_local_delay(g_session, i, 2);
        } else {
            gekko_add_actor(g_session, RemotePlayer, &addr);
        }
    }

    g_session_ready = false;  // Will be set true by SessionStarted event
    Netplay_SetState(NetplayState::BATTLE_INIT);

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Netplay: GekkoNet session created, waiting for sync");
    return true;
}

void Netplay_StopGekkoSession() {
    if (!g_session) return;

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Netplay: Stopping GekkoNet session");

    DestroyMultiplexAdapter();
    gekko_destroy(&g_session);
    g_session = nullptr;
    g_session_ready = false;
    g_local_battle_ready = false;
    g_remote_battle_ready = false;
}

bool Netplay_IsSessionReady() {
    return g_session_ready;
}

void Netplay_PollGekkoNet() {
    if (!g_session) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Netplay_PollGekkoNet: No session!");
        return;
    }

    // Poll network
    gekko_network_poll(g_session);

    // Process session events (to get SessionStarted)
    int event_count = 0;
    auto events = gekko_session_events(g_session, &event_count);
    for (int i = 0; i < event_count; i++) {
        auto event = events[i];
        switch (event->type) {
            case SessionStarted:
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Netplay: GekkoNet SessionStarted event!");
                g_session_ready = true;
                break;

            case PlayerConnected:
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Netplay: GekkoNet player %d connected",
                            event->data.connected.handle);
                // Player connected means session is ready for battle
                g_session_ready = true;
                break;

            case PlayerSyncing: {
                unsigned char cur = event->data.syncing.current;
                unsigned char mx = event->data.syncing.max;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Netplay: GekkoNet syncing %u/%u (cur=0x%02X, max=0x%02X, cur>=max: %s)",
                            (unsigned)cur, (unsigned)mx,
                            (unsigned)cur, (unsigned)mx,
                            (cur >= mx) ? "YES" : "NO");
                Netplay_SetState(NetplayState::BATTLE_SYNCING);
                // Check if sync complete (current == max means done)
                if (cur >= mx) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                "Netplay: GekkoNet sync complete, marking ready");
                    g_session_ready = true;
                }
                break;
            }

            case PlayerDisconnected:
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "Netplay: GekkoNet player %d disconnected",
                            event->data.disconnected.handle);
                break;

            case DesyncDetected: {
                auto& d = event->data.desynced;
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                    "!!! DESYNC DETECTED (Poll) !!! frame=%d local=0x%08X remote=0x%08X",
                    d.frame, d.local_checksum, d.remote_checksum);

                // Log current game state for diagnostics
                uint32_t rng = *(uint32_t*)FM2K::ADDR_RANDOM_SEED;
                uint32_t mode = *(uint32_t*)FM2K::ADDR_GAME_MODE;
                uint32_t p1_hp = *(uint32_t*)FM2K::ADDR_P1_HP;
                uint32_t p2_hp = *(uint32_t*)FM2K::ADDR_P2_HP;
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                    "DESYNC state: rng=0x%08X mode=%u p1_hp=%u p2_hp=%u",
                    rng, mode, p1_hp, p2_hp);

                g_desync_detected = true;
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                    "FATAL: Desync detected - terminating client!");

                Netplay_StopGekkoSession();
                ExitProcess(1);
                break;
            }

            default:
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Netplay: GekkoNet event type %d", event->type);
                break;
        }
    }
}

// =============================================================================
// INPUT
// =============================================================================

uint16_t Netplay_GetInput(int player_id) {
    return (player_id == 0) ? g_p1_input : g_p2_input;
}

uint16_t Netplay_GetCSSInput(int player_id) {
    // Return the appropriate input based on player
    if (player_id == g_player_index) {
        // Local player - return our captured input
        return g_local_css_input;
    } else {
        // Remote player - return received input
        return g_remote_css_input;
    }
}

// =============================================================================
// STATUS QUERIES
// =============================================================================

bool Netplay_IsActive() {
    return g_session != nullptr;
}

bool Netplay_IsConnected() {
    return ControlChannel_IsConnected();
}

uint32_t Netplay_GetFrame() {
    return g_netplay_frame;
}

uint32_t Netplay_GetPingMs() {
    return ControlChannel_GetRttMs();
}
