// CCCaster-Style Netplay Implementation
// - Control channel for CSS input sync using INPUT DELAY (not lockstep)
// - GekkoNet for battle mode rollback
// - Uses game's internal timer for frame counting
#include "netplay.h"
#include "control_channel.h"
#include "game_hash.h"
#include "input.h"
#include "savestate.h"
#include "replay.h"
#include "spectator_node.h"
#include "nat_traversal.h"
#include "globals.h"
#include "gekkonet.h"
#include "../audio/sound_rollback.h"
#include "../ui/shared_mem.h"  // SharedMem_PublishMatchOutcome
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
// SESSION TRACKING
// =============================================================================
//
// What kind of GekkoNet session g_session currently points at. We run two
// distinct sessions back-to-back for a single match: a CSS lockstep session
// (input_prediction_window=0, no rollback) and a battle rollback session
// (prediction_window=8 + runahead). The kind determines the per-tick driver
// and the swap-frame protocol direction. SessionKind is an alias for the
// public NetplaySessionKind enum exported from netplay.h so external callers
// (spectator_node, hub_client) can compare against the same values.

using SessionKind = NetplaySessionKind;
static SessionKind g_session_kind = SessionKind::NONE;

// CSS lockstep parameters (ported from the legacy CCCaster-style ring-buffer
// implementation). With GekkoNet's prediction=0 mode, local_delay is the
// per-player input commitment delay — peer A's input committed at frame F
// becomes the input applied at frame F+local_delay, identical semantics to
// the previous "store at frame+delay, read at frame" model.
static constexpr int CSS_LOCAL_DELAY = 6;
static constexpr int CSS_CONFIRM_LOCKOUT = 150;     // Block confirm for first N frames (moon selector workaround)

// CSS state — input transport now lives inside the CSS GekkoSession
static bool g_css_active        = false;  // Currently in CSS mode (game-side detection)
static bool g_css_synced        = false;  // Both peers BATTLE_READY, CSS GekkoSession ready
static bool g_remote_css_ready  = false;  // Remote has entered CSS
static bool g_local_css_ready   = false;  // We've entered CSS
static uint32_t g_css_frame     = 0;      // Last confirmed CSS AdvanceEvent frame (+1)

// Per-poll AdvanceEvent input cache. Netplay_ProcessCSS drives gekko_update_session,
// which fires AdvanceEvent only when both peers have inputs for the current frame
// (lockstep guarantee). The cached inputs are read by Hook_GetPlayerInput via
// Netplay_GetCSSInput.
static uint16_t g_css_advance_p1     = 0;
static uint16_t g_css_advance_p2     = 0;
static bool     g_css_advance_ready  = false;

// GekkoNet session pointer + readiness flag (shared by CSS and battle —
// only one is alive at a time, distinguished by g_session_kind).
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

// Connection-lifetime cache for the computed (auto) battle delay. Pinned
// at the FIRST battle-session start after CONNECTED and reused for every
// subsequent CSS->battle transition until disconnect. Without this we
// recomputed per-battle off whatever stale rtt_worst happened to be sitting
// in the bucket — a single CSS spike would slam delay to 15 on a 6 ms link.
// Cleared on Netplay_Shutdown / Netplay_OnDisconnect so a reconnect picks
// a fresh value next time.
static int  g_session_delay_cached       = 0;
static bool g_session_delay_cache_valid  = false;

// Highest frame number we've ever recorded into the replay/spectator stream.
// Reset on each Netplay_StartBattle (g_netplay_frame also resets to 0).
// Gates the GekkoAdvance recording path against runahead duplicates — each
// frame is advanced multiple times under runahead, but only the first
// monotonic forward crossing is "the" confirmed advance.
static uint32_t g_highest_recorded_frame = 0;

// Battle entry sync barrier - ensures both clients enter battle at same time.
// Also carries swap_frame negotiation for the CSS-session->battle-session swap:
// both peers propose g_css_frame + SWAP_FRAME_BUFFER on detection, exchange via
// BATTLE_ENTERING.data.sync.frame, and the agreed value is max(local, remote).
// The actual gekko_destroy(css)/gekko_create(battle) deferred until the active
// CSS session reaches g_battle_entry_swap_frame.
static bool     g_local_battle_entered    = false;
static bool     g_remote_battle_entered   = false;
static bool     g_battle_synced           = false;
static uint32_t g_battle_entry_swap_frame = 0;     // Latest agreed swap frame.

// Battle exit sync barrier (battle-session -> CSS-session swap, for rematch
// or return-to-menu). Mirrors the entry barrier but reads g_netplay_frame
// instead of g_css_frame and is driven by BATTLE_END instead of BATTLE_ENTERING.
static bool     g_local_battle_end_signaled  = false;
static bool     g_remote_battle_end_signaled = false;
static bool     g_battle_end_synced          = false;
static uint32_t g_battle_end_swap_frame      = 0;

// Frames of slack added to the proposed swap_frame so both peers have time
// to drain in-flight inputs and converge their proposals before reaching it.
// 8 @ 100 FPS = 80ms — comfortably above typical RTT. Tunable.
constexpr uint32_t SWAP_FRAME_BUFFER = 8;

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

// Forward decls for SOCD-mode helpers in hooks.cpp (file-scope so all
// uses below — Netplay_BroadcastHostConfig + the HOST_CONFIG receiver —
// can reach them).
extern "C" int  Hook_GetSOCDModePublic();
extern "C" void Hook_SetSOCDMode(int mode);

// Snapshot host's current settings and ship them to the remote peer +
// any subscribed spectators. Called from CheckFullyConnected (initial
// rendezvous) and from Netplay_StartBattle (every new match) so settings
// changes mid-session propagate. No-op when the local peer isn't host.
static void Netplay_BroadcastHostConfig() {
    if (g_player_index != 0) return;  // only host pushes config
    const uint32_t stage = *(uint32_t*)FM2K::ADDR_SELECTED_STAGE;
    const uint8_t  socd  = (uint8_t)Hook_GetSOCDModePublic();
    ControlChannel_SendHostConfig(
        /*selected_stage*/  stage,
        /*round_count*/     0,        // forward-compat field; address unmapped
        /*round_time_sec*/  0,        // forward-compat field; address unmapped
        /*game_speed_pct*/  0,        // forward-compat field; address unmapped
        /*socd_mode*/       socd);

    // Also push to subscribed spectators on the same multiplex channel.
    // BroadcastSwapToSubscribers is the existing helper that fans CtrlPacket
    // to every subscriber addr; we reuse the same control packet.
    {
        CtrlPacket pkt = {};
        pkt.header.type = CtrlMsg::HOST_CONFIG;
        pkt.data.host_config.selected_stage  = stage;
        pkt.data.host_config.socd_mode       = socd;
        pkt.data.host_config.round_count     = 0;
        pkt.data.host_config.round_time_sec  = 0;
        pkt.data.host_config.game_speed_pct  = 0;
        auto subs = SpectatorNode_GetSubscriberAddrs();
        for (const auto& s : subs) {
            ControlChannel_SendTo(pkt, s);
        }
    }
}

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

            // Push host's authoritative match config so client adopts the
            // same stage/SOCD/etc settings without manual mirroring.
            Netplay_BroadcastHostConfig();
        }
    }
}

static void OnControlMessage(const CtrlPacket* packet, const sockaddr_in& from) {
    switch (packet->header.type) {
        case CtrlMsg::HELLO: {
            const uint32_t local_hash = fm2k::game_hash::Compute();
            const uint32_t peer_hash  = packet->data.hello.game_hash;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Netplay: Received HELLO from player %d (peer_hash=0x%08X local_hash=0x%08X)",
                packet->data.hello.player_id,
                peer_hash, local_hash);
            // Game-data hash check (#57). 0 on either side means the
            // peer is older / we couldn't enumerate — fall through to
            // the existing handshake flow so we don't break legacy
            // clients during rollout. Both sides nonzero + different
            // = abort: write a DISCONNECT outcome so the launcher's
            // PollMatchOutcome surfaces a toast and closes the game.
            if (local_hash != 0 && peer_hash != 0 && local_hash != peer_hash) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                    "Netplay: GAME DATA MISMATCH — peer=0x%08X us=0x%08X (%s). "
                    "Aborting handshake; have both peers send each other their "
                    "FM2K_*_Debug.log file and diff the 'GameHash: manifest' "
                    "section to find which file differs.",
                    peer_hash, local_hash, fm2k::game_hash::DescribeLocal());
                // Re-dump the local manifest right next to the error so users
                // who scroll up from the bottom of the log see exactly what
                // we hashed without hunting for the original "GameHash:
                // manifest" line up at boot time. Multi-line so a peer
                // reading the log can quickly spot a different size or
                // content hash on a specific filename.
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                    "Netplay: local manifest follows (compare against peer's "
                    "log line-by-line):");
                // Iterate the cached entries vector directly. We used
                // to split the cached manifest STRING line-by-line via
                // strchr('\n'), but that path turned out to corrupt one
                // entry's render in some installs (placeholder22.player
                // showed up as "placeholder22|-", missing extension and
                // size). Going through entries gets bytes byte-equivalent
                // to the boot-time per-entry log.
                fm2k::game_hash::ForEachManifestEntry(
                    [](const char* name, uint64_t size, uint64_t content_hash,
                       void* /*user*/) {
                        if (content_hash != 0) {
                            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                                "  %s|%llu|%016llx",
                                name,
                                (unsigned long long)size,
                                (unsigned long long)content_hash);
                        } else {
                            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                                "  %s|%llu|-",
                                name,
                                (unsigned long long)size);
                        }
                    }, nullptr);
                SharedMem_PublishMatchOutcome(FM2K_MATCH_OUTCOME_HASH_MISMATCH);
                break;
            }
            ControlChannel_SendHelloAck(static_cast<uint8_t>(g_player_index));
            g_received_hello = true;
            CheckFullyConnected();
            break;
        }

        case CtrlMsg::HELLO_ACK:
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Netplay: Received HELLO_ACK");
            g_received_hello_ack = true;
            CheckFullyConnected();
            break;

        case CtrlMsg::CSS_INPUT:
            // CSS_INPUT is dead code post-redesign — CSS lockstep now lives
            // inside a GekkoGameSession with prediction_window=0, so inputs
            // flow through GekkoNet's transport. The enum value + this case
            // are kept as a no-op for backward compatibility with peers that
            // still send the old packet (they'll be silently ignored).
            break;

        case CtrlMsg::BATTLE_READY: {
            // After CSS GekkoSession is fully up (g_css_synced=true) the
            // BATTLE_READY rendezvous is over — any leftover packets are
            // network-buffered echoes from the rendezvous window and can
            // be silently dropped. Without this gate the unconditional
            // echo below would ping-pong forever between both peers,
            // logging "Sent / Received BATTLE_READY" every ~10ms for the
            // entire CSS phase.
            if (g_css_synced) {
                break;
            }
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Netplay: Received BATTLE_READY from remote");
            g_remote_css_ready = true;

            // Loss-tolerant echo — same pattern as BATTLE_ENTERING /
            // BATTLE_END. When peers return to CSS at slightly
            // different wall-clock times (one finishes battle-end-sync
            // ~300 ms before the other), the ahead peer creates its CSS
            // GekkoSession on the first BATTLE_READY it sees, then
            // STOPS sending its own BATTLE_READY. The lagging peer's
            // BATTLE_READYs after that point arrive here and need an
            // echo back, otherwise the lagging peer never sees our
            // signal and stays stuck resending forever. Bounded by the
            // !g_css_synced gate above — echo only happens during the
            // rendezvous window, terminates when sync completes.
            if (g_local_css_ready) {
                ControlChannel_SendBattleReady();
            }
            break;
        }

        case CtrlMsg::BATTLE_ENTERING: {
            const uint32_t remote_proposal = packet->data.sync.frame;
            // Spectator-side handling: this is host telling us about the
            // upcoming CSS->battle swap. Flip our SpectateSession to battle
            // config. (Spectators don't participate in proposal convergence —
            // they passively follow whatever the host announces.)
            if (g_session_kind == SessionKind::SPECTATE) {
                Netplay_OnHostBattleEntering(remote_proposal);
                break;
            }
            // Player-side handling: convergence on max(local, remote) swap.
            const uint32_t prev_agreed = g_battle_entry_swap_frame;
            if (remote_proposal > g_battle_entry_swap_frame) {
                g_battle_entry_swap_frame = remote_proposal;
            }
            g_remote_battle_entered = true;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Netplay: Received BATTLE_ENTERING (remote_swap=%u, prev_agreed=%u, agreed=%u)",
                remote_proposal, prev_agreed, g_battle_entry_swap_frame);

            // Echo our own BATTLE_ENTERING back if we've already signaled
            // locally — needed for the lossy-network case where remote
            // received our signal but their echo to us was dropped.
            // Without rate-limiting, both peers echo every echo from the
            // other and we get an infinite ping-pong storm (observed
            // hundreds of sends in a single millisecond). 100ms cap is
            // far below the swap-frame transition window so packet-loss
            // recovery still works, but the storm can't run away.
            if (g_local_battle_entered) {
                static uint32_t last_echo_ms = 0;
                const uint32_t now_ms = GetTickCount();
                if (now_ms - last_echo_ms >= 100) {
                    ControlChannel_SendBattleEntering(g_battle_entry_swap_frame);
                    last_echo_ms = now_ms;
                }
            }
            break;
        }

        case CtrlMsg::BATTLE_END: {
            const uint32_t remote_proposal = packet->data.sync.frame;
            if (g_session_kind == SessionKind::SPECTATE) {
                Netplay_OnHostBattleEnd(remote_proposal);
                break;
            }
            const uint32_t prev_agreed = g_battle_end_swap_frame;
            if (remote_proposal > g_battle_end_swap_frame) {
                g_battle_end_swap_frame = remote_proposal;
            }
            g_remote_battle_end_signaled = true;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Netplay: Received BATTLE_END (remote_swap=%u, prev_agreed=%u, agreed=%u)",
                remote_proposal, prev_agreed, g_battle_end_swap_frame);

            // Same rate-limited echo as BATTLE_ENTERING.
            if (g_local_battle_end_signaled) {
                static uint32_t last_echo_ms = 0;
                const uint32_t now_ms = GetTickCount();
                if (now_ms - last_echo_ms >= 100) {
                    ControlChannel_SendBattleEnd(g_battle_end_swap_frame);
                    last_echo_ms = now_ms;
                }
            }
            break;
        }

        case CtrlMsg::DISCONNECT:
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Netplay: Remote disconnected");
            ControlChannel_SetConnected(false);
            g_simple_state = SimpleState::DISCONNECTED;
            // Drop the pinned auto-delay so the next connection measures
            // fresh (peer might be on a different network now).
            g_session_delay_cache_valid = false;
            g_session_delay_cached      = 0;
            break;

        case CtrlMsg::HOST_CONFIG: {
            // Host's authoritative match settings — adopt locally so this
            // peer (client OR spectator) runs with identical rules.
            // Per-field "unset" sentinels: 0xFFFFFFFF for selected_stage,
            // 0 for the count/time/speed fields, 0xFF for socd_mode.
            const auto& hc = packet->data.host_config;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Netplay: Received HOST_CONFIG (stage=%u rounds=%u time=%u speed=%u socd=%u)",
                hc.selected_stage, hc.round_count, hc.round_time_sec,
                hc.game_speed_pct, (unsigned)hc.socd_mode);

            // Stage selection — direct memcpy to FM2K's selected-stage
            // global (FM2K::ADDR_SELECTED_STAGE; IDA-verified in WW as
            // 0x43010c, the var that vs_round_function reads when
            // calling LoadStageFile(wParam)). The previous addr
            // 0x470188 had no xrefs and writes were silently ignored.
            if (hc.selected_stage != 0xFFFFFFFF) {
                *(uint32_t*)FM2K::ADDR_SELECTED_STAGE = hc.selected_stage;
            }

            // SOCD mode — wire through the runtime setter. Persists for
            // the rest of the session unless host changes it again.
            if (hc.socd_mode != 0xFF) {
                Hook_SetSOCDMode((int)hc.socd_mode);
            }

            // round_count / round_time_sec / game_speed_pct: forward-compat
            // fields. Per-game addresses aren't mapped yet (would need IDA
            // session per FM2K variant). Document on the wire for v1.1.
            break;
        }

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
            SpectatorNode_HandleJoinAck(from,
                                        packet->data.spec_join_ack.host_session_kind,
                                        packet->data.spec_join_ack.host_tcp_port);
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

    // Reset CSS state — input transport now lives in the CSS GekkoSession,
    // so the legacy ring-buffer fields are gone. The session itself is
    // created on first BATTLE_READY rendezvous and torn down on battle entry.
    g_session_kind      = SessionKind::NONE;
    g_css_frame         = 0;
    g_css_advance_p1    = 0;
    g_css_advance_p2    = 0;
    g_css_advance_ready = false;
    g_css_active = false;
    g_css_synced = false;
    g_remote_css_ready = false;
    g_local_css_ready = false;

    // Reset battle sync state (entry direction)
    g_local_battle_entered    = false;
    g_remote_battle_entered   = false;
    g_battle_synced           = false;
    g_battle_entry_swap_frame = 0;

    // Reset battle sync state (exit direction, for next return-to-CSS)
    g_local_battle_end_signaled  = false;
    g_remote_battle_end_signaled = false;
    g_battle_end_synced          = false;
    g_battle_end_swap_frame      = 0;

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

    // Initialize spectator tree node AFTER NetSocket — its TCP listener
    // binds to the UDP socket's port number (TCP/UDP share port space).
    SpectatorNode_Init();

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

    // Send HELLO with our local game-data fingerprint (#57). Receiver
    // compares against its own and aborts the handshake on mismatch.
    ControlChannel_SendHello(static_cast<uint8_t>(player_index),
                             fm2k::game_hash::Compute());

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
    // the host. INPUT_BATCH frames flow over a TCP connection opened on
    // JOIN_ACK; UDP carries only handshake + heartbeat.
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
    g_session_delay_cache_valid = false;
    g_session_delay_cached      = 0;
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
// CSS PROCESSING — GekkoNet lockstep (input_prediction_window = 0)
//
// CSS used to ride a custom CCCaster-style ring buffer over CtrlMsg::CSS_INPUT.
// It's now a GekkoGameSession with prediction_window=0, which gives true
// lockstep: gekko_update_session emits AdvanceEvent only when both peers'
// inputs for the current frame have arrived. Save/Load events are suppressed
// in lockstep mode (game_session.cpp:226-228, 365-367, 537), so CSS state is
// derived purely from the shared seed (0x12345678 reseeded on sync) +
// identical confirmed inputs — same determinism as today, but riding a
// well-tested transport.
// =============================================================================

// Forward decl — defined after Netplay_StartBattle (shares the actor-add
// pattern). Returns true if the CSS session is ready to drive.
static bool Netplay_StartCSSSession();
static void Netplay_EndCSSSession();

// Iterate currently-subscribed spectators (from SpectatorNode's address list)
// and call gekko_add_actor(GekkoSpectator, &addr) for each on the active
// g_session. Called after the player actors are added in StartCSS/StartBattle.
// Late joiners (after session creation) are added directly in
// SpectatorNode_HandleJoinReq.
static void AddSubscribedSpectatorsToSession();

void Netplay_PollCSS() {
    ControlChannel_Poll();
    if (g_session && g_session_kind == SessionKind::CSS) {
        gekko_network_poll(g_session);
    }
}

bool Netplay_CanAdvanceCSS() {
    // Not synced yet — let game run freely (pre-CSS or waiting for remote)
    if (!g_css_synced) {
        return true;
    }
    // Once the CSS session is up, advance is gated on the AdvanceEvent
    // having fired during the most recent Netplay_ProcessCSS call.
    return g_css_advance_ready;
}

bool Netplay_ProcessCSS() {
    // Poll for incoming control-channel messages (BATTLE_READY rendezvous,
    // BATTLE_ENTERING, etc.) — independent of GekkoNet's transport.
    ControlChannel_Poll();

    // Not connected yet — let game run with local input
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

    // Keep resending BATTLE_READY until remote acknowledges. Without this,
    // if P1 syncs first and sends only one BATTLE_READY that gets lost,
    // P2 never syncs and gets stuck forever.
    static uint32_t last_ready_send = 0;
    if (!g_remote_css_ready && now - last_ready_send > 100) {
        ControlChannel_SendBattleReady();
        last_ready_send = now;
    }

    // Wait for both clients to be in CSS before bringing up the GekkoNet
    // CSS session. Pre-rendezvous frames run unsynchronized (identical to
    // today's pre-g_css_synced behavior).
    if (!g_remote_css_ready) {
        return true;  // Let game run but don't drive the session yet
    }

    // First frame after rendezvous: reseed RNG and stand up the CSS session.
    if (!g_css_synced) {
        // CRITICAL: Re-seed RNG now that both clients are synced. Pre-CSS
        // frames ran unsynchronized and diverged the RNG. Stage selection
        // uses RNG during CSS->battle transition, so it MUST be identical
        // from this point forward.
        *(uint32_t*)FM2K::ADDR_RANDOM_SEED = 0x12345678;

        if (!Netplay_StartCSSSession()) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                "CSS: Netplay_StartCSSSession failed");
            return false;
        }
        g_css_synced = true;
        g_css_frame  = 0;

        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "CSS SYNCED: Both ready, GekkoNet CSS session up, RNG reseeded");
    }

    // Drive the GekkoNet CSS session for this tick.
    g_css_advance_ready = false;
    if (!g_session || g_session_kind != SessionKind::CSS) {
        // Session torn down (e.g., we just swapped to battle); nothing to do.
        return true;
    }

    gekko_network_poll(g_session);

    // Submit local input. With prediction=0, this commits at frame
    // local_frame + CSS_LOCAL_DELAY; AdvanceEvent fires later for the
    // committed frame once the remote's input for that frame arrives.
    uint16_t local_raw = Input_CaptureLocal();
    gekko_add_local_input(g_session, g_player_index, &local_raw);

    // Drain session events (Connected/Syncing/Disconnected/Desync).
    int event_count = 0;
    auto events = gekko_session_events(g_session, &event_count);
    for (int i = 0; i < event_count; i++) {
        auto event = events[i];
        switch (event->type) {
            case GekkoSessionStarted:
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "CSS: GekkoNet CSS session started");
                g_session_ready = true;
                break;
            case GekkoPlayerConnected:
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "CSS: GekkoNet player %d connected", event->data.connected.handle);
                g_session_ready = true;
                break;
            case GekkoPlayerSyncing:
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "CSS: GekkoNet syncing %u/%u",
                    event->data.syncing.current, event->data.syncing.max);
                break;
            case GekkoPlayerDisconnected:
                // Peer's CSS-phase Gekko session went silent past
                // DISCONNECT_TIMEOUT. Publish CSS_ABORT (NOT DISCONNECT)
                // so the launcher closes the surviving local game but
                // doesn't record this in W/L/D — battle never started,
                // there's no result to commit. DISCONNECT outcome is
                // reserved for "peer dropped during battle", which IS
                // a forfeit and counts.
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "CSS: peer disconnected (handle=%d) — publishing CSS_ABORT outcome",
                    event->data.disconnected.handle);
                SharedMem_PublishMatchOutcome(FM2K_MATCH_OUTCOME_CSS_ABORT);
                break;
            case GekkoDesyncDetected:
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                    "CSS DESYNC f=%d local=0x%08X remote=0x%08X",
                    event->data.desynced.frame,
                    event->data.desynced.local_checksum,
                    event->data.desynced.remote_checksum);
                break;
            default:
                break;
        }
    }

    // Drain update events. With prediction=0 + limited_saving=false, only
    // AdvanceEvent fires (lockstep mode skips Save/Load — see
    // game_session.cpp:226 / :365 / :537).
    int update_count = 0;
    auto updates = gekko_update_session(g_session, &update_count);
    for (int i = 0; i < update_count; i++) {
        auto update = updates[i];
        if (update->type != GekkoAdvanceEvent) {
            continue;  // Save/Load shouldn't fire under lockstep, but ignore if they do.
        }
        // Inputs are packed in slot order (p1 at index 0, p2 at index 1).
        const uint16_t* in = (const uint16_t*)update->data.adv.inputs;
        g_css_advance_p1    = in[0];
        g_css_advance_p2    = in[1];
        g_css_advance_ready = true;
        g_css_frame         = (uint32_t)update->data.adv.frame + 1;

        // session_history recording moved to Hook_GetPlayerInput where
        // the actual returned input values pass through. That captures
        // pre-rendezvous title-screen / auto-mash inputs too, which this
        // post-AdvanceEvent point misses (no AdvanceEvents fire pre-
        // rendezvous). One canonical log spanning FM2K boot to disconnect.

        if ((g_css_frame - 1) % 100 == 0) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "CSS: advance frame=%d p1=0x%04X p2=0x%04X",
                update->data.adv.frame, g_css_advance_p1, g_css_advance_p2);
        }
    }

    // No AdvanceEvent this tick → lockstep is waiting on remote → stall.
    return g_css_advance_ready;
}

uint16_t Netplay_GetCSSInput(int player_id) {
    uint16_t input;
    if (player_id == 0) {
        input = g_css_advance_p1;
    } else {
        input = g_css_advance_p2;
    }

    // CCCaster-style: block confirm/cancel for the first CSS_CONFIRM_LOCKOUT
    // frames (moon selector workaround). g_css_frame is one past the last
    // confirmed AdvanceEvent, so the "current" read frame is g_css_frame - 1.
    const uint32_t read_frame = (g_css_frame > 0) ? g_css_frame - 1 : 0;
    if (read_frame < (uint32_t)CSS_CONFIRM_LOCKOUT) {
        input &= 0x0FF;  // Mask button presses, keep direction bits.
    }

    return input;
}

// =============================================================================
// BATTLE ENTRY SYNC BARRIER
// Ensures both clients enter battle mode at the same time
// =============================================================================

// Helper: send a BATTLE_ENTERING / BATTLE_END payload to every currently
// subscribed spectator so they can mirror the swap. Called alongside the
// usual unicast-to-remote-peer send.
static void BroadcastSwapToSubscribers(CtrlMsg type, uint32_t swap_frame) {
    auto subs = SpectatorNode_GetSubscriberAddrs();
    for (const auto& addr : subs) {
        CtrlPacket pkt = {};
        pkt.header.type     = type;
        pkt.data.sync.frame = swap_frame;
        ControlChannel_SendTo(pkt, addr);
    }
}

void Netplay_SignalBattleEntry() {
    if (g_local_battle_entered) {
        return;  // Already signaled
    }

    // Compute our proposed swap frame on the active CSS session. Remote may
    // already have proposed a higher value (we adopt it via the receive
    // handler); take max so we never go backwards.
    const uint32_t local_proposal = g_css_frame + SWAP_FRAME_BUFFER;
    if (local_proposal > g_battle_entry_swap_frame) {
        g_battle_entry_swap_frame = local_proposal;
    }
    g_local_battle_entered = true;
    ControlChannel_SendBattleEntering(g_battle_entry_swap_frame);
    BroadcastSwapToSubscribers(CtrlMsg::BATTLE_ENTERING, g_battle_entry_swap_frame);

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "BATTLE SYNC: Local entered battle mode (css_frame=%u, swap_frame=%u)",
        g_css_frame, g_battle_entry_swap_frame);
}

bool Netplay_IsBattleSynced() {
    // Once the game's CSS detects the battle transition (game_mode -> 3000),
    // the trampoline phase classifier flips us into TRAMPOLINE_BATTLE and we
    // stop driving Netplay_ProcessCSS — so g_css_frame stops advancing. The
    // swap_frame value (g_css_frame + SWAP_FRAME_BUFFER) is therefore
    // unreachable from the CSS session itself. Lockstep already guarantees
    // both peers detect the transition at the same logical frame (the same
    // shared CSS input stream produced the same selected character + lock-in
    // events), so the agreed-on-both-sides BATTLE_ENTERING is enough — no
    // need to also gate on css_frame parity. swap_frame stays in the message
    // for diagnostic logging and future battle-side gating.
    if (!g_battle_synced &&
        g_local_battle_entered &&
        g_remote_battle_entered) {
        g_battle_synced = true;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "BATTLE SYNC: both peers signaled (css_frame=%u, swap_frame=%u) - swap CSS->battle now",
            g_css_frame, g_battle_entry_swap_frame);
    }
    return g_battle_synced;
}

uint32_t Netplay_GetBattleEntrySwapFrame() {
    return g_battle_entry_swap_frame;
}

void Netplay_PollBattleSync() {
    // Poll control channel to receive BATTLE_ENTERING from remote
    ControlChannel_Poll();

    // Resend BATTLE_ENTERING until remote acknowledges, carrying the latest
    // agreed swap_frame each time. If remote's proposal arrived higher than
    // ours, the agreed value bumped — keep both sides in sync via resend.
    static uint32_t last_send = 0;
    uint32_t now = GetTickCount();
    if (g_local_battle_entered && !g_remote_battle_entered && now - last_send > 50) {
        ControlChannel_SendBattleEntering(g_battle_entry_swap_frame);
        last_send = now;
    }
}

// =============================================================================
// BATTLE EXIT SYNC BARRIER
// Mirrors the entry barrier; gates battle->CSS swap on agreed swap_frame.
// =============================================================================

void Netplay_SignalBattleEnd() {
    if (g_local_battle_end_signaled) {
        return;
    }

    const uint32_t local_proposal = g_netplay_frame + SWAP_FRAME_BUFFER;
    if (local_proposal > g_battle_end_swap_frame) {
        g_battle_end_swap_frame = local_proposal;
    }
    g_local_battle_end_signaled = true;
    ControlChannel_SendBattleEnd(g_battle_end_swap_frame);
    BroadcastSwapToSubscribers(CtrlMsg::BATTLE_END, g_battle_end_swap_frame);

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "BATTLE END SYNC: Local left battle mode (battle_frame=%u, swap_frame=%u)",
        g_netplay_frame, g_battle_end_swap_frame);
}

bool Netplay_IsBattleEndSynced() {
    // Same chicken-and-egg as the entry direction: once game_mode leaves
    // the [3000,4000) battle range, the phase classifier flips to CSS and
    // RunBattleTick stops driving the battle session — so g_netplay_frame
    // stops, the swap_frame target is unreachable. Both peers' confirmed
    // exit detection happens at the same logical battle frame (deterministic
    // from shared inputs), so the both-signaled gate is sufficient.
    if (!g_battle_end_synced &&
        g_local_battle_end_signaled &&
        g_remote_battle_end_signaled) {
        g_battle_end_synced = true;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "BATTLE END SYNC: both peers signaled (battle_frame=%u, swap_frame=%u) - swap battle->CSS now",
            g_netplay_frame, g_battle_end_swap_frame);
    }
    return g_battle_end_synced;
}

uint32_t Netplay_GetBattleEndSwapFrame() {
    return g_battle_end_swap_frame;
}

void Netplay_PollBattleEndSync() {
    ControlChannel_Poll();

    static uint32_t last_send = 0;
    uint32_t now = GetTickCount();
    if (g_local_battle_end_signaled && !g_remote_battle_end_signaled && now - last_send > 50) {
        ControlChannel_SendBattleEnd(g_battle_end_swap_frame);
        last_send = now;
    }
}

static void AddSubscribedSpectatorsToSession() {
    // Spectators are NOT GekkoSpectator actors. Input distribution to
    // spectators flows over the SpectatorNode INPUT_BATCH path — every
    // confirmed (p1, p2) frame is recorded into session_history at
    // Hook_GetPlayerInput's capture_and_return and the host's
    // FlushBatch broadcasts to every subscriber.
    //
    // Adding spectators to GekkoNet was the wrong architecture — it required
    // host/spectator sub-state to match at session-create time, which is
    // launch-timing dependent and a snapshot transfer to fix. Pure input
    // replay sidesteps all of that: spectator boots → starts consuming
    // host's recorded inputs from frame 0 → walks title→CSS→battle in
    // lockstep with host's recorded execution.
    (void)0;  // intentionally empty — kept as a hook for future per-session
              // setup if needed.
}

// =============================================================================
// GEKKONET SESSION - CSS Lockstep (input_prediction_window = 0)
// =============================================================================

static bool Netplay_StartCSSSession() {
    if (g_session) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "Netplay_StartCSSSession: session already exists (kind=%d)",
            (int)g_session_kind);
        return g_session_kind == SessionKind::CSS;
    }

    // Compute CSS delay dynamically from current RTT instead of pinning
    // at the conservative CSS_LOCAL_DELAY=6. With prediction=0 lockstep,
    // delay too low for the actual link makes CSS visibly choppy: every
    // frame stalls waiting for peer input. Same formula the battle path
    // uses: ceil(mean_one_way_ms / 10ms), floored at 2, capped at 15.
    // RTT samples come from the existing PING / HELLO ack cycle so this
    // is meaningful by the time we create the CSS session (post-HELLO_ACK).
    int css_delay = CSS_LOCAL_DELAY;  // fallback
    {
        const uint32_t rtt_mean_ms  = ControlChannel_GetRttMs();
        if (rtt_mean_ms > 0) {
            const uint32_t mean_one_way = rtt_mean_ms / 2;
            int d = (int)((mean_one_way + 9) / 10);
            if (d < 2)  d = 2;
            if (d > 15) d = 15;
            css_delay = d;
        }
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "Netplay: Creating CSS GekkoSession (lockstep, prediction=0, delay=%d)",
        css_delay);

    GekkoConfig config = {};
    config.num_players              = 2;
    // Allow up to 4 spectators per session; spectator_delay sized for full
    // CSS catch-up. CSS sessions are short (a few hundred frames at most
    // before battle entry), so default is plenty.
    config.max_spectators           = 4;
    config.spectator_delay          = 0;    // see battle-session comment — disables pause-buffer
    // input_history_size: host keeps every confirmed CSS input frame in
    // _net_spectator_queue, capped at this many. Late-joining spectators
    // (last_acked_frame == NULL_FRAME) get the entire history streamed
    // on connect. 60000 frames = 10 min @ 100 FPS — plenty for a CSS
    // lobby session that ran for an unusually long pre-match wait.
    // See vendored/GekkoNet patch + README.md:36.
    config.input_history_size       = 60000;
    config.input_prediction_window  = 0;    // lockstep — IsLockstepActive() in game_session.cpp:520
    config.input_size               = sizeof(uint16_t);
    config.state_size               = sizeof(uint32_t);
    config.desync_detection         = true;
    config.limited_saving           = false;  // No effect in lockstep — Save events suppressed

    gekko_create(&g_session, GekkoGameSession);
    gekko_start(g_session, &config);

    auto adapter = CreateMultiplexAdapter();
    gekko_net_adapter_set(g_session, adapter);

    // Refresh remote address string from learned sockaddr (post-HELLO_ACK).
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
            gekko_set_local_delay(g_session, i, css_delay);
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "CSS: Added local player at slot %d (delay=%d)", i, css_delay);
        } else {
            GekkoNetAddress addr = {};
            addr.data = (void*)g_remote_addr;
            addr.size = (int)strlen(g_remote_addr);
            gekko_add_actor(g_session, GekkoRemotePlayer, &addr);
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "CSS: Added remote player at slot %d -> %s", i, g_remote_addr);
        }
    }

    // No runahead in lockstep mode (suppressed by IsLockstepActive at
    // game_session.cpp:537 even if requested).

    // Set kind BEFORE the spectator add — AddSubscribedSpectatorsToSession
    // re-broadcasts SPEC_JOIN_ACK carrying g_session_kind, which spectators
    // use to swap their SpectateSession config to match.
    g_session_kind      = SessionKind::CSS;
    g_session_ready     = false;
    g_css_advance_ready = false;
    g_css_advance_p1    = 0;
    g_css_advance_p2    = 0;
    g_css_frame         = 0;
    g_local_delay       = css_delay;

    AddSubscribedSpectatorsToSession();

    return true;
}

static void Netplay_EndCSSSession() {
    if (g_session && g_session_kind == SessionKind::CSS) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Netplay: Destroying CSS GekkoSession");
        gekko_destroy(&g_session);
        g_session       = nullptr;
        g_session_kind  = SessionKind::NONE;
        g_session_ready = false;
    }
    g_css_advance_ready = false;
}

// =============================================================================
// GEKKONET SESSION - Spectate (passive observer of a remote host)
// =============================================================================

// Build the GekkoConfig for a spectator session, mirroring the host's config
// for the given phase. Spectators only need session_type, input_size,
// state_size, and spectator_delay — num_players + prediction_window are
// informational but kept consistent with host for clarity.
static GekkoConfig MakeSpectateConfig(SessionKind host_kind) {
    GekkoConfig config = {};
    config.num_players      = 2;
    config.max_spectators   = 0;     // Spectator doesn't accept further spectators via gekko
    config.input_size       = sizeof(uint16_t);
    config.state_size       = sizeof(uint32_t);
    config.desync_detection = true;
    config.limited_saving   = false;
    // spectator_delay = 0: disable GekkoNet's spectator pause-buffer.
    // With > 0, SpectatorSession::ShouldDelaySpectator gates AdvanceEvent
    // emission until min_received - current >= delay frames, which can
    // never converge on low-latency connections (current advances each
    // tick once unpaused; min only advances on incoming packets, so the
    // diff stays ~stable). 0 == always advance as fast as inputs arrive.
    config.spectator_delay     = 0;
    // Late-joiner backfill: receive buffer holds up to ~10 minutes of
    // confirmed inputs (60000 frames @ 100 FPS, 4B each = ~240 KB). Lets
    // the spectator's local sim FF through the host's full session
    // history on connect. See vendored/GekkoNet patch for README.md:36.
    config.input_history_size  = 60000;
    if (host_kind == SessionKind::BATTLE) {
        config.input_prediction_window = 8;
    } else {
        // CSS or anything else → lockstep config
        config.input_prediction_window = 0;
    }
    return config;
}

bool Netplay_IsSpectatorSession() {
    return g_session_kind == SessionKind::SPECTATE;
}

NetplaySessionKind Netplay_GetSessionKind() {
    return g_session_kind;
}

GekkoSession* Netplay_GetActiveSession() {
    return g_session;
}

bool Netplay_StartSpectateSession(NetplaySessionKind host_kind, const char* host_addr) {
    // Idempotent: if a SpectateSession is already alive against the same
    // host with the same kind, don't tear it down. SpectatorNode's silence-
    // failover currently fires on legacy 0xCE quiet (input streaming dead)
    // and re-runs JOIN_REQ → JOIN_ACK → here. Without this guard we'd
    // destroy a healthy session every 5 seconds and re-handshake.
    if (g_session && g_session_kind == SessionKind::SPECTATE &&
        host_addr && host_addr[0] && std::strcmp(g_remote_addr, host_addr) == 0) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "Netplay_StartSpectateSession: session already alive for %s — keeping",
            host_addr);
        return true;
    }
    if (g_session) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "Netplay_StartSpectateSession: session already exists (kind=%d) — destroying first",
            (int)g_session_kind);
        gekko_destroy(&g_session);
        g_session = nullptr;
        g_session_kind = SessionKind::NONE;
    }

    if (!host_addr || !host_addr[0]) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "Netplay_StartSpectateSession: empty host_addr");
        return false;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "Netplay: Creating SpectateSession (host_kind=%d, host=%s)",
        (int)host_kind, host_addr);

    GekkoConfig config = MakeSpectateConfig(host_kind);

    gekko_create(&g_session, GekkoSpectateSession);
    gekko_start(g_session, &config);
    g_session_kind = SessionKind::SPECTATE;

    auto adapter = CreateMultiplexAdapter();
    gekko_net_adapter_set(g_session, adapter);

    // Add host as the (single) remote actor we receive input forwards from.
    GekkoNetAddress addr = {};
    addr.data = (void*)host_addr;
    addr.size = (int)strlen(host_addr);
    gekko_add_actor(g_session, GekkoRemotePlayer, &addr);

    g_session_ready     = false;
    g_p1_input          = 0;
    g_p2_input          = 0;
    g_netplay_frame     = 0;
    g_css_advance_ready = false;

    return true;
}

void Netplay_EndSpectateSession() {
    if (g_session && g_session_kind == SessionKind::SPECTATE) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Netplay: Destroying SpectateSession");
        gekko_destroy(&g_session);
        g_session       = nullptr;
        g_session_kind  = SessionKind::NONE;
        g_session_ready = false;
    }
}

// Spectator-side phase-flip handlers. Called from the BATTLE_ENTERING /
// BATTLE_END control-channel handlers when g_session_kind == SPECTATE.
// The spectator is passive: it observes the host's announced swap_frame
// and executes destroy/create at that frame.
//
// NOTE: For now we don't gate on swap_frame on the spectator side — the host's
// session destroy at swap_frame causes magic-mismatch silence to land at the
// same logical point on the wire, and the spectator's session sync re-handshake
// with the new session naturally aligns. swap_frame gating on the spectator
// side is a refinement; keep simple for first cut.
// Spectator-side pending swap: when the host announces a phase change,
// the spectator may still be FFing through backfilled inputs to catch up
// to live. Recording the target swap_frame lets ProcessSpectatorPhase
// finish draining AdvanceEvents up to that frame BEFORE tearing the
// session down — otherwise we'd lose the tail of CSS / battle inputs.
// Drained per-tick from Netplay_ProcessSpectatorPhase.
//
// pending_kind == NONE means no swap pending. Set by On* handlers, cleared
// when the swap actually fires (or on EndSpectateSession reset).
static NetplaySessionKind g_pending_swap_kind  = NetplaySessionKind::NONE;
static uint32_t           g_pending_swap_frame = 0;

void Netplay_OnHostBattleEntering(uint32_t swap_frame) {
    if (g_session_kind != SessionKind::SPECTATE) return;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "Spectator: host entering battle (swap_frame=%u) — pending CSS->battle swap",
        swap_frame);
    g_pending_swap_kind  = SessionKind::BATTLE;
    g_pending_swap_frame = swap_frame;
}

void Netplay_OnHostBattleEnd(uint32_t swap_frame) {
    if (g_session_kind != SessionKind::SPECTATE) return;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "Spectator: host ending battle (swap_frame=%u) — pending battle->CSS swap",
        swap_frame);
    g_pending_swap_kind  = SessionKind::CSS;
    g_pending_swap_frame = swap_frame;
}

// Called from Netplay_ProcessSpectatorPhase after each AdvanceEvent.
// Returns true if a swap fired this tick (caller should stop draining
// further events from the now-destroyed session).
static bool MaybeSwapPendingSpectator(uint32_t advanced_frame) {
    if (g_pending_swap_kind == SessionKind::NONE)        return false;
    if (g_session_kind     != SessionKind::SPECTATE)     return false;
    if (advanced_frame      < g_pending_swap_frame)      return false;

    const NetplaySessionKind next_kind = g_pending_swap_kind;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "Spectator: caught up to swap_frame=%u — swapping to %s SpectateSession",
        g_pending_swap_frame,
        next_kind == SessionKind::BATTLE ? "battle" : "CSS");

    char host_addr[64];
    snprintf(host_addr, sizeof(host_addr), "%s", g_remote_addr);
    Netplay_EndSpectateSession();
    Netplay_StartSpectateSession(next_kind, host_addr);

    g_pending_swap_kind  = SessionKind::NONE;
    g_pending_swap_frame = 0;
    return true;
}

// =============================================================================
// GEKKONET SESSION - Battle Mode (rollback)
// =============================================================================

bool Netplay_StartBattle() {
    // Tear down the CSS lockstep session before standing up the battle
    // session. Sequenced so g_session is null between the two — the
    // multiplex adapter is shared (session_magic demuxes), but only one
    // session is alive at a time.
    if (g_session && g_session_kind == SessionKind::CSS) {
        Netplay_EndCSSSession();
    }
    if (g_session) {
        return true;  // Already in some other session (battle or stress).
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Netplay: Starting battle GekkoSession");

    // Capture per-match character IDs + resolved .player filenames
    // while the CSS-side state is still live. Held in shared mem so
    // the launcher can include them in the hub `match_result` payload
    // after Netplay_EndBattle wipes CSS. Addresses come from
    // FM2K_Integration.h (launcher-side header) — inlined here because
    // the hook DLL has its own minimal FM2K:: namespace in globals.h
    // that doesn't pull in the full Integration header.
    //
    // Roster table at 0x435474 (`g_char_slot_data`): array of 256-byte
    // CP932 (Shift-JIS) filename strings, indexed by the same char_id
    // that lives at P1/P2_CHARACTER_ID_ADDR. Convert CP932 → UTF-8
    // via Win32 before publishing so JP names survive round-tripping
    // through the JSON hub protocol intact.
    {
        // Engine-aware via globals.h: FM2K reads CCCaster-style address pair
        // at 0x470180/4 with the 258-slot g_char_slot_data table at 0x435474;
        // FM95 reads its per-player CSS struct at 0x432720/0x432730 with the
        // 50-slot g_player_file_name_array at 0x463CF0.
        constexpr uintptr_t kP1CharIdAddr  = FM2K::ADDR_P1_SELECTED_CHAR;
        constexpr uintptr_t kP2CharIdAddr  = FM2K::ADDR_P2_SELECTED_CHAR;
        constexpr uintptr_t kCharSlotData  = FM2K::ADDR_CHAR_FILENAME_TABLE;
        constexpr size_t    kSlotStride    = FM2K::CHAR_FILENAME_STRIDE;
        constexpr size_t    kSlotCount     = FM2K::CHAR_FILENAME_COUNT;

        const uint32_t p1_char = *(const uint32_t*)kP1CharIdAddr;
        const uint32_t p2_char = *(const uint32_t*)kP2CharIdAddr;

        auto resolve_name = [&](uint32_t id, char* out, size_t out_cap) {
            out[0] = '\0';
            if (id >= kSlotCount) return;
            const char* cp932 =
                reinterpret_cast<const char*>(kCharSlotData + id * kSlotStride);
            // Bound the read at the slot stride so a missing NUL never
            // walks off into the next slot's data.
            const int len_932 = (int)strnlen(cp932, kSlotStride);
            if (len_932 == 0) return;

            wchar_t wbuf[kSlotStride];
            int wlen = MultiByteToWideChar(932, 0, cp932, len_932,
                                           wbuf, (int)(sizeof(wbuf) / sizeof(wbuf[0])));
            if (wlen <= 0) return;
            int u8len = WideCharToMultiByte(CP_UTF8, 0, wbuf, wlen,
                                            out, (int)(out_cap - 1),
                                            nullptr, nullptr);
            if (u8len <= 0) { out[0] = '\0'; return; }
            out[u8len] = '\0';
        };

        char p1_name[FM2K_MATCH_CHAR_NAME_MAX] = {};
        char p2_name[FM2K_MATCH_CHAR_NAME_MAX] = {};
        resolve_name(p1_char, p1_name, sizeof(p1_name));
        resolve_name(p2_char, p2_name, sizeof(p2_name));

        SharedMem_PublishMatchChars(p1_char, p2_char, p1_name, p2_name);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "Netplay: match chars p1=%u(\"%s\") p2=%u(\"%s\")",
            p1_char, p1_name, p2_char, p2_name);
    }

    // Random stage (#56 — Lilithport-style seeded xorshift). The
    // launcher hands us a host-generated seed via FM2K_STAGE_RANDOM_SEED
    // when both peers agree on random stage. We re-seed once per
    // process from that env var (g_xorshift_seeded), then advance one
    // step per Netplay_StartBattle and write the resulting index to
    // FM2K's stage memory. Both peers run the same xorshift sequence
    // from the same seed, so rematches keep rolling identically with
    // zero per-rematch wire traffic.
    {
        constexpr uintptr_t kSelectedStageAddr = FM2K::ADDR_SELECTED_STAGE;
        static bool      g_xorshift_seeded = false;
        static uint32_t  g_xs_a = 1812433254u, g_xs_b = 3713160357u,
                         g_xs_c = 3109174145u, g_xs_d = 64984499u;
        static int       g_stage_min = 0;
        static int       g_stage_max = -1;   // -1 = random disabled

        if (!g_xorshift_seeded) {
            g_xorshift_seeded = true;
            const char* seed_env = std::getenv("FM2K_STAGE_RANDOM_SEED");
            const char* min_env  = std::getenv("FM2K_STAGE_RANDOM_MIN");
            const char* max_env  = std::getenv("FM2K_STAGE_RANDOM_MAX");
            // One-shot diagnostic: if the env vars aren't visible to
            // the hook process, random-stage silently does nothing
            // (g_stage_max stays -1) and every match plays on whatever
            // stage_id the CSS cursor happens to be on. Logging both
            // presence + value lets us tell apart "host disabled" vs
            // "env didn't propagate to game process".
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Netplay: random-stage env: SEED=%s MIN=%s MAX=%s",
                seed_env ? seed_env : "(unset)",
                min_env  ? min_env  : "(unset)",
                max_env  ? max_env  : "(unset)");
            if (seed_env && *seed_env) {
                uint32_t seed = (uint32_t)std::strtoul(seed_env, nullptr, 10);
                if (seed != 0) {
                    // Lilith's seeding scheme — same constants so a
                    // shared seed produces the same a[4] state on both
                    // peers (cross-implementation parity if anyone ever
                    // wants Lilith-FM2K interop, though we don't ship
                    // that today).
                    uint32_t s = seed;
                    uint32_t* arr[4] = {&g_xs_a, &g_xs_b, &g_xs_c, &g_xs_d};
                    for (int i = 0; i < 4; ++i) {
                        s = 1812433253u * (s ^ (s >> 30)) + (uint32_t)i;
                        *arr[i] = s;
                    }
                    g_stage_min = (min_env && *min_env)
                        ? std::atoi(min_env) : 0;
                    g_stage_max = (max_env && *max_env)
                        ? std::atoi(max_env) : -1;
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Netplay: random-stage seeded (seed=%u min=%d max=%d)",
                        (unsigned)seed, g_stage_min, g_stage_max);
                }
            }
        }

        if (g_stage_max >= g_stage_min && g_stage_max >= 0) {
            // Advance one step. xorshift128 — identical to Lilith's
            // RandomStage() arrival-no-args branch (stdafx.cpp:670).
            uint32_t t  = g_xs_a ^ (g_xs_a << 11);
            g_xs_a = g_xs_b; g_xs_b = g_xs_c; g_xs_c = g_xs_d;
            g_xs_d = (g_xs_d ^ (g_xs_d >> 19)) ^ (t ^ (t >> 8));
            const uint32_t span = (uint32_t)(g_stage_max - g_stage_min + 1);
            const uint32_t roll = g_xs_d % span;
            const uint32_t stage = (uint32_t)g_stage_min + roll;
            *(uint32_t*)kSelectedStageAddr = stage;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Netplay: random stage rolled=%u (range %d..%d)",
                stage, g_stage_min, g_stage_max);
        }

        // Stage_id capture for the hub match_result payload. Done AFTER
        // the random-stage roll so we publish the post-roll value, not
        // the stale CSS pre-roll one. FM95 has no documented selected-
        // stage scalar yet (ADDR_SELECTED_STAGE == 0); publish unknown.
        if constexpr (FM2K::ADDR_SELECTED_STAGE != 0) {
            const uint32_t stage_id =
                *(const uint32_t*)FM2K::ADDR_SELECTED_STAGE;
            SharedMem_PublishMatchStage(stage_id);
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Netplay: match stage_id=%u", stage_id);
        } else {
            SharedMem_PublishMatchStage(0xFFFFFFFFu);
        }
    }

    // Reset CSS state when entering battle
    g_css_active        = false;
    g_css_synced        = false;
    g_local_css_ready   = false;
    g_remote_css_ready  = false;
    g_css_advance_ready = false;

    // Per-player local delay. Computed ONCE per connection at the first
    // battle-session start, then cached and reused across every CSS->
    // battle transition until disconnect.
    //
    // Formula uses MEAN one-way latency (not worst). CCCaster's
    // `latency.getWorst() + 1` works for them because they run a tight
    // 30-ping burst right before session creation; we opportunistically
    // accumulate samples over the entire CSS period, so a single
    // outlier (Discord notification spike, Windows scheduler hiccup)
    // would inflate "worst" and lock in a too-high delay for the rest
    // of the match. Mean is robust to that and gives matched delays
    // between peers on stable links.
    //
    // FM2K runs at 100 Hz, so 10 ms is one frame budget.
    //   local_delay = max(2, ceil(mean_one_way_ms / 10))
    // Floor of 2 keeps GekkoNet's prediction window happy on
    // sub-millisecond loopback.
    //
    // FM2K_LOCAL_DELAY env var set to 1..15 forces a manual value (UI
    // override). Manual override is checked every battle so the user
    // can flip the override mid-session; AUTO is pinned for the
    // connection lifetime.
    int  local_delay = 0;
    enum DelaySource { DS_MANUAL, DS_COMPUTED, DS_CACHED } delay_source = DS_COMPUTED;
    if (const char* env = std::getenv("FM2K_LOCAL_DELAY"); env && env[0]) {
        int v = std::atoi(env);
        if (v >= 1 && v <= 15) { local_delay = v; delay_source = DS_MANUAL; }
    }
    const uint32_t rtt_mean_ms  = ControlChannel_GetRttMs();
    const uint32_t rtt_worst_ms = ControlChannel_GetWorstRttMs();
    const uint32_t mean_one_way = rtt_mean_ms / 2;
    if (local_delay == 0) {
        if (g_session_delay_cache_valid) {
            local_delay  = g_session_delay_cached;
            delay_source = DS_CACHED;
        } else {
            // First battle of this connection: compute, pin, log.
            // ceil(mean_one_way / 10), then max(2, ...).
            local_delay = (int)((mean_one_way + 9) / 10);
            if (local_delay < 2)  local_delay = 2;
            if (local_delay > 15) local_delay = 15;
            g_session_delay_cached      = local_delay;
            g_session_delay_cache_valid = true;
            delay_source = DS_COMPUTED;
            // Worst-RTT bucket reset on first compute. We don't read it
            // for the formula anymore but TickHealth still updates it,
            // and a fresh window helps the diagnostic log line stay
            // tied to the upcoming match window.
            ControlChannel_ResetWorstRttMs();
        }
    }

    int prediction_window = 8;

    const char* source_str =
        delay_source == DS_MANUAL   ? "manual" :
        delay_source == DS_CACHED   ? "auto (cached)" :
                                      "auto (computed)";
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "Netplay: RTT mean=%ums worst=%ums one_way_mean=%ums -> "
        "local_delay=%d (%s), prediction_window=%d",
        rtt_mean_ms, rtt_worst_ms, mean_one_way, local_delay,
        source_str, prediction_window);

    GekkoConfig config = {};
    config.num_players = 2;
    // Up to 4 spectators per battle session. spectator_delay = 0 disables
    // GekkoNet's spectator pause-buffer (spectator_session.cpp:216-218
    // short-circuits ShouldDelaySpectator). With > 0 the spectator's
    // local sim stays paused until min_received - current >= delay, but
    // current also advances each tick post-unpause — for low-latency
    // LAN/localhost setups the buffer never converges and spectator stays
    // paused forever. Re-enable jitter buffering once we have measured
    // cross-internet ping data.
    config.max_spectators           = 4;
    config.spectator_delay          = 0;
    // Late-joiner backfill — see CSS-session comment + README.md:36.
    config.input_history_size       = 60000;
    config.input_prediction_window  = prediction_window;
    config.input_size = sizeof(uint16_t);
    config.state_size = sizeof(uint32_t);
    config.desync_detection = true;
    config.limited_saving = false;

    gekko_create(&g_session, GekkoGameSession);
    gekko_start(g_session, &config);
    g_session_kind = SessionKind::BATTLE;

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

    AddSubscribedSpectatorsToSession();

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

    // Re-broadcast host config so any setting changes (host clicked a
    // different stage between matches, switched SOCD mode, etc.) propagate
    // to client + spectators before this match's first sim frame.
    Netplay_BroadcastHostConfig();

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
    g_session_kind = SessionKind::STRESS;

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
    // Capture match outcome BEFORE we destroy the session — reading HP
    // at this point reflects the final state of the just-ended battle.
    // Outcome is from the local player's perspective; the launcher
    // forwards it as a `match_result` to the hub, which correlates
    // both peers' reports for stats. Only meaningful for actual
    // GekkoGameSessions (player vs player); spectate sessions and
    // stress runs skip the publish.
    if (g_session && g_session_kind == SessionKind::BATTLE) {
        const uint32_t p1_hp = *(uint32_t*)0x4DFC85;
        const uint32_t p2_hp = *(uint32_t*)0x4EDCC4;
        FM2KMatchOutcome outcome = FM2K_MATCH_OUTCOME_NONE;
        if (p1_hp == 0 && p2_hp == 0)              outcome = FM2K_MATCH_OUTCOME_DRAW;
        else if (p1_hp > 0 && p2_hp == 0)          outcome = (g_player_index == 0)
                                                            ? FM2K_MATCH_OUTCOME_SELF_WON
                                                            : FM2K_MATCH_OUTCOME_PEER_WON;
        else if (p2_hp > 0 && p1_hp == 0)          outcome = (g_player_index == 1)
                                                            ? FM2K_MATCH_OUTCOME_SELF_WON
                                                            : FM2K_MATCH_OUTCOME_PEER_WON;
        // Both HPs > 0: timeout / non-KO end (rematch trigger, return-to-
        // CSS without final round). We can't tell who won — log as draw.
        else                                        outcome = FM2K_MATCH_OUTCOME_DRAW;

        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "Netplay: match outcome p1_hp=%u p2_hp=%u outcome=%d",
            p1_hp, p2_hp, (int)outcome);
        SharedMem_PublishMatchOutcome(outcome);
    }

    if (g_session) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "Netplay: Ending GekkoNet session (kind=%d)", (int)g_session_kind);
        gekko_destroy(&g_session);
        g_session       = nullptr;
        g_session_kind  = SessionKind::NONE;
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

    // Reset battle sync state for next battle (entry direction)
    g_local_battle_entered    = false;
    g_remote_battle_entered   = false;
    g_battle_synced           = false;
    g_battle_entry_swap_frame = 0;

    // Reset battle-end sync state — fresh for the rematch's next return.
    g_local_battle_end_signaled  = false;
    g_remote_battle_end_signaled = false;
    g_battle_end_synced          = false;
    g_battle_end_swap_frame      = 0;
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

            case GekkoPlayerDisconnected: {
                // Peer dropped (timeout / closed game / network died). Publish
                // a DISCONNECT outcome so the launcher's shared-mem poll
                // forwards a match_result to the hub AND tears down the
                // surviving local game. Fires on CSS too — without this the
                // survivor froze on the character-select screen with music
                // playing when their opponent closed during CSS (real bug
                // report 2026-05-05).
                if (g_session_kind == SessionKind::BATTLE ||
                    g_session_kind == SessionKind::CSS) {
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Netplay: peer disconnected (handle=%d kind=%d) — publishing DISCONNECT outcome",
                        event->data.disconnected.handle,
                        (int)g_session_kind);
                    SharedMem_PublishMatchOutcome(FM2K_MATCH_OUTCOME_DISCONNECT);
                }
                break;
            }

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

                // Snapshot RNG BEFORE PGI/UG run. Used by the [HOST-TRACE] log
                // below — distinguishes "per-frame leak inside PGI/UG" from
                // "inter-frame leak between confirmed advances."
                const uint32_t rng_pre_advance = *(uint32_t*)0x41FB1C;

                // Speculative-advance carve-out for palette flash state.
                //
                // Why: original_update_game is the game's update_game_state @
                // 0x404CD0 — a 7-instruction stub that does nothing but
                // decrement two palette flash timers (g_timer_countdown1 @
                // 0x4456E4 and g_timer_countdown2 @ 0x447D91). PGI also runs
                // character_state_machine which may re-fire [EB] and write
                // timer = duration during a script's trigger anim slot.
                //
                // Under runahead = 4, every wall-clock frame fires 1 confirmed
                // + 4 speculative AdvanceEvents. Each speculative advance also
                // hits update_game_state, decrementing palette timers. Net:
                // palette drains 5x per wall-clock frame instead of 1x — a
                // 43-frame palette flash plays for ~9 wall-clock frames and
                // then "cuts out", which is the bug user reported.
                //
                // Fix: snapshot the palette flash structs before PGI runs,
                // and after UG returns, restore them IFF this advance is
                // speculative (running_ahead). Confirmed advances drain the
                // timer naturally; speculative advances are a no-op on
                // palette state. End result: timer drains 1/wall-clock-frame
                // matching vanilla.
                //
                // Why not the same fix for shake? Shake is decremented in
                // RENDER (ProcessShakeEffect), not sim. Render runs at most
                // once per wall-clock frame regardless of advance count, so
                // shake already drains correctly. Verified empirically — the
                // same diag log shows shake decrementing 1/frame while
                // palette decremented 5/frame.
                constexpr uintptr_t kEffectSys1Addr = 0x447D7D;
                constexpr size_t    kEffectSys1Size = 42;
                constexpr uintptr_t kPflash2Addr    = 0x4456D0;
                constexpr size_t    kPflash2Size    = 44;
                struct PaletteSnapshot {
                    uint8_t  effect_sys1[kEffectSys1Size];   // 0x447D7D, 42 B
                    uint8_t  pflash2[kPflash2Size];          // 0x4456D0, palette-flash-2 struct
                    uint32_t rng_seed;                       // 0x41FB1C
                };
                PaletteSnapshot pal_pre{};
                const bool running_ahead = update->data.adv.running_ahead;
                if (running_ahead) {
                    std::memcpy(pal_pre.effect_sys1,
                                (void*)kEffectSys1Addr,
                                kEffectSys1Size);
                    std::memcpy(pal_pre.pflash2, (void*)kPflash2Addr, kPflash2Size);
                    // Snapshot RNG too. Same reason: under runahead, PGI
                    // (character_state_machine and friends) calls game_rand()
                    // multiple times per advance. With 1 confirmed + 4
                    // speculative advances per wall-clock frame, sim RNG
                    // progresses ~5x faster than vanilla, which means
                    // ProcessColorInterpolation mode 3 (the random palette
                    // flash mode used by Bewear 214B) reads a different
                    // seed than vanilla would — same draw-rate, same
                    // duration, but different colors per frame. Restoring
                    // RNG after speculative advances makes sim RNG progress
                    // 1/wall-clock-frame matching vanilla, so palette mode 3
                    // (and any other render-side RNG consumer) sees the
                    // same seed at render time as offline 2DFM_Player.
                    pal_pre.rng_seed = *(uint32_t*)0x41FB1C;
                }

                // Run a FULL game tick for EVERY AdvanceEvent (matching GekkoNet examples).
                // The game loop must NOT run its own tick - we handle everything here.
                if (original_process_game_inputs) {
                    original_process_game_inputs();
                }
                if (original_update_game) {
                    original_update_game();
                }

                if (running_ahead) {
                    std::memcpy((void*)kEffectSys1Addr,
                                pal_pre.effect_sys1,
                                kEffectSys1Size);
                    std::memcpy((void*)kPflash2Addr, pal_pre.pflash2, kPflash2Size);
                    *(uint32_t*)0x41FB1C = pal_pre.rng_seed;
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

                    // PER-FRAME alignment-trace log: every confirmed frame
                    // for the first 100 frames of each battle. Pairs with the
                    // identical [SPEC-TRACE] log on spectator. Comparing
                    // (rng_pre, rng_post, p1_in, p2_in) at every battle frame
                    // tells us:
                    //   * inputs match every frame? → alignment correct
                    //   * inputs differ at some bf? → alignment off / wrong
                    //                                 input popped that frame
                    //   * rng_pre matches but rng_post differs → PGI+UG itself
                    //                                            calls game_rand
                    //                                            different #
                    //   * rng_pre differs → leak between confirmed advances
                    //                       (something between this frame and
                    //                       last frame mutated RNG)
                    // Long trace: 5000 confirmed frames (~50 sec), so we
                    // capture the wall-clock window where the user reports
                    // visible desync. "bf-1" subtraction aligns this with
                    // spec's bf=K labelling (spec starts at 0, host's
                    // post-increment makes its first log bf=1).
                    if (g_netplay_frame > 0 && g_netplay_frame <= 5000) {
                        constexpr uintptr_t POOL = 0x4701E0;
                        constexpr size_t    SLOT = 382;
                        const int32_t p1_x = *(int32_t*)(POOL + 0 * SLOT + 0x08);
                        const int32_t p2_x = *(int32_t*)(POOL + 1 * SLOT + 0x08);
                        const int32_t p1_script = *(int32_t*)(POOL + 0 * SLOT + 0x30);
                        const int32_t p2_script = *(int32_t*)(POOL + 1 * SLOT + 0x30);
                        const uint32_t p1_hp = *(uint32_t*)0x4DFC85;
                        const uint32_t p2_hp = *(uint32_t*)0x4EDCC4;
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "[HOST-TRACE] bf=%u rng_pre=0x%08X rng_post=0x%08X "
                            "p1=0x%03X p2=0x%03X p1_x=%d p2_x=%d "
                            "p1_s=%d p2_s=%d hp=%u/%u",
                            g_netplay_frame - 1u, rng_pre_advance,
                            *(uint32_t*)0x41FB1C,
                            g_p1_input, g_p2_input,
                            p1_x, p2_x, p1_script, p2_script,
                            p1_hp, p2_hp);
                    }

                    Replay::Replay_RecordFrame(g_p1_input, g_p2_input);
                    // Spectator input forwarding: gate is here because this
                    // site is already !rolling_back && !running_ahead &&
                    // new_frame — the dedup we need to avoid runahead /
                    // rollback re-recording the same frame. Pre-battle
                    // frames (title screen, CSS) are captured by
                    // Hook_GetPlayerInput's capture_and_return, which is
                    // correct for those phases (no rollback there).
                    SpectatorNode_OnFrameConfirmed(g_p1_input, g_p2_input);

                    // Per-frame state fingerprint for spectator-desync
                    // diagnosis — pairs with [SPEC-FP] log in
                    // RunSpectatorTick. Same sample addresses; spectator's
                    // bf counter is its own pop count post-battle-entry,
                    // host's bf is g_netplay_frame. Match by bf to find
                    // first divergent frame.
                    if ((g_netplay_frame % 30) == 0) {
                        const uint32_t rng     = *(uint32_t*)0x41FB1C;
                        const uint32_t buf_idx = *(uint32_t*)0x447EE0;
                        const uint32_t p1_hp   = *(uint32_t*)0x4DFC85;
                        const uint32_t p2_hp   = *(uint32_t*)0x4EDCC4;
                        const uint32_t timer   = *(uint32_t*)0x470044;
                        // World positions live in the object pool: slot 0 +0x08
                        // = pos_x, slot 1 = slot 0 + 382. (0x470020 was the
                        // CSS character-slot index, not the in-battle x —
                        // earlier logs were comparing useless data.)
                        constexpr uintptr_t POOL = 0x4701E0;
                        constexpr size_t    SLOT = 382;
                        const int32_t p1_x = *(int32_t*)(POOL + 0 * SLOT + 0x08);
                        const int32_t p1_y = *(int32_t*)(POOL + 0 * SLOT + 0x0C);
                        const int32_t p2_x = *(int32_t*)(POOL + 1 * SLOT + 0x08);
                        const int32_t p2_y = *(int32_t*)(POOL + 1 * SLOT + 0x0C);
                        const int32_t p1_script = *(int32_t*)(POOL + 0 * SLOT + 0x30);
                        const int32_t p2_script = *(int32_t*)(POOL + 1 * SLOT + 0x30);
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "[HOST-FP] bf=%u rng=0x%08X buf=%u "
                            "p1_hp=%u p2_hp=%u timer=%u "
                            "p1_pos=(%d,%d) p2_pos=(%d,%d) "
                            "p1_script=%d p2_script=%d "
                            "p1_in=0x%03X p2_in=0x%03X",
                            g_netplay_frame, rng, buf_idx,
                            p1_hp, p2_hp, timer,
                            p1_x, p1_y, p2_x, p2_y,
                            p1_script, p2_script,
                            g_p1_input, g_p2_input);
                    }
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

// Spectator-side per-tick driver. Mirrors Netplay_ProcessBattleInputPhase
// minus the gekko_add_local_input call (spectators have no local input).
// Save/Load events fire only when host's session is in BATTLE config
// (rollback-capable); CSS spectate sessions are pure forward AdvanceEvent.
//
// Returns true if the sim advanced this tick (caller renders); false on
// stall (waiting for confirmed inputs from host).

bool Netplay_ProcessSpectatorPhase() {
    if (!g_session || g_session_kind != SessionKind::SPECTATE) return false;

    gekko_network_poll(g_session);

    // No local input add — spectator is passive.

    // Drain session events.
    int event_count = 0;
    auto events = gekko_session_events(g_session, &event_count);
    for (int i = 0; i < event_count; i++) {
        auto event = events[i];
        switch (event->type) {
            case GekkoSessionStarted:
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Spectator: GekkoSpectateSession started");
                g_session_ready = true;
                break;
            case GekkoPlayerConnected:
                g_session_ready = true;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Spectator: handle %d connected",
                    event->data.connected.handle);
                break;
            case GekkoPlayerSyncing:
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Spectator: syncing %u/%u",
                    event->data.syncing.current, event->data.syncing.max);
                break;
            case GekkoPlayerDisconnected:
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Spectator: host disconnected (handle=%d)",
                    event->data.disconnected.handle);
                break;
            case GekkoSpectatorPaused:
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Spectator: paused — buffering");
                break;
            case GekkoSpectatorUnpaused:
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Spectator: unpaused — playback resumed");
                break;
            case GekkoDesyncDetected:
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                    "Spectator: DESYNC f=%d local=0x%08X remote=0x%08X",
                    event->data.desynced.frame,
                    event->data.desynced.local_checksum,
                    event->data.desynced.remote_checksum);
                break;
            default:
                break;
        }
    }

    // Drain Save/Load/Advance events — same handlers as the host's battle
    // path, ensuring the spectator's render-side mutations and virtual
    // clock stay locked to the host's confirmed state.
    int update_count = 0;
    auto updates = gekko_update_session(g_session, &update_count);

    bool advanced = false;
    for (int i = 0; i < update_count; i++) {
        auto update = updates[i];
        switch (update->type) {
            case GekkoSaveEvent: {
                int frame = update->data.save.frame;
                SaveState_Save(frame);
                (void)SaveState_GetLastChecksum(frame);
                uint32_t checksum = SaveState_GetRegionChecksums().gameplay_fingerprint;
                *update->data.save.state_len = sizeof(uint32_t);
                *update->data.save.checksum  = checksum;
                memcpy(update->data.save.state, &frame, sizeof(uint32_t));
                break;
            }
            case GekkoLoadEvent: {
                int frame = update->data.load.frame;
                SaveState_Load(frame);
                break;
            }
            case GekkoAdvanceEvent: {
                const uint16_t* in = (const uint16_t*)update->data.adv.inputs;
                g_p1_input = in[0];
                g_p2_input = in[1];
                g_netplay_frame = (uint32_t)update->data.adv.frame;
                // Lock virtual clock to host's frame schedule — same contract
                // as the host's GekkoAdvance handler. This is what closes H3
                // (g_virtual_time_ms skew vs host's rollback-rewinds).
                extern uint32_t g_virtual_time_ms;
                g_virtual_time_ms = g_netplay_frame * 10;

                if (original_process_game_inputs) original_process_game_inputs();
                if (original_update_game)         original_update_game();
                // ParityRecorder::Capture() runs from the trampoline post-tick
                // (mirrors the offline + battle paths) — keeps the parity
                // header dependency out of netplay.cpp.
                advanced = true;

                // Pending CSS<->battle phase swap: now that the local sim
                // has caught up to the host's announced swap_frame, tear
                // down this session and bring up the next-kind one. Stop
                // draining further events from the now-destroyed session.
                if (MaybeSwapPendingSpectator(g_netplay_frame)) {
                    return advanced;
                }
                break;
            }
            default:
                break;
        }
    }

    return advanced;
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
    // Tear down the CSS GekkoSession if it's still alive — happens when this
    // is called outside the normal battle-entry path (e.g. peer disconnect).
    if (g_session && g_session_kind == SessionKind::CSS) {
        Netplay_EndCSSSession();
    }
    g_css_frame         = 0;
    g_css_advance_p1    = 0;
    g_css_advance_p2    = 0;
    g_css_advance_ready = false;
    g_css_active        = false;
    g_css_synced        = false;
    g_remote_css_ready  = false;
    g_local_css_ready   = false;
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
