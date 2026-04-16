// CCCaster-Style Netplay Implementation
// - Control channel for CSS input sync using INPUT DELAY (not lockstep)
// - GekkoNet for battle mode rollback
// - Uses game's internal timer for frame counting
#include "netplay.h"
#include "control_channel.h"
#include "input.h"
#include "savestate.h"
#include "globals.h"
#include "gekkonet.h"
#include <SDL3/SDL_log.h>
#include <windows.h>
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

static void OnControlMessage(const CtrlPacket* packet) {
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

        case CtrlMsg::BATTLE_READY:
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Netplay: Received BATTLE_READY from remote");
            g_remote_css_ready = true;
            break;

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

    // Send HELLO
    ControlChannel_SendHello(static_cast<uint8_t>(player_index), 0);

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Netplay: Initialized, connecting...");
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

    // Resend until remote is ready
    static uint32_t last_ready_send = 0;
    if (!g_remote_css_ready && now - last_ready_send > 100) {
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

    // Capture input, store at frame + DELAY, send to remote
    uint16_t local_raw = Input_CaptureLocal();
    uint32_t store_frame = g_css_frame + CSS_INPUT_DELAY;
    g_local_inputs.Set(store_frame, local_raw);
    ControlChannel_SendCSSInput(local_raw, store_frame);

    // Set read frame for GetCSSInput, then advance
    g_css_current_read_frame = g_css_frame;
    g_css_frame++;

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

    GekkoConfig config = {};
    config.num_players = 2;
    config.max_spectators = 0;
    config.input_prediction_window = 8;
    config.input_size = sizeof(uint16_t);
    config.state_size = sizeof(uint32_t);
    config.desync_detection = true;
    config.limited_saving = false;

    gekko_create(&g_session, GekkoGameSession);
    gekko_start(g_session, &config);

    auto adapter = CreateMultiplexAdapter();
    gekko_net_adapter_set(g_session, adapter);

    for (int i = 0; i < 2; i++) {
        if (i == g_player_index) {
            gekko_add_actor(g_session, GekkoLocalPlayer, nullptr);
            gekko_set_local_delay(g_session, i, 1);
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Netplay: Added local player at slot %d", i);
        } else {
            GekkoNetAddress addr = {};
            addr.data = (void*)g_remote_addr;
            addr.size = (int)strlen(g_remote_addr);
            gekko_add_actor(g_session, GekkoRemotePlayer, &addr);
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Netplay: Added remote player at slot %d -> %s", i, g_remote_addr);
        }
    }

    *(uint32_t*)FM2K::ADDR_RANDOM_SEED = 0x12345678;
    SaveState_Init();

    g_simple_state = SimpleState::BATTLE;
    g_session_ready = false;
    g_netplay_frame = 0;
    g_rollback_count = 0;
    g_last_rollback_frame = 0;
    g_desync_count = 0;
    g_last_desync_log_tick = 0;

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Netplay: GekkoNet session created");
    return true;
}

void Netplay_EndBattle() {
    if (g_session) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Netplay: Ending GekkoNet session");
        gekko_destroy(&g_session);
        g_session = nullptr;
    }

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

// Called at the END of each frame to delay until target frame time
// FM2K runs at 100 FPS = 10ms per frame = 10,000,000 ns
static void HandleFrameTime(float frames_ahead) {
    if (!g_frame_timer_initialized) {
        QueryPerformanceFrequency(&g_perf_freq);
        QueryPerformanceCounter(&g_frame_start);
        g_frame_timer_initialized = true;
        return;
    }

    constexpr uint64_t BASE_FRAME_NS = 10000000;  // 10ms = 100fps

    LARGE_INTEGER frame_end;
    QueryPerformanceCounter(&frame_end);
    uint64_t elapsed_ns = ((frame_end.QuadPart - g_frame_start.QuadPart) * 1000000000ULL) / g_perf_freq.QuadPart;

    // Scale throttle proportionally to advantage:
    // 0.5-1.0 ahead: +1.6% (standard GekkoNet)
    // 1.0-2.0 ahead: +5%
    // 2.0+ ahead:    +10%
    // This converges the gap in ~1-2 seconds instead of 15+
    uint64_t target_ns = BASE_FRAME_NS;
    if (frames_ahead > 2.0f) {
        target_ns = (uint64_t)(BASE_FRAME_NS * 1.10);
    } else if (frames_ahead > 1.0f) {
        target_ns = (uint64_t)(BASE_FRAME_NS * 1.05);
    } else if (frames_ahead > 0.5f) {
        target_ns = (uint64_t)(BASE_FRAME_NS * 1.016);
    }

    if (target_ns > elapsed_ns) {
        uint64_t sleep_ns = target_ns - elapsed_ns;
        // Use Sleep for the ms portion, busy-wait for sub-ms precision
        if (sleep_ns > 2000000) {  // > 2ms
            Sleep((DWORD)((sleep_ns - 1000000) / 1000000));  // Sleep ms portion
        }
        // Busy-wait for remainder
        do {
            QueryPerformanceCounter(&frame_end);
            elapsed_ns = ((frame_end.QuadPart - g_frame_start.QuadPart) * 1000000000ULL) / g_perf_freq.QuadPart;
        } while (elapsed_ns < target_ns);
    }

    // Reset frame start for next frame
    QueryPerformanceCounter(&g_frame_start);
}

bool Netplay_ProcessBattleInputPhase() {
    if (!g_session) return true;

    gekko_network_poll(g_session);

    uint16_t local_input = Input_CaptureLocal();
    gekko_add_local_input(g_session, g_player_index, &local_input);

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
    for (int i = 0; i < update_count; i++) {
        auto update = updates[i];
        switch (update->type) {
            case GekkoSaveEvent: {
                int frame = update->data.save.frame;
                SaveState_Save(frame);
                uint32_t checksum = SaveState_GetLastChecksum(frame);
                *update->data.save.state_len = sizeof(uint32_t);
                *update->data.save.checksum = checksum;
                memcpy(update->data.save.state, &frame, sizeof(uint32_t));
                break;
            }

            case GekkoLoadEvent: {
                int frame = update->data.load.frame;
                g_rollback_count++;
                g_last_rollback_frame = g_netplay_frame;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "ROLLBACK #%u: loading frame %d (current=%u, rewinding %u frames)",
                    g_rollback_count, frame, g_netplay_frame,
                    g_netplay_frame - (uint32_t)frame);
                SaveState_Load(frame);
                break;
            }

            case GekkoAdvanceEvent: {
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

                // Periodic status every 500 frames (~5 sec)
                if (g_netplay_frame % 500 == 0) {
                    float fa = g_session ? gekko_frames_ahead(g_session) : 0.0f;
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "BATTLE STATUS: frame=%u rb=%u desync=%u ahead=%.1f frame_time_ms=%u skip=%u",
                        g_netplay_frame, g_rollback_count, g_desync_count, fa,
                        *(uint32_t*)0x41E2F0,    // g_frame_time_ms (should be 10)
                        *(uint32_t*)0x4246F4);   // g_frame_skip_count
                }

                // Dense state logging around desync boundary (frames 5000-5500)
                // Also log every 1000 frames for baseline comparison
                if (g_netplay_frame % 1000 == 0 ||
                    (g_netplay_frame >= 5000 && g_netplay_frame <= 5500 && g_netplay_frame % 50 == 0)) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "STATE f=%u: rng=0x%08X game_timer=%u round_timer_ctr=%u render_fc=%u",
                        g_netplay_frame,
                        *(uint32_t*)0x41FB1C,   // RNG seed
                        *(uint32_t*)0x470044,   // g_game_timer
                        *(uint32_t*)0x47008E,   // g_round_timer_counter
                        *(uint32_t*)0x4456FC);  // g_render_frame_counter
                }
                break;
            }

            default:
                break;
        }
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
    // TODO: Calculate from control channel RTT
    return 0;
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
