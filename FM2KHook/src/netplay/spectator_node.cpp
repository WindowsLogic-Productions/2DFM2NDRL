#include "spectator_node.h"
#include "control_channel.h"
#include "replay.h"
#include "netplay_state.h"

#include <SDL3/SDL_log.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <vector>
#include <cstring>
#include <cstdlib>
#include <cstdio>

// =============================================================================
// MODULE STATE
// =============================================================================

namespace {

// Broadcast cadence: batch INPUT_BATCH every N confirmed frames. Tunable.
// 8 means ~12.5 batches/sec at 100 FPS — ~40 B/batch → ~500 B/s per subscriber.
constexpr size_t BROADCAST_BATCH_FRAMES = 8;

// Subscriber entry from the host's perspective.
struct Subscriber {
    sockaddr_in addr;
    uint64_t    last_seen_ms;
    uint32_t    ack_frame;    // Last frame we know this subscriber has. TODO: fill on SPEC_ACK.
};

// Cached initial-match metadata so new joiners get a consistent handoff.
struct InitialMatch {
    bool     valid;
    uint8_t  header_bytes[96];  // Serialized ReplayHeader
};

// Pending batch: frames accumulated since last broadcast.
struct PendingBatch {
    uint32_t                 start_frame;
    std::vector<uint16_t>    p1_inputs;
    std::vector<uint16_t>    p2_inputs;
};

// Viewer-side playback queue entry — one confirmed frame from upstream.
struct PlaybackFrame {
    uint16_t p1_input;
    uint16_t p2_input;
};

struct State {
    bool                      broadcasting      = false;
    size_t                    capacity          = SPECTATOR_DEFAULT_CAPACITY;
    std::vector<Subscriber>   subscribers;
    InitialMatch              initial_match     = {};
    PendingBatch              pending;

    // Session input history. Every confirmed (p1, p2) pair the host streams
    // is also appended here, so a late joiner can be fast-forwarded from
    // session start. Memory: 4 bytes/frame at 100 Hz = 24 KB/min, ~1.4 MB
    // for an hour-long set. No cap for now; revisit if pathological.
    std::vector<PlaybackFrame> session_history;
    uint32_t                  session_start_frame = 0;  // frame number of session_history[0]

    // Viewer-side state (this node subscribed upstream).
    bool                      subscribed_upstream = false;
    bool                      join_req_pending    = false;  // We sent SPEC_JOIN_REQ; expect ACK.
    sockaddr_in               upstream_addr       = {};
    // Failover support. root_addr is the originally-configured upstream
    // (the actual match host). If our current upstream goes silent we
    // fall back to root, which always-on by design. last_upstream_recv_ms
    // tracks any inbound traffic; >SILENCE_THRESHOLD_MS triggers failover.
    sockaddr_in               root_addr           = {};
    uint64_t                  last_upstream_recv_ms = 0;
    uint64_t                  last_heartbeat_send_ms = 0;
    uint64_t                  last_reconnect_attempt_ms = 0;
    // De-dup gate. Backfill from a reconnected upstream replays history
    // from frame 0; frames at or below this counter were already consumed
    // locally and would re-render the past. Updated by PopFrameInputs.
    uint32_t                  highest_consumed_frame = 0;
    // Frame-counter offset so highest_consumed_frame is comparable to
    // the start_frame field on incoming INPUT_BATCH datagrams. Set on the
    // first batch received after JOIN_ACK; subsequent batches increment.
    bool                      have_frame_baseline = false;
    uint32_t                  next_expected_frame = 0;

    // Viewer-side playback driver state.
    // Populated by HandleSpecData (INITIAL_MATCH / INPUT_BATCH / MATCH_END);
    // consumed by the trampoline's RunSpectatorTick + Hook_GetPlayerInput.
    bool                      playing_back        = false;
    uint32_t                  pb_initial_seed     = 0;
    uint32_t                  pb_initial_state    = 0;  // fingerprint sanity
    uint8_t                   pb_p1_char          = 0;
    uint8_t                   pb_p1_color         = 0;
    uint8_t                   pb_p2_char          = 0;
    uint8_t                   pb_p2_color         = 0;
    std::vector<PlaybackFrame> pb_queue;
    uint16_t                  pb_current_p1       = 0;
    uint16_t                  pb_current_p2       = 0;
};

State g_state;

// =============================================================================
// UDP HELPERS
// =============================================================================

bool AddrEqual(const sockaddr_in& a, const sockaddr_in& b) {
    return a.sin_family == b.sin_family
        && a.sin_port   == b.sin_port
        && a.sin_addr.s_addr == b.sin_addr.s_addr;
}

void FormatAddr(const sockaddr_in& a, char* out, size_t out_sz) {
    char ip[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &a.sin_addr, ip, sizeof(ip));
    std::snprintf(out, out_sz, "%s:%u", ip, ntohs(a.sin_port));
}

// Send a raw UDP buffer to a peer via the shared socket.
void SendRaw(const void* buf, size_t len, const sockaddr_in& to) {
    SOCKET sock = NetSocket_GetHandle();
    if (sock == INVALID_SOCKET) return;
    sendto(sock, reinterpret_cast<const char*>(buf), static_cast<int>(len), 0,
           reinterpret_cast<const sockaddr*>(&to), sizeof(to));
}

// =============================================================================
// BROADCAST
// =============================================================================

// Emit the current pending batch as an INPUT_BATCH datagram to all subscribers.
void FlushBatch() {
    if (g_state.pending.p1_inputs.empty()) return;
    if (g_state.subscribers.empty()) {
        g_state.pending.p1_inputs.clear();
        g_state.pending.p2_inputs.clear();
        return;
    }

    const uint16_t n = static_cast<uint16_t>(g_state.pending.p1_inputs.size());
    const size_t   payload_bytes = n * 4;  // (p1 u16, p2 u16) per frame
    const size_t   dgram_bytes   = sizeof(SpecDataHeader) + payload_bytes;

    std::vector<uint8_t> buf(dgram_bytes);
    SpecDataHeader hdr = {};
    hdr.magic       = SPEC_DATA_MAGIC;
    hdr.type        = SpecDataType::INPUT_BATCH;
    hdr.start_frame = g_state.pending.start_frame;
    hdr.frame_count = n;
    std::memcpy(buf.data(), &hdr, sizeof(hdr));

    // Pack (p1, p2) interleaved — matches ReplayFrame layout exactly so
    // the spectator side can memcpy directly into replay buffer.
    uint8_t* out = buf.data() + sizeof(hdr);
    for (uint16_t i = 0; i < n; i++) {
        uint16_t p1 = g_state.pending.p1_inputs[i];
        uint16_t p2 = g_state.pending.p2_inputs[i];
        std::memcpy(out + i * 4 + 0, &p1, 2);
        std::memcpy(out + i * 4 + 2, &p2, 2);
    }

    for (const auto& sub : g_state.subscribers) {
        SendRaw(buf.data(), buf.size(), sub.addr);
    }

    // Advance start_frame for next batch; clear pending.
    g_state.pending.start_frame += n;
    g_state.pending.p1_inputs.clear();
    g_state.pending.p2_inputs.clear();
}

void SendInitialMatchTo(const sockaddr_in& to) {
    if (!g_state.initial_match.valid) return;
    std::vector<uint8_t> buf(sizeof(SpecDataHeader) +
                             sizeof(g_state.initial_match.header_bytes));
    SpecDataHeader hdr = {};
    hdr.magic       = SPEC_DATA_MAGIC;
    hdr.type        = SpecDataType::INITIAL_MATCH;
    hdr.start_frame = 0;
    hdr.frame_count = 0;
    std::memcpy(buf.data(), &hdr, sizeof(hdr));
    std::memcpy(buf.data() + sizeof(hdr),
                g_state.initial_match.header_bytes,
                sizeof(g_state.initial_match.header_bytes));
    SendRaw(buf.data(), buf.size(), to);
}

// Send the full session input history to a specific subscriber, chunked at
// CHUNK frames per datagram. Used on JOIN_ACK so a late joiner can fast-
// forward from session start to live. Each chunk is its own INPUT_BATCH
// datagram with start_frame set so the spectator can detect any gaps.
//
// Sized to keep each datagram well under typical UDP MTU (1200 B safe):
// 256 frames * 4 B/frame + 10 B header ≈ 1034 B.
constexpr size_t BACKFILL_CHUNK_FRAMES = 256;

void SendSessionBackfillTo(const sockaddr_in& to) {
    const size_t total = g_state.session_history.size();
    if (total == 0) return;

    char addr_buf[48] = {};
    FormatAddr(to, addr_buf, sizeof(addr_buf));
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "SpectatorNode: backfilling %zu frames to %s", total, addr_buf);

    for (size_t off = 0; off < total; off += BACKFILL_CHUNK_FRAMES) {
        const size_t n = std::min(BACKFILL_CHUNK_FRAMES, total - off);
        std::vector<uint8_t> buf(sizeof(SpecDataHeader) + n * 4);

        SpecDataHeader hdr = {};
        hdr.magic       = SPEC_DATA_MAGIC;
        hdr.type        = SpecDataType::INPUT_BATCH;
        hdr.start_frame = g_state.session_start_frame + (uint32_t)off;
        hdr.frame_count = (uint16_t)n;
        std::memcpy(buf.data(), &hdr, sizeof(hdr));

        uint8_t* out = buf.data() + sizeof(hdr);
        for (size_t i = 0; i < n; i++) {
            const PlaybackFrame& f = g_state.session_history[off + i];
            std::memcpy(out + i * 4 + 0, &f.p1_input, 2);
            std::memcpy(out + i * 4 + 2, &f.p2_input, 2);
        }
        SendRaw(buf.data(), buf.size(), to);
    }
}

void SendMatchEndToAll() {
    SpecDataHeader hdr = {};
    hdr.magic = SPEC_DATA_MAGIC;
    hdr.type  = SpecDataType::MATCH_END;
    for (const auto& sub : g_state.subscribers) {
        SendRaw(&hdr, sizeof(hdr), sub.addr);
    }
}

} // namespace

// =============================================================================
// PUBLIC API
// =============================================================================

void SpectatorNode_Init() {
    g_state = State{};
    g_state.capacity = SPECTATOR_DEFAULT_CAPACITY;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "SpectatorNode: Init (capacity=%zu, batch=%zu frames)",
                g_state.capacity, BROADCAST_BATCH_FRAMES);
}

void SpectatorNode_Shutdown() {
    // Best-effort: notify all subscribers before tearing down so they
    // can fail over to root immediately instead of waiting out their
    // silence timer.
    for (const auto& sub : g_state.subscribers) {
        CtrlPacket leave = {};
        leave.header.type = CtrlMsg::SPEC_LEAVE;
        ControlChannel_SendTo(leave, sub.addr);
    }
    // And tell our upstream we're going away (frees their subscriber slot).
    if (g_state.subscribed_upstream) {
        CtrlPacket leave = {};
        leave.header.type = CtrlMsg::SPEC_LEAVE;
        ControlChannel_SendTo(leave, g_state.upstream_addr);
    }

    g_state.subscribers.clear();
    g_state.session_history.clear();
    g_state.session_history.shrink_to_fit();
    g_state.broadcasting = false;
    g_state.subscribed_upstream = false;
    g_state.playing_back = false;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "SpectatorNode: Shutdown");
}

void SpectatorNode_SetCapacity(size_t max_direct) {
    g_state.capacity = max_direct;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "SpectatorNode: Capacity set to %zu", max_direct);
}

// -----------------------------------------------------------------------------
// HOST-SIDE
// -----------------------------------------------------------------------------

void SpectatorNode_OnMatchStart(
    uint32_t game_hash,
    uint32_t initial_rng_seed,
    uint32_t initial_state_hash,
    uint8_t p1_char, uint8_t p1_color,
    uint8_t p2_char, uint8_t p2_color)
{
    g_state.broadcasting = true;
    // Don't reset pending.start_frame — keep it monotonic across matches so
    // INPUT_BATCH start_frame is the session-history index, matching the
    // numbering SendSessionBackfillTo uses. Required for de-dup on
    // failover reconnect (spectator's highest_consumed_frame is also a
    // session-history index; mixing per-match-relative + session-relative
    // numbering on the wire would break de-dup).
    g_state.pending.p1_inputs.clear();
    g_state.pending.p2_inputs.clear();

    // Stash the initial-match metadata as a 96-byte payload identical to
    // Replay::ReplayHeader's on-disk layout, so a subscriber can feed it
    // directly into Replay_LoadFromBuffer. We reconstruct the layout here
    // (can't pull from Replay because that header is per-file with a
    // post-match frame_count — spectators get frame_count=0, streaming).
    uint8_t* h = g_state.initial_match.header_bytes;
    std::memset(h, 0, 96);
    uint32_t magic   = 0x52504D46;  // Replay::REPLAY_MAGIC
    uint16_t version = 1;
    std::memcpy(h + 0,  &magic,              4);
    std::memcpy(h + 4,  &version,            2);
    std::memcpy(h + 16, &game_hash,          4);  // game_hash (after timestamp)
    std::memcpy(h + 20, &initial_rng_seed,   4);
    std::memcpy(h + 24, &initial_state_hash, 4);
    h[28] = p1_char;
    h[29] = p1_color;
    h[30] = p2_char;
    h[31] = p2_color;
    // p1_name / p2_name at h+32 / h+56 left zeroed; filled once UI plumbs them.
    // frame_count at h+92 stays 0 — subscribers get INPUT_BATCH frames live.
    g_state.initial_match.valid = true;

    // Push INITIAL_MATCH to any already-subscribed viewers (they'll drop
    // their old replay and start fresh on the new match).
    for (const auto& sub : g_state.subscribers) {
        SendInitialMatchTo(sub.addr);
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "SpectatorNode: Match start broadcast (seed=0x%08X, subs=%zu)",
                initial_rng_seed, g_state.subscribers.size());
}

void SpectatorNode_OnFrameConfirmed(uint16_t p1_input, uint16_t p2_input) {
    // Always record into session_history so a late joiner can backfill
    // every confirmed frame from session start, including CSS frames that
    // happened before any spectator subscribed. This is cheap (4 B/frame).
    g_state.session_history.push_back({p1_input, p2_input});

    // Live broadcast batching window — only fan out to existing subscribers.
    g_state.pending.p1_inputs.push_back(p1_input);
    g_state.pending.p2_inputs.push_back(p2_input);
    if (g_state.pending.p1_inputs.size() >= BROADCAST_BATCH_FRAMES) {
        FlushBatch();
    }
}

void SpectatorNode_OnMatchEnd() {
    if (!g_state.broadcasting) return;
    // Flush whatever's left in the pending batch so viewers see the final frames.
    FlushBatch();
    SendMatchEndToAll();
    g_state.broadcasting = false;
    g_state.initial_match.valid = false;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "SpectatorNode: Match end broadcast");
}

// -----------------------------------------------------------------------------
// JOIN / REDIRECT
// -----------------------------------------------------------------------------

void SpectatorNode_HandleJoinReq(const sockaddr_in& from) {
    char addr_buf[48] = {};
    FormatAddr(from, addr_buf, sizeof(addr_buf));

    // Already subscribed? Idempotent ACK.
    for (const auto& sub : g_state.subscribers) {
        if (AddrEqual(sub.addr, from)) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "SpectatorNode: JOIN_REQ from existing subscriber %s — re-ACKing",
                        addr_buf);
            CtrlPacket ack = {};
            ack.header.type = CtrlMsg::SPEC_JOIN_ACK;
            ControlChannel_SendTo(ack, from);
            SendInitialMatchTo(from);
            return;
        }
    }

    if (g_state.subscribers.size() < g_state.capacity) {
        Subscriber sub = {};
        sub.addr         = from;
        sub.last_seen_ms = GetTickCount64();
        sub.ack_frame    = 0;
        g_state.subscribers.push_back(sub);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: Accepted subscriber %s (%zu/%zu)",
                    addr_buf, g_state.subscribers.size(), g_state.capacity);

        CtrlPacket ack = {};
        ack.header.type = CtrlMsg::SPEC_JOIN_ACK;
        ControlChannel_SendTo(ack, from);
        SendInitialMatchTo(from);

        // Fast-forward backfill: ship the entire session input history so the
        // late joiner can drain it locally at >100 Hz and catch up to live.
        SendSessionBackfillTo(from);
        return;
    }

    // At capacity — random redirect à la CCCaster.
    if (!g_state.subscribers.empty()) {
        const size_t i = static_cast<size_t>(std::rand()) % g_state.subscribers.size();
        const sockaddr_in& target = g_state.subscribers[i].addr;

        CtrlPacket redir = {};
        redir.header.type = CtrlMsg::SPEC_JOIN_REDIRECT;
        redir.data.spec_redirect.redirect_ip   = target.sin_addr.s_addr;
        redir.data.spec_redirect.redirect_port = ntohs(target.sin_port);
        ControlChannel_SendTo(redir, from);

        char target_buf[48] = {};
        FormatAddr(target, target_buf, sizeof(target_buf));
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: At capacity, redirecting %s -> %s",
                    addr_buf, target_buf);
        return;
    }

    // Capacity=0, no subscribers — reject with null redirect. Viewer gives up.
    CtrlPacket redir = {};
    redir.header.type = CtrlMsg::SPEC_JOIN_REDIRECT;
    redir.data.spec_redirect.redirect_ip   = 0;
    redir.data.spec_redirect.redirect_port = 0;
    ControlChannel_SendTo(redir, from);
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "SpectatorNode: Rejected JOIN_REQ from %s (capacity=0)", addr_buf);
}

void SpectatorNode_HandleLeave(const sockaddr_in& from) {
    // First check: was this from our upstream telling us to leave (it's
    // shutting down)? If so, immediately fail over to root rather than
    // waiting out the silence timer.
    if (g_state.subscribed_upstream && AddrEqual(g_state.upstream_addr, from)) {
        char buf[48] = {}; FormatAddr(from, buf, sizeof(buf));
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: upstream %s sent SPEC_LEAVE — failing over to root",
                    buf);
        g_state.subscribed_upstream = false;
        // TickHealth will pick up the disconnected state and trigger
        // RequestJoin(root) on its next call (rate-limited).
        return;
    }
    // Otherwise, treat as a downstream subscriber leaving us.
    for (auto it = g_state.subscribers.begin(); it != g_state.subscribers.end(); ++it) {
        if (AddrEqual(it->addr, from)) {
            char buf[48] = {}; FormatAddr(from, buf, sizeof(buf));
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "SpectatorNode: Subscriber %s left", buf);
            g_state.subscribers.erase(it);
            return;
        }
    }
}

void SpectatorNode_HandleHeartbeat(const sockaddr_in& from) {
    for (auto& sub : g_state.subscribers) {
        if (AddrEqual(sub.addr, from)) {
            sub.last_seen_ms = GetTickCount64();
            return;
        }
    }
}

// -----------------------------------------------------------------------------
// STATUS
// -----------------------------------------------------------------------------

size_t SpectatorNode_GetSubscriberCount() { return g_state.subscribers.size(); }
bool   SpectatorNode_IsBroadcasting()     { return g_state.broadcasting;      }

// -----------------------------------------------------------------------------
// VIEWER-SIDE
// -----------------------------------------------------------------------------

bool SpectatorNode_RequestJoin(const sockaddr_in& upstream) {
    g_state.upstream_addr       = upstream;
    g_state.subscribed_upstream = false;
    g_state.join_req_pending    = true;  // Gates HandleJoinAck so a stray
                                         // JOIN_ACK from the wire doesn't
                                         // promote a non-spectator client
                                         // into spectator mode.
    CtrlPacket req = {};
    req.header.type = CtrlMsg::SPEC_JOIN_REQ;
    ControlChannel_SendTo(req, upstream);
    return true;
}

void SpectatorNode_HandleJoinAck(const sockaddr_in& from) {
    if (!g_state.join_req_pending) {
        // We never sent a JOIN_REQ — this ACK was misrouted (e.g. host's
        // ack to a different subscriber arrived here because we share the
        // socket). Ignore it entirely; flipping playing_back here would
        // freeze a player client into spectator mode.
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: ignoring stray SPEC_JOIN_ACK (no pending JOIN_REQ)");
        return;
    }
    g_state.join_req_pending      = false;
    g_state.upstream_addr         = from;
    g_state.subscribed_upstream   = true;
    g_state.last_upstream_recv_ms = (uint64_t)GetTickCount64();

    // RNG sync: every netplay reset point on the host writes the same fixed
    // seed (0x12345678) — see CheckFullyConnected, Netplay_StartBattle.
    // Pin our seed to that value the moment we subscribe so the spectator's
    // local CSS code (which makes its own RNG calls before any INITIAL_MATCH
    // arrives) evolves the RNG state identically to the host. Subsequent
    // INITIAL_MATCH packets also write this seed — idempotent.
    *(uint32_t*)0x41FB1C = 0x12345678;

    // Flip into playback mode immediately. No initial match metadata yet, so
    // queue starts empty — trampoline holds the current frame until the host
    // ships the first INPUT_BATCH. Once frames flow, the local FM2K (booted
    // to CSS via game_patches) walks the same CSS path the host's two
    // players walked, locks the same chars, transitions to battle, etc.
    g_state.playing_back = true;
    g_state.pb_queue.clear();
    g_state.pb_current_p1 = 0;
    g_state.pb_current_p2 = 0;

    char buf[48] = {}; FormatAddr(from, buf, sizeof(buf));
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "SpectatorNode: JOIN_ACK from %s — subscribed, RNG pinned to 0x12345678",
                buf);
}

void SpectatorNode_HandleJoinRedirect(const sockaddr_in& from,
                                      uint32_t redirect_ip,
                                      uint16_t redirect_port)
{
    (void)from;
    if (redirect_ip == 0 && redirect_port == 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: Redirect with no target — giving up");
        g_state.subscribed_upstream = false;
        return;
    }
    sockaddr_in target = {};
    target.sin_family      = AF_INET;
    target.sin_addr.s_addr = redirect_ip;
    target.sin_port        = htons(redirect_port);
    char buf[48] = {}; FormatAddr(target, buf, sizeof(buf));
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "SpectatorNode: Redirecting to %s", buf);
    SpectatorNode_RequestJoin(target);
}

void SpectatorNode_HandleSpecData(const uint8_t* buf, size_t len,
                                  const sockaddr_in& from)
{
    (void)from;
    if (len < sizeof(SpecDataHeader)) return;
    SpecDataHeader hdr;
    std::memcpy(&hdr, buf, sizeof(hdr));
    if (hdr.magic != SPEC_DATA_MAGIC) return;

    const uint8_t* payload = buf + sizeof(hdr);
    const size_t   payload_len = len - sizeof(hdr);

    switch (hdr.type) {
        case SpecDataType::INITIAL_MATCH: {
            // 96-byte ReplayHeader payload. New-match metadata: stash char
            // selects + state-hash for UI / sanity, re-write seed (host
            // wrote 0x12345678 at battle start; we mirror to stay locked).
            // We do NOT clear pb_queue or flip playing_back — subscription
            // already turned playback on, and any in-flight CSS-end frames
            // need to keep draining.
            if (payload_len < 96) return;
            uint32_t seed = 0, state_hash = 0;
            std::memcpy(&seed,       payload + 20, 4);
            std::memcpy(&state_hash, payload + 24, 4);

            g_state.pb_initial_seed  = seed;
            g_state.pb_initial_state = state_hash;
            g_state.pb_p1_char       = payload[28];
            g_state.pb_p1_color      = payload[29];
            g_state.pb_p2_char       = payload[30];
            g_state.pb_p2_color      = payload[31];

            // Daisy-chain: cache the full 96-byte header so we can relay it
            // verbatim to any sub-spectators that join us.
            std::memcpy(g_state.initial_match.header_bytes, payload, 96);
            g_state.initial_match.valid = true;

            // Idempotent re-write — same value as JOIN_ACK wrote.
            *(uint32_t*)0x41FB1C = seed;

            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "SpectatorNode: INITIAL_MATCH seed=0x%08X state=0x%08X "
                        "p1=%u/%u p2=%u/%u",
                        seed, state_hash,
                        g_state.pb_p1_char, g_state.pb_p1_color,
                        g_state.pb_p2_char, g_state.pb_p2_color);
            break;
        }
        case SpecDataType::INPUT_BATCH: {
            // Each frame is 4 bytes: (p1 u16, p2 u16). Same on-wire layout
            // as ReplayFrame so memcpy works directly.
            if (!g_state.playing_back) return;
            const size_t expected = (size_t)hdr.frame_count * 4;
            if (payload_len < expected) return;

            // Refresh upstream liveness — any inbound 0xCE counts.
            g_state.last_upstream_recv_ms = (uint64_t)GetTickCount64();

            g_state.pb_queue.reserve(g_state.pb_queue.size() + hdr.frame_count);
            for (uint16_t i = 0; i < hdr.frame_count; i++) {
                const uint32_t global_frame = hdr.start_frame + (uint32_t)i;
                // De-dup gate. On failover the new upstream (root) ships
                // session_history from frame 0; any frame at or before the
                // local sim's cursor was already played and would re-render
                // the past if requeued. Skip silently.
                if (g_state.have_frame_baseline &&
                    global_frame < g_state.next_expected_frame) {
                    continue;
                }
                if (!g_state.have_frame_baseline) {
                    g_state.have_frame_baseline  = true;
                    g_state.next_expected_frame  = global_frame;
                }

                PlaybackFrame f;
                std::memcpy(&f.p1_input, payload + i * 4 + 0, 2);
                std::memcpy(&f.p2_input, payload + i * 4 + 2, 2);
                g_state.pb_queue.push_back(f);
                g_state.next_expected_frame = global_frame + 1;

                // Hop-1 relay (rare overflow path): feed forward to any
                // subscribers we have. Most spectators have zero subs, so
                // this is a no-op cost. When a spectator IS acting as
                // overflow relay, this populates its session_history +
                // broadcast queue so its 1-hop downstreams keep streaming.
                SpectatorNode_OnFrameConfirmed(f.p1_input, f.p2_input);
            }
            // Quiet log — at K=8 frames/batch this fires ~12.5x/sec per batch.
            // Keep at INFO so it shows in file log but throttle to one per second.
            static uint32_t last_log_tick = 0;
            uint32_t now = (uint32_t)GetTickCount64();
            if (now - last_log_tick > 1000) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "SpectatorNode: queue depth=%zu (last batch=%u frames @ start=%u, subs=%zu)",
                            g_state.pb_queue.size(), hdr.frame_count, hdr.start_frame,
                            g_state.subscribers.size());
                last_log_tick = now;
            }
            break;
        }
        case SpecDataType::MATCH_END: {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "SpectatorNode: MATCH_END (drained %zu queued frames)",
                        g_state.pb_queue.size());
            g_state.playing_back = false;
            // Don't clear the queue — let the trampoline drain whatever is
            // buffered so we see the final frames before idling. Next
            // INITIAL_MATCH will reset state.
            break;
        }
    }
}

bool SpectatorNode_IsSubscribedUpstream() { return g_state.subscribed_upstream; }

// -----------------------------------------------------------------------------
// PLAYBACK DRIVER API (called from main_loop_trampoline + Hook_GetPlayerInput)
// -----------------------------------------------------------------------------

bool SpectatorNode_IsPlayingBack() {
    // Sticky once subscribed: stays true from JOIN_ACK through everything
    // (active matches, MATCH_END drains, post-match idle, between-match
    // CSS), only resetting on shutdown / leave. This is what makes
    // Hook_GetPlayerInput unconditionally route through the
    // spectator-cached values — important because the spectator is
    // marked Netplay_IsConnected() (we set it in InitAsSpectator), so
    // without this gate the CSS branch of Hook_GetPlayerInput would
    // serve garbage from the spectator's empty CSS input buffers.
    return g_state.subscribed_upstream
        || g_state.playing_back
        || !g_state.pb_queue.empty();
}

bool SpectatorNode_PopFrameInputs(uint16_t* p1_input, uint16_t* p2_input) {
    if (g_state.pb_queue.empty()) return false;
    const PlaybackFrame f = g_state.pb_queue.front();
    g_state.pb_queue.erase(g_state.pb_queue.begin());
    g_state.pb_current_p1 = f.p1_input;
    g_state.pb_current_p2 = f.p2_input;
    if (p1_input) *p1_input = f.p1_input;
    if (p2_input) *p2_input = f.p2_input;
    return true;
}

uint16_t SpectatorNode_GetCurrentP1Input() { return g_state.pb_current_p1; }
uint16_t SpectatorNode_GetCurrentP2Input() { return g_state.pb_current_p2; }
size_t   SpectatorNode_PendingFrameCount() { return g_state.pb_queue.size(); }

void SpectatorNode_ResetCurrentInputs() {
    g_state.pb_current_p1 = 0;
    g_state.pb_current_p2 = 0;
}

void SpectatorNode_LeaveUpstream() {
    if (!g_state.subscribed_upstream) return;
    CtrlPacket leave = {};
    leave.header.type = CtrlMsg::SPEC_LEAVE;
    ControlChannel_SendTo(leave, g_state.upstream_addr);
    g_state.subscribed_upstream = false;
}

void SpectatorNode_SetRootAddr(const sockaddr_in& root) {
    g_state.root_addr = root;
}

// Periodic health tick. Three jobs:
//   1. Heartbeat to current upstream every HEARTBEAT_INTERVAL_MS — lets
//      upstream's expiry sweep know we're still alive (otherwise it'd
//      drop us after SUBSCRIBER_EXPIRY_MS of silence).
//   2. Failover on silence: if no inbound from upstream for
//      SILENCE_FAILOVER_MS, assume upstream died. Send SPEC_LEAVE
//      (best-effort) to current upstream, then re-RequestJoin against
//      root_addr. Throttle reconnect attempts to once per RECONNECT_BACKOFF.
//   3. Upstream-side sweep: drop subscribers that haven't sent a
//      heartbeat in SUBSCRIBER_EXPIRY_MS (notify via SPEC_LEAVE so they
//      fail over instantly instead of waiting for their own silence
//      timer).
//
// Cheap to call every iter; the work is gated on the time deltas.
void SpectatorNode_TickHealth() {
    const uint64_t now = (uint64_t)GetTickCount64();

    // ---- Subscriber-side: heartbeat + silence failover ------------------
    if (g_state.subscribed_upstream) {
        if (now - g_state.last_heartbeat_send_ms >= SPECTATOR_HEARTBEAT_INTERVAL_MS) {
            CtrlPacket hb = {};
            hb.header.type = CtrlMsg::SPEC_HEARTBEAT;
            ControlChannel_SendTo(hb, g_state.upstream_addr);
            g_state.last_heartbeat_send_ms = now;
        }

        if (g_state.last_upstream_recv_ms > 0 &&
            now - g_state.last_upstream_recv_ms >= SPECTATOR_SILENCE_FAILOVER_MS)
        {
            // Upstream died. Mark unsubscribed; the not-subscribed branch
            // below will trigger reconnect-to-root on next tick.
            char buf[48] = {}; FormatAddr(g_state.upstream_addr, buf, sizeof(buf));
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "SpectatorNode: upstream %s silent for %llu ms — failing over",
                        buf,
                        (unsigned long long)(now - g_state.last_upstream_recv_ms));
            // Best-effort SPEC_LEAVE to the dead upstream (might no-op).
            CtrlPacket leave = {};
            leave.header.type = CtrlMsg::SPEC_LEAVE;
            ControlChannel_SendTo(leave, g_state.upstream_addr);
            g_state.subscribed_upstream = false;
        }
    }

    // Reconnect path: not subscribed, but we have a root we can fall
    // back to. Throttle so we don't spam JOIN_REQ.
    if (!g_state.subscribed_upstream &&
        g_state.root_addr.sin_port != 0 &&
        now - g_state.last_reconnect_attempt_ms >= SPECTATOR_RECONNECT_BACKOFF_MS)
    {
        char buf[48] = {}; FormatAddr(g_state.root_addr, buf, sizeof(buf));
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: reconnecting to root %s", buf);
        g_state.last_reconnect_attempt_ms = now;
        SpectatorNode_RequestJoin(g_state.root_addr);
    }

    // ---- Upstream-side: expire silent subscribers -----------------------
    for (auto it = g_state.subscribers.begin(); it != g_state.subscribers.end(); ) {
        if (now - it->last_seen_ms >= SPECTATOR_SUBSCRIBER_EXPIRY_MS) {
            char buf[48] = {}; FormatAddr(it->addr, buf, sizeof(buf));
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "SpectatorNode: subscriber %s silent — expiring + sending LEAVE",
                        buf);
            // Notify so they fail over fast instead of waiting their own timer.
            CtrlPacket leave = {};
            leave.header.type = CtrlMsg::SPEC_LEAVE;
            ControlChannel_SendTo(leave, it->addr);
            it = g_state.subscribers.erase(it);
        } else {
            ++it;
        }
    }
}
