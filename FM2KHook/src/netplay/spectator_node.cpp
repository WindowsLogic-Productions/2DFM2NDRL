#include "spectator_node.h"
#include "spectator_tcp.h"        // TCP transport for INPUT_BATCH stream
#include "control_channel.h"
#include "netplay.h"
#include "replay.h"
#include "netplay_state.h"
#include "../audio/sound_rollback.h"  // Op apply: SOUND_INIT
#include "gekkonet.h"

#include <SDL3/SDL_log.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <array>
#include <vector>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <ctime>

// =============================================================================
// MODULE STATE
// =============================================================================

namespace {

// Broadcast cadence: emit a batch every N newly-confirmed frames.
constexpr size_t BROADCAST_BATCH_FRAMES = 8;

// Redundancy window: each batch carries the last N confirmed frames (not
// just the BROADCAST_BATCH_FRAMES new ones). This means each frame appears
// in floor(REDUNDANCY_WINDOW / BROADCAST_BATCH_FRAMES) consecutive batches,
// so a single dropped UDP packet doesn't lose any inputs — the next batch
// re-delivers them. With WINDOW=32 and BATCH=8, each frame ships in 4
// batches: tolerates up to 3 consecutive packet losses before any data is
// genuinely lost.
//
// Wire cost: ~128 B / batch payload (32 frames × 4 B) + 10 B header + UDP
// overhead ≈ 180 B / packet. At 12.5 batches/sec that's ~2.3 KB/s per
// subscriber — still nothing.
constexpr size_t REDUNDANCY_WINDOW = 32;

// Subscriber entry from the host's perspective.
struct Subscriber {
    sockaddr_in addr;
    uint64_t    last_seen_ms;
    uint32_t    ack_frame;    // Last frame we know this subscriber has. TODO: fill on SPEC_ACK.
    bool        tcp_bound;    // True once SpectatorTCP::RegisterAcceptedClient(addr)
                              // has paired this sub with an accepted TCP socket.
                              // Until then, INITIAL_MATCH + backfill are deferred —
                              // any send before binding silently drops on the floor.
};

// Cached initial-match metadata so new joiners get a consistent handoff.
struct InitialMatch {
    bool     valid;
    uint8_t  header_bytes[96];  // Serialized ReplayHeader
};

// Side table of MATCH_START headers (96 bytes each) referenced by index from
// SessionEvent::u.match_start_idx. Out-of-band so SessionEvent stays at 5 B
// in memory regardless of match boundary frequency.
using MatchHeader = std::array<uint8_t, SESSION_EVENT_MATCH_HDR_SIZE>;

struct State {
    bool                      broadcasting      = false;
    size_t                    capacity          = SPECTATOR_DEFAULT_CAPACITY;
    std::vector<Subscriber>   subscribers;
    InitialMatch              initial_match     = {};

    // ─── HOST SIDE: session event log ──────────────────────────────────
    // Every confirmed event the host produces (INPUT pairs in C2; PIN_RNG
    // / RESET_INPUT_STATE / SOUND_INIT / MATCH_START / MATCH_END /
    // FINGERPRINT in C3+) is appended monotonically here. Late joiners get
    // the whole vector replayed via SendSessionBackfillTo. Memory: 5 B/event
    // → ~1.7 MB for an hour of 100 Hz INPUT events; non-INPUT events are
    // sparse (a few per match).
    std::vector<SessionEvent> session_events;
    std::vector<MatchHeader>  match_headers;     // side table for MATCH_START
    uint32_t                  total_input_count   = 0;  // INPUT events in session_events
    uint32_t                  session_start_frame = 0;  // INPUT-frame index of session_events[0]
                                                         // (always 0 unless we ever drop history)

    // Pending broadcast cursor: events past this index are unflushed.
    // FlushBatch encodes [last_flushed_event_idx .. session_events.size()),
    // emits as one EVENT_BATCH datagram, advances the cursor.
    size_t                    last_flushed_event_idx = 0;
    uint32_t                  flushed_input_count    = 0;  // INPUT events flushed so far

    // Index of the most recent MATCH_START event in session_events. Used by
    // SpectatorNode_WriteCurrentBattleFile to slice the per-battle segment.
    // -1 sentinel = no MATCH_START emitted in this session yet.
    int64_t                   last_match_start_idx   = -1;

    // ─── VIEWER SIDE: playback queue ───────────────────────────────────
    // Populated by HandleSpecData (decodes incoming EVENT_BATCH /
    // INPUT_BATCH); consumed by RunSpectatorTick. Mirrors the host's
    // session_events shape: typed events drained in order, non-INPUT
    // events at the head are applied (RNG pin etc.) before the next
    // INPUT pops. C2 keeps non-INPUT drain as a no-op gate; C3+ wires
    // the apply paths.
    std::vector<SessionEvent> pb_queue;
    std::vector<MatchHeader>  pb_match_headers;

    // Viewer-side state (this node subscribed upstream).
    bool                      subscribed_upstream = false;
    bool                      join_req_pending    = false;  // We sent SPEC_JOIN_REQ; expect ACK.
    sockaddr_in               upstream_addr       = {};
    // Failover support. root_addr is the originally-configured upstream
    // (the actual match host). If our current upstream goes silent we
    // fall back to root, which always-on by design. Liveness is tracked
    // via SpectatorTCP::LastUpstreamRecvMs() (TCP-side) — this struct
    // owns only handshake / heartbeat send-cadence state.
    sockaddr_in               root_addr           = {};
    uint64_t                  last_heartbeat_send_ms = 0;
    uint64_t                  last_reconnect_attempt_ms = 0;
    // De-dup gate. Backfill from a reconnected upstream replays history
    // from frame 0; frames at or below this counter were already consumed
    // locally and would re-render the past. Updated by PopFrameInputs.
    uint32_t                  highest_consumed_frame = 0;
    // Frame-counter offset so highest_consumed_frame is comparable to
    // the start_frame field on incoming EVENT_BATCH datagrams. Set on the
    // first batch received after JOIN_ACK; subsequent batches increment.
    bool                      have_frame_baseline = false;
    uint32_t                  next_expected_frame = 0;  // session-relative INPUT-frame index
    // Pull-based gap recovery: periodic + on-demand INPUT_REQUEST
    // (spectator → upstream) for the missing frame range. Closes any
    // gap that the push-based redundancy window can't recover from.
    uint64_t                  last_input_request_send_ms = 0;

    // Viewer-side playback driver state.
    // Populated by HandleSpecData (INITIAL_MATCH / EVENT_BATCH / MATCH_END);
    // consumed by the trampoline's RunSpectatorTick + Hook_GetPlayerInput.
    bool                      playing_back        = false;
    uint32_t                  pb_initial_seed     = 0;
    uint32_t                  pb_initial_state    = 0;  // fingerprint sanity
    uint8_t                   pb_p1_char          = 0;
    uint8_t                   pb_p1_color         = 0;
    uint8_t                   pb_p2_char          = 0;
    uint8_t                   pb_p2_color         = 0;
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

// Append one event's wire encoding into a byte vector. Used by both the
// live broadcast path (FlushBatch) and the backfill path
// (SendSessionBackfillTo) — keeps wire-encoding logic in one place.
void AppendEventToWire(std::vector<uint8_t>& out, const SessionEvent& ev,
                       const std::vector<MatchHeader>& headers) {
    uint8_t buf[SESSION_EVENT_MAX_WIRE_SIZE];
    size_t w = 0;
    switch (ev.type) {
        case SessionEventType::INPUT:
            w = SessionEvent_EncodeInput(buf, sizeof(buf),
                                         ev.u.input.p1, ev.u.input.p2);
            break;
        case SessionEventType::PIN_RNG:
            w = SessionEvent_EncodePinRng(buf, sizeof(buf), ev.u.pin_rng_seed);
            break;
        case SessionEventType::RESET_INPUT_STATE:
            w = SessionEvent_EncodeResetInputState(buf, sizeof(buf));
            break;
        case SessionEventType::SOUND_INIT:
            w = SessionEvent_EncodeSoundInit(buf, sizeof(buf));
            break;
        case SessionEventType::MATCH_START:
            if (ev.u.match_start_idx < headers.size()) {
                w = SessionEvent_EncodeMatchStart(buf, sizeof(buf),
                                                  headers[ev.u.match_start_idx].data());
            }
            break;
        case SessionEventType::MATCH_END:
            w = SessionEvent_EncodeMatchEnd(buf, sizeof(buf));
            break;
        case SessionEventType::FINGERPRINT:
            w = SessionEvent_EncodeFingerprint(buf, sizeof(buf), ev.u.fingerprint_hash);
            break;
    }
    if (w > 0) out.insert(out.end(), buf, buf + w);
}

// Count INPUT events in [first, last) of a SessionEvent slice. Used to
// populate SpecDataHeader.frame_count for log/diagnostic purposes — wire
// dedup uses start_frame.
uint32_t CountInputs(const std::vector<SessionEvent>& events,
                     size_t first, size_t last) {
    uint32_t n = 0;
    for (size_t i = first; i < last; i++) {
        if (events[i].type == SessionEventType::INPUT) ++n;
    }
    return n;
}

// Emit an EVENT_BATCH datagram covering all events appended since the last
// flush. TCP guarantees in-order, exactly-once delivery — no redundancy
// window. The header carries:
//   start_frame = session-relative INPUT-frame index of the first INPUT in
//                 this batch (= flushed_input_count at entry). Used by the
//                 receiver's next_expected_frame dedup gate.
//   frame_count = number of INPUT events in this batch (informational).
//   payload     = packed SessionEvent[] (1-byte type tag + variant payload
//                 per event; see SessionEvent_Encode* in this file).
void FlushBatch() {
    const size_t first = g_state.last_flushed_event_idx;
    const size_t last  = g_state.session_events.size();
    if (first == last) return;

    const uint32_t input_count = CountInputs(g_state.session_events, first, last);
    const uint32_t start_frame = g_state.flushed_input_count;

    // Advance flush watermark unconditionally so the cadence trigger
    // ("every BROADCAST_BATCH_FRAMES INPUT events") keeps walking even
    // when there are no subscribers to receive the batch.
    g_state.last_flushed_event_idx = last;
    g_state.flushed_input_count   += input_count;

    if (g_state.subscribers.empty()) return;

    std::vector<uint8_t> payload;
    payload.reserve((last - first) * SESSION_EVENT_MAX_WIRE_SIZE);
    for (size_t i = first; i < last; i++) {
        AppendEventToWire(payload, g_state.session_events[i], g_state.match_headers);
    }
    if (payload.empty()) return;

    std::vector<uint8_t> buf(sizeof(SpecDataHeader) + payload.size());
    SpecDataHeader hdr = {};
    hdr.magic       = SPEC_DATA_MAGIC;
    hdr.type        = SpecDataType::EVENT_BATCH;
    hdr.start_frame = start_frame;
    hdr.frame_count = static_cast<uint16_t>(std::min<uint32_t>(input_count, 0xFFFFu));
    // For EVENT_BATCH, flags carries the payload byte count so the TCP
    // framer can size the receive without re-decoding events. 16-bit cap
    // (65535) is well above any reasonable live FlushBatch — backfill
    // chunks are explicitly bounded at BACKFILL_CHUNK_BYTES=1024.
    hdr.flags       = static_cast<uint16_t>(std::min<size_t>(payload.size(), 0xFFFFu));
    std::memcpy(buf.data(), &hdr, sizeof(hdr));
    std::memcpy(buf.data() + sizeof(hdr), payload.data(), payload.size());

    SpectatorTCP::BroadcastToAll(buf.data(), buf.size());
}

// Legacy SendInitialMatchTo / INITIAL_MATCH packet path removed in C12.
// MATCH_START flows as a SessionEvent op interleaved with INPUTs in the
// EVENT_BATCH stream; late joiners get it via SendSessionBackfillTo.

// Send the full session event log to a specific subscriber, chunked at
// ~BACKFILL_CHUNK_BYTES per datagram. Used on JOIN_ACK so a late joiner
// can fast-forward from session start to live. Each chunk is its own
// EVENT_BATCH datagram. Hdr.start_frame = session-relative INPUT-frame
// index of the first INPUT in the chunk; receiver's dedup gate uses this
// to detect gaps / redundant retransmits across reconnects.
//
// Bytes-based chunking (vs the old fixed-frame chunking) handles variable-
// length events: a chunk with mostly INPUT events fits ~200 events; a
// chunk with a 96-byte MATCH_START fits fewer. Sized to stay under typical
// UDP MTU (1200 B safe) once we keep TCP — over TCP this only matters for
// receive-buffer pacing and avoids an oversized syscall stalling the
// session_events traversal.
constexpr size_t BACKFILL_CHUNK_BYTES = 1024;

void SendSessionBackfillTo(const sockaddr_in& to) {
    const size_t total_events = g_state.session_events.size();
    if (total_events == 0) return;

    char addr_buf[48] = {};
    FormatAddr(to, addr_buf, sizeof(addr_buf));
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "SpectatorNode: backfilling %zu events (%u INPUTs) to %s",
                total_events, g_state.total_input_count, addr_buf);

    // Walk session_events, packing into chunks bounded by BACKFILL_CHUNK_BYTES.
    // For each chunk, hdr.start_frame is the INPUT-frame index of the first
    // INPUT *in the chunk*. If a chunk is non-INPUT-only (rare; trailing
    // ops at session tail), use the running input cursor.
    size_t   ev_idx          = 0;
    uint32_t cursor_inputs   = g_state.session_start_frame;  // INPUT-frame idx of session_events[0]

    while (ev_idx < total_events) {
        std::vector<uint8_t> payload;
        payload.reserve(BACKFILL_CHUNK_BYTES);

        const size_t chunk_first_idx   = ev_idx;
        uint32_t     chunk_first_input = cursor_inputs;
        bool         saw_input         = false;
        uint32_t     chunk_input_count = 0;

        while (ev_idx < total_events) {
            const SessionEvent& ev = g_state.session_events[ev_idx];
            // Pre-encode to know the size; if it'd overflow the chunk
            // budget, defer to the next chunk (unless the chunk is empty,
            // in which case ship even an oversize event alone).
            uint8_t one[SESSION_EVENT_MAX_WIRE_SIZE];
            size_t  one_w = 0;
            switch (ev.type) {
                case SessionEventType::INPUT:
                    one_w = SessionEvent_EncodeInput(one, sizeof(one),
                                                     ev.u.input.p1, ev.u.input.p2);
                    break;
                case SessionEventType::PIN_RNG:
                    one_w = SessionEvent_EncodePinRng(one, sizeof(one),
                                                      ev.u.pin_rng_seed);
                    break;
                case SessionEventType::RESET_INPUT_STATE:
                    one_w = SessionEvent_EncodeResetInputState(one, sizeof(one));
                    break;
                case SessionEventType::SOUND_INIT:
                    one_w = SessionEvent_EncodeSoundInit(one, sizeof(one));
                    break;
                case SessionEventType::MATCH_START:
                    if (ev.u.match_start_idx < g_state.match_headers.size()) {
                        one_w = SessionEvent_EncodeMatchStart(one, sizeof(one),
                            g_state.match_headers[ev.u.match_start_idx].data());
                    }
                    break;
                case SessionEventType::MATCH_END:
                    one_w = SessionEvent_EncodeMatchEnd(one, sizeof(one));
                    break;
                case SessionEventType::FINGERPRINT:
                    one_w = SessionEvent_EncodeFingerprint(one, sizeof(one),
                                                           ev.u.fingerprint_hash);
                    break;
            }
            if (one_w == 0) { ++ev_idx; continue; }   // unknown / unencodable

            if (!payload.empty() && payload.size() + one_w > BACKFILL_CHUNK_BYTES) {
                break;  // emit current chunk; this event goes in the next
            }
            payload.insert(payload.end(), one, one + one_w);

            if (ev.type == SessionEventType::INPUT) {
                if (!saw_input) {
                    saw_input         = true;
                    chunk_first_input = cursor_inputs;
                }
                ++chunk_input_count;
                ++cursor_inputs;
            }
            ++ev_idx;
        }

        if (payload.empty()) break;

        std::vector<uint8_t> buf(sizeof(SpecDataHeader) + payload.size());
        SpecDataHeader hdr = {};
        hdr.magic       = SPEC_DATA_MAGIC;
        hdr.type        = SpecDataType::EVENT_BATCH;
        hdr.start_frame = saw_input ? chunk_first_input
                                    : cursor_inputs;     // tail-only chunk
        hdr.frame_count = static_cast<uint16_t>(std::min<uint32_t>(chunk_input_count, 0xFFFFu));
        // EVENT_BATCH: flags carries payload byte count for the receiver's
        // TCP framer (see PayloadLenForType in spectator_tcp.cpp). The
        // BACKFILL_CHUNK_BYTES=1024 cap keeps this well under the 16-bit
        // limit. (Was: bit 0 flagged deferred-ops chunk — moot now since
        // the framer needs the byte count regardless.)
        hdr.flags       = static_cast<uint16_t>(std::min<size_t>(payload.size(), 0xFFFFu));
        std::memcpy(buf.data(), &hdr, sizeof(hdr));
        std::memcpy(buf.data() + sizeof(hdr), payload.data(), payload.size());

        SpectatorTCP::SendTo(to, buf.data(), buf.size());

        (void)chunk_first_idx;  // reserved for future per-chunk diagnostics
    }
}

// Legacy SendMatchEndToAll / MATCH_END packet path removed in C12.
// MATCH_END flows as a SessionEvent op (see SpectatorNode_AppendMatchEnd).

// (SendInputRequest + RespondToInputRequest deleted: TCP guarantees in-order
// delivery exactly once, so the spectator-side gap-recovery handshake is
// dead code. The whole class of UDP-loss recovery — REDUNDANCY_WINDOW,
// INPUT_REQUEST_POLL_MS in TickHealth, the on-gap immediate request inside
// HandleSpecData — has been removed.)

} // namespace

// =============================================================================
// SESSION EVENT WIRE FORMAT (C1)
// =============================================================================
//
// Pure byte-level encoders/decoders. No socket / state side effects — these
// just mediate between SessionEvent values and packed wire bytes. Production
// integration (vector<SessionEvent> session_history, head-of-queue drain in
// RunSpectatorTick, etc.) is layered on top in C2+.

namespace {

// Wire payload size (excluding the 1-byte type tag) per SessionEventType.
// Used by both encoders (sanity) and the decoder (bounds check before memcpy).
size_t WirePayloadSize(SessionEventType t) {
    switch (t) {
        case SessionEventType::INPUT:             return 4;   // u16 + u16
        case SessionEventType::PIN_RNG:           return 4;   // u32
        case SessionEventType::RESET_INPUT_STATE: return 0;
        case SessionEventType::SOUND_INIT:        return 0;
        case SessionEventType::MATCH_START:       return SESSION_EVENT_MATCH_HDR_SIZE;
        case SessionEventType::MATCH_END:         return 0;
        case SessionEventType::FINGERPRINT:       return 4;   // u32
    }
    return SIZE_MAX;  // unknown tag — caller treats as malformed
}

bool IsValidEventTag(uint8_t tag) {
    return tag >= static_cast<uint8_t>(SessionEventType::INPUT)
        && tag <= static_cast<uint8_t>(SessionEventType::FINGERPRINT);
}

} // namespace

size_t SessionEvent_EncodeInput(uint8_t* out, size_t cap, uint16_t p1, uint16_t p2) {
    constexpr size_t need = 1 + 4;
    if (cap < need) return 0;
    out[0] = static_cast<uint8_t>(SessionEventType::INPUT);
    std::memcpy(out + 1, &p1, 2);
    std::memcpy(out + 3, &p2, 2);
    return need;
}

size_t SessionEvent_EncodePinRng(uint8_t* out, size_t cap, uint32_t seed) {
    constexpr size_t need = 1 + 4;
    if (cap < need) return 0;
    out[0] = static_cast<uint8_t>(SessionEventType::PIN_RNG);
    std::memcpy(out + 1, &seed, 4);
    return need;
}

size_t SessionEvent_EncodeResetInputState(uint8_t* out, size_t cap) {
    if (cap < 1) return 0;
    out[0] = static_cast<uint8_t>(SessionEventType::RESET_INPUT_STATE);
    return 1;
}

size_t SessionEvent_EncodeSoundInit(uint8_t* out, size_t cap) {
    if (cap < 1) return 0;
    out[0] = static_cast<uint8_t>(SessionEventType::SOUND_INIT);
    return 1;
}

size_t SessionEvent_EncodeMatchStart(uint8_t* out, size_t cap,
                                     const uint8_t header[SESSION_EVENT_MATCH_HDR_SIZE]) {
    constexpr size_t need = 1 + SESSION_EVENT_MATCH_HDR_SIZE;
    if (cap < need) return 0;
    out[0] = static_cast<uint8_t>(SessionEventType::MATCH_START);
    std::memcpy(out + 1, header, SESSION_EVENT_MATCH_HDR_SIZE);
    return need;
}

size_t SessionEvent_EncodeMatchEnd(uint8_t* out, size_t cap) {
    if (cap < 1) return 0;
    out[0] = static_cast<uint8_t>(SessionEventType::MATCH_END);
    return 1;
}

size_t SessionEvent_EncodeFingerprint(uint8_t* out, size_t cap, uint32_t hash) {
    constexpr size_t need = 1 + 4;
    if (cap < need) return 0;
    out[0] = static_cast<uint8_t>(SessionEventType::FINGERPRINT);
    std::memcpy(out + 1, &hash, 4);
    return need;
}

size_t SessionEvent_Decode(const uint8_t* in, size_t in_len,
                           SessionEvent* out_event,
                           uint8_t out_match_header[SESSION_EVENT_MATCH_HDR_SIZE]) {
    if (in_len < 1) return 0;
    const uint8_t tag = in[0];
    if (!IsValidEventTag(tag)) return 0;

    const SessionEventType type = static_cast<SessionEventType>(tag);
    const size_t payload = WirePayloadSize(type);
    if (in_len < 1 + payload) return 0;

    if (out_event) {
        out_event->type = type;
        // Zero the union so unused-variant readers see deterministic 0s.
        std::memset(&out_event->u, 0, sizeof(out_event->u));

        switch (type) {
            case SessionEventType::INPUT:
                std::memcpy(&out_event->u.input.p1, in + 1, 2);
                std::memcpy(&out_event->u.input.p2, in + 3, 2);
                break;
            case SessionEventType::PIN_RNG:
                std::memcpy(&out_event->u.pin_rng_seed, in + 1, 4);
                break;
            case SessionEventType::FINGERPRINT:
                std::memcpy(&out_event->u.fingerprint_hash, in + 1, 4);
                break;
            case SessionEventType::MATCH_START:
                // u.match_start_idx left at 0 — caller assigns when appending
                // to its match_headers side table.
                break;
            case SessionEventType::RESET_INPUT_STATE:
            case SessionEventType::SOUND_INIT:
            case SessionEventType::MATCH_END:
                break;
        }
    }
    if (type == SessionEventType::MATCH_START && out_match_header) {
        std::memcpy(out_match_header, in + 1, SESSION_EVENT_MATCH_HDR_SIZE);
    }

    return 1 + payload;
}

// =============================================================================
// PUBLIC API
// =============================================================================

void SpectatorNode_Init() {
    g_state = State{};
    g_state.capacity = SPECTATOR_DEFAULT_CAPACITY;

    // Bring up the TCP listener for the host→spectator INPUT_BATCH stream.
    // We bind TCP to the same port as the UDP control socket — TCP and UDP
    // share the same port-number space without conflict, and reusing the
    // UDP port means spectators already know it (they connected over UDP
    // there) so the host_tcp_port field in JOIN_ACK is redundant but kept
    // for explicitness. Idempotent — re-Init won't double-bind.
    const uint16_t udp_port = NetSocket_GetLocalPort();
    if (!SpectatorTCP::StartListener(udp_port)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "SpectatorNode: Init — TCP listener failed to bind on port %u; "
            "spectator subscriptions will fail", (unsigned)udp_port);
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "SpectatorNode: Init (capacity=%zu, batch=%zu frames, "
                "tcp_port=%u)",
                g_state.capacity, BROADCAST_BATCH_FRAMES,
                (unsigned)SpectatorTCP::GetListenPort());
}

void SpectatorNode_Shutdown() {
    // C7: write full session log on shutdown if there's anything to flush.
    // Skipped on the spectator side where session_events is the relay log
    // (correct to write — the relay's local view IS the canonical session
    // for any sub-spectators, even if we'd be one of two writers when
    // host + spectator both run on this machine; per-process file paths
    // already include timestamp + pid disambiguation).
    if (!g_state.session_events.empty()) {
        char ts[64] = {};
        std::time_t now = std::time(nullptr);
        std::tm tm_buf{};
        localtime_s(&tm_buf, &now);
        std::strftime(ts, sizeof(ts), "sessions/%Y-%m-%d_%H%M%S.fm2kset", &tm_buf);
        CreateDirectoryA("sessions", nullptr);
        SpectatorNode_WriteSessionFile(ts);
    }

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
    g_state.session_events.clear();
    g_state.session_events.shrink_to_fit();
    g_state.match_headers.clear();
    g_state.match_headers.shrink_to_fit();
    g_state.last_flushed_event_idx = 0;
    g_state.flushed_input_count    = 0;
    g_state.total_input_count      = 0;
    g_state.broadcasting = false;
    g_state.subscribed_upstream = false;
    g_state.playing_back = false;
    SpectatorTCP::Shutdown();
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
    // Flush any unbatched CSS events before the match-start MATCH_START
    // event hits the wire — keeps the per-INPUT-frame numbering monotonic
    // across the CSS→battle seam. Without this, trailing CSS frames sit
    // unbatched in session_events past last_flushed_event_idx; their
    // session-relative INPUT-frame indices are below the next live battle
    // batch's start_frame, but the spectator's next_expected_frame would
    // already be at the higher index → indefinite gap.
    FlushBatch();

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

    // C6: append MATCH_START as a SessionEvent op so the metadata flows in
    // the same ordered stream as INPUTs. Spectator's drain applies the op
    // at exactly the logical frame the host set the match up. Late joiners
    // get this op as part of SendSessionBackfillTo. The legacy INITIAL_MATCH
    // packet path (still sent below) is kept for back-compat; once all
    // peers run C6+ builds we can retire it.
    SpectatorNode_AppendMatchStart(g_state.initial_match.header_bytes);

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "SpectatorNode: Match start broadcast (seed=0x%08X, subs=%zu)",
                initial_rng_seed, g_state.subscribers.size());
}

void SpectatorNode_OnFrameConfirmed(uint16_t p1_input, uint16_t p2_input) {
    // Append an INPUT event to the session log so a late joiner can backfill
    // every confirmed frame from session start, including CSS frames that
    // happened before any spectator subscribed. 5 B/event in memory.
    SessionEvent ev{};
    ev.type = SessionEventType::INPUT;
    ev.u.input.p1 = p1_input;
    ev.u.input.p2 = p2_input;
    g_state.session_events.push_back(ev);
    ++g_state.total_input_count;

    // Live broadcast batching window — only fan out to existing subscribers.
    // Cadence trigger: every BROADCAST_BATCH_FRAMES new INPUT events.
    const uint32_t pending_inputs =
        g_state.total_input_count - g_state.flushed_input_count;
    if (pending_inputs >= BROADCAST_BATCH_FRAMES) {
        FlushBatch();
    }
}

void SpectatorNode_OnMatchEnd() {
    if (!g_state.broadcasting) return;
    // Flush whatever's left in the pending event window so viewers see the
    // final frames before MATCH_END.
    FlushBatch();
    // MATCH_END flows in-band as a SessionEvent op; the apply-at-head drain
    // on the receiver flips playing_back=false at the same logical frame
    // the host appended. (Legacy MATCH_END packet was retired in C12.)
    SpectatorNode_AppendMatchEnd();
    g_state.broadcasting = false;
    g_state.initial_match.valid = false;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "SpectatorNode: Match end broadcast");
}

// -----------------------------------------------------------------------------
// HOST-SIDE OP APPENDERS (C3)
// -----------------------------------------------------------------------------
//
// Append-and-flush helpers. Called by host pin sites in netplay.cpp /
// savestate.cpp immediately after the local memory write. The append+flush
// pair guarantees the op reaches subscribed spectators before the next
// INPUT event in the stream — drain-at-head semantics on the receiver
// then apply the op exactly when the spectator's local sim is about to
// consume the same logical frame the host did.

namespace {

void AppendOpAndFlush(const SessionEvent& ev) {
    g_state.session_events.push_back(ev);
    // Flush eagerly when subscribers exist (host with live spectators OR
    // relay node with sub-spectators). When the subscriber list is empty,
    // there's nothing to send; late joiners get the full backlog via
    // SendSessionBackfillTo. Note: we don't gate on `broadcasting` —
    // that flag is host-side match state and doesn't apply to the relay
    // path where a spectator's HandleSpecData re-Appends incoming ops
    // to its own session_events for sub-spectator forwarding.
    if (!g_state.subscribers.empty()) {
        FlushBatch();
    }
}

} // namespace

void SpectatorNode_AppendPinRng(uint32_t seed) {
    SessionEvent ev{};
    ev.type            = SessionEventType::PIN_RNG;
    ev.u.pin_rng_seed  = seed;
    AppendOpAndFlush(ev);
}

void SpectatorNode_AppendResetInputState() {
    SessionEvent ev{};
    ev.type = SessionEventType::RESET_INPUT_STATE;
    AppendOpAndFlush(ev);
}

void SpectatorNode_AppendSoundInit() {
    SessionEvent ev{};
    ev.type = SessionEventType::SOUND_INIT;
    AppendOpAndFlush(ev);
}

void SpectatorNode_AppendFingerprint(uint32_t hash) {
    SessionEvent ev{};
    ev.type                  = SessionEventType::FINGERPRINT;
    ev.u.fingerprint_hash    = hash;
    AppendOpAndFlush(ev);
}

// =============================================================================
// FINGERPRINT (C9) — diagnostic state hash for desync detection
// =============================================================================
//
// Both host and spectator sample the same set of FM2K state fields, hash
// them with classic Fletcher-32, and the host appends the result as a
// FINGERPRINT op every 30 sim frames. Spectator's ApplySessionEvent
// computes its own hash on its current state at the same logical frame
// (drain-at-head ordering ensures it's the same frame the host hashed)
// and logs WARN on mismatch, including both values. Replaces the manual
// [HOST-FP] / [SPEC-FP] log-grep diagnostic once enabled.
//
// Gated on FM2K_SPEC_FINGERPRINT=1 — off by default so the wire stays
// quiet for normal play.

bool SpectatorFingerprint_Enabled() {
    static int s_state = -1;  // 0=off, 1=on
    if (s_state < 0) {
        const char* v = std::getenv("FM2K_SPEC_FINGERPRINT");
        s_state = (v && v[0] == '1' && v[1] == '\0') ? 1 : 0;
    }
    return s_state != 0;
}

uint32_t SpectatorFingerprint_Compute() {
    // Same fields the [HOST-FP]/[SPEC-FP] logs already pin. If we ever add
    // fields, both sides update together — divergent samples would yield
    // a hash mismatch that the spectator catches at runtime.
    constexpr uintptr_t POOL = 0x4701E0;
    constexpr size_t    SLOT = 382;
    struct Sample {
        uint32_t rng;
        uint32_t buf_idx;
        uint32_t p1_hp, p2_hp;
        uint32_t timer;
        int32_t  p1_x, p1_y, p2_x, p2_y;
        int32_t  p1_script, p2_script;
    } s;
    s.rng       = *(uint32_t*)0x41FB1C;
    s.buf_idx   = *(uint32_t*)0x447EE0;
    s.p1_hp     = *(uint32_t*)0x4DFC85;
    s.p2_hp     = *(uint32_t*)0x4EDCC4;
    s.timer     = *(uint32_t*)0x470044;
    s.p1_x      = *(int32_t*)(POOL + 0 * SLOT + 0x08);
    s.p1_y      = *(int32_t*)(POOL + 0 * SLOT + 0x0C);
    s.p2_x      = *(int32_t*)(POOL + 1 * SLOT + 0x08);
    s.p2_y      = *(int32_t*)(POOL + 1 * SLOT + 0x0C);
    s.p1_script = *(int32_t*)(POOL + 0 * SLOT + 0x30);
    s.p2_script = *(int32_t*)(POOL + 1 * SLOT + 0x30);

    // Classic Fletcher-32 over the packed sample. Keep this implementation
    // self-contained (rather than reusing savestate.cpp's xxhash wrapper)
    // so the test mirror in test_spectator_protocol.cpp can match byte-
    // for-byte without pulling xxhash into the test build.
    const uint8_t* data = reinterpret_cast<const uint8_t*>(&s);
    const size_t   len  = sizeof(s);
    uint32_t sum1 = 0xFFFF, sum2 = 0xFFFF;
    size_t i = 0;
    while (i + 1 < len) {
        uint16_t w = (uint16_t)data[i] | ((uint16_t)data[i + 1] << 8);
        sum1 = (sum1 + w)    % 0xFFFFu;
        sum2 = (sum2 + sum1) % 0xFFFFu;
        i += 2;
    }
    if (i < len) {
        uint16_t w = data[i];
        sum1 = (sum1 + w)    % 0xFFFFu;
        sum2 = (sum2 + sum1) % 0xFFFFu;
    }
    return (sum2 << 16) | sum1;
}

void SpectatorNode_AppendMatchStart(const uint8_t header[96]) {
    // Stash the 96-byte header in the side table and reference it by index
    // from the SessionEvent (keeps the in-memory event size at 5 B).
    MatchHeader hdr_copy;
    std::memcpy(hdr_copy.data(), header, hdr_copy.size());
    g_state.match_headers.push_back(hdr_copy);

    SessionEvent ev{};
    ev.type = SessionEventType::MATCH_START;
    ev.u.match_start_idx =
        static_cast<uint16_t>(g_state.match_headers.size() - 1);
    g_state.last_match_start_idx =
        static_cast<int64_t>(g_state.session_events.size());
    AppendOpAndFlush(ev);
}

void SpectatorNode_AppendMatchEnd() {
    SessionEvent ev{};
    ev.type = SessionEventType::MATCH_END;
    AppendOpAndFlush(ev);
}

// =============================================================================
// SESSION FILE FORMAT (C7) — .fm2kset / .fm2krep
// =============================================================================
//
// On-disk layout: [SessionFileHeader (32 B)] [packed SessionEvent[] bytes]
//
// Same wire encoding as EVENT_BATCH payload (SessionEvent_Encode*), so
// loaders can reuse SessionEvent_Decode without an intermediate format.
// Events are self-describing (1-byte tag + variant payload), so no
// separate side table is needed — MATCH_START's 96-byte ReplayHeader is
// inline in the encoded byte stream.

namespace {

constexpr uint32_t SESSION_FILE_MAGIC   = 0x53534D46;  // 'FMSS' little-endian
constexpr uint16_t SESSION_FILE_VERSION = 1;

#pragma pack(push, 1)
struct SessionFileHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t flags;            // bit 0: 1=battle slice, 0=full session
    uint64_t unix_timestamp;
    uint32_t event_count;
    uint32_t input_count;
    uint8_t  reserved[8];
};
static_assert(sizeof(SessionFileHeader) == 32, "SessionFileHeader must be 32 bytes");
#pragma pack(pop)

// Encode events[first..last) into a vector<uint8_t> of packed wire bytes.
// MATCH_START events look up their 96-byte header from the host's
// match_headers side table.
void EncodeEventSliceToBytes(const std::vector<SessionEvent>& events,
                             const std::vector<MatchHeader>& headers,
                             size_t first, size_t last,
                             std::vector<uint8_t>& out_bytes,
                             uint32_t& out_input_count) {
    out_input_count = 0;
    out_bytes.reserve((last - first) * SESSION_EVENT_MAX_WIRE_SIZE);
    for (size_t i = first; i < last; i++) {
        const SessionEvent& ev = events[i];
        AppendEventToWire(out_bytes, ev, headers);
        if (ev.type == SessionEventType::INPUT) ++out_input_count;
    }
}

bool WriteSessionFileImpl(const char* path,
                          const std::vector<SessionEvent>& events,
                          const std::vector<MatchHeader>& headers,
                          size_t first, size_t last,
                          bool is_battle_slice) {
    if (first >= last) return false;
    if (last > events.size()) return false;

    std::vector<uint8_t> body;
    uint32_t input_count = 0;
    EncodeEventSliceToBytes(events, headers, first, last, body, input_count);
    if (body.empty()) return false;

    SessionFileHeader hdr = {};
    hdr.magic           = SESSION_FILE_MAGIC;
    hdr.version         = SESSION_FILE_VERSION;
    hdr.flags           = is_battle_slice ? 1u : 0u;
    hdr.unix_timestamp  = static_cast<uint64_t>(std::time(nullptr));
    hdr.event_count     = static_cast<uint32_t>(last - first);
    hdr.input_count     = input_count;

    FILE* fp = std::fopen(path, "wb");
    if (!fp) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "SpectatorNode: failed to open %s for write", path);
        return false;
    }
    std::fwrite(&hdr, 1, sizeof(hdr), fp);
    std::fwrite(body.data(), 1, body.size(), fp);
    std::fclose(fp);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "SpectatorNode: wrote %s (%u events, %u INPUTs, %zu bytes, %s)",
        path, hdr.event_count, hdr.input_count, sizeof(hdr) + body.size(),
        is_battle_slice ? "battle slice" : "full session");
    return true;
}

} // namespace

bool SpectatorNode_WriteSessionFile(const char* path) {
    return WriteSessionFileImpl(path,
                                g_state.session_events,
                                g_state.match_headers,
                                0, g_state.session_events.size(),
                                /*is_battle_slice=*/false);
}

bool SpectatorNode_WriteCurrentBattleFile(const char* path) {
    if (g_state.last_match_start_idx < 0) return false;
    const size_t first = static_cast<size_t>(g_state.last_match_start_idx);
    const size_t last  = g_state.session_events.size();
    return WriteSessionFileImpl(path,
                                g_state.session_events,
                                g_state.match_headers,
                                first, last,
                                /*is_battle_slice=*/true);
}

bool SpectatorNode_LoadSessionFile(const char* path) {
    FILE* fp = std::fopen(path, "rb");
    if (!fp) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "SpectatorNode: failed to open %s for read", path);
        return false;
    }

    SessionFileHeader hdr = {};
    if (std::fread(&hdr, 1, sizeof(hdr), fp) != sizeof(hdr)) {
        std::fclose(fp);
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "SpectatorNode: %s truncated header", path);
        return false;
    }
    if (hdr.magic != SESSION_FILE_MAGIC) {
        std::fclose(fp);
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "SpectatorNode: %s bad magic 0x%08X", path, hdr.magic);
        return false;
    }
    if (hdr.version > SESSION_FILE_VERSION) {
        std::fclose(fp);
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "SpectatorNode: %s version %u not supported (max %u)",
            path, hdr.version, SESSION_FILE_VERSION);
        return false;
    }

    std::fseek(fp, 0, SEEK_END);
    long total = std::ftell(fp);
    if (total < (long)sizeof(hdr)) {
        std::fclose(fp);
        return false;
    }
    const size_t body_len = static_cast<size_t>(total) - sizeof(hdr);
    std::fseek(fp, sizeof(hdr), SEEK_SET);
    std::vector<uint8_t> body(body_len);
    if (std::fread(body.data(), 1, body_len, fp) != body_len) {
        std::fclose(fp);
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "SpectatorNode: %s body read failed", path);
        return false;
    }
    std::fclose(fp);

    // Fresh playback: clear receiver state, then walk events.
    g_state.pb_queue.clear();
    g_state.pb_match_headers.clear();
    g_state.pb_current_p1 = 0;
    g_state.pb_current_p2 = 0;
    g_state.have_frame_baseline = false;
    g_state.next_expected_frame = 0;
    g_state.playing_back = true;

    size_t off = 0;
    uint32_t pushed_inputs = 0;
    while (off < body_len) {
        SessionEvent ev{};
        uint8_t hdr_buf[SESSION_EVENT_MATCH_HDR_SIZE] = {};
        size_t r = SessionEvent_Decode(body.data() + off, body_len - off,
                                       &ev, hdr_buf);
        if (r == 0) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                "SpectatorNode: %s decode failed at off=%zu (left=%zu)",
                path, off, body_len - off);
            return false;
        }
        if (ev.type == SessionEventType::MATCH_START) {
            MatchHeader hdr_copy;
            std::memcpy(hdr_copy.data(), hdr_buf, hdr_copy.size());
            g_state.pb_match_headers.push_back(hdr_copy);
            ev.u.match_start_idx =
                static_cast<uint16_t>(g_state.pb_match_headers.size() - 1);
        }
        g_state.pb_queue.push_back(ev);
        if (ev.type == SessionEventType::INPUT) ++pushed_inputs;
        off += r;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "SpectatorNode: loaded %s — %u events (%u INPUTs) into pb_queue",
        path, hdr.event_count, pushed_inputs);
    return true;
}

// -----------------------------------------------------------------------------
// SPECTATOR-SIDE OP APPLY (C3)
// -----------------------------------------------------------------------------
//
// Called from PopFrameInputs head-drain loop. Each non-INPUT event at the
// head dispatches here before the next INPUT pops; the local memory write
// happens at the same logical frame the host's write happened, eliminating
// the game_mode-driven mirror race that lived in CheckGameModeTransition.

namespace {

void ApplyResetInputState() {
    // Mirror Netplay_StartBattle's first-call SaveState_Save reset
    // (savestate.cpp:223-237). FM2K addresses; FM95 will need its own
    // mapping if/when spectator support extends to CPW.
    *(uint32_t*)0x447EE0 = 0;            // g_input_buffer_index
    *(uint32_t*)0x4456FC = 0;            // render frame counter
    std::memset((void*)0x447F00, 0, 0x20);    // g_prev_input_state
    std::memset((void*)0x447F40, 0, 0x20);    // g_processed_input
    std::memset((void*)0x447F60, 0, 0x20);    // g_input_changes
    std::memset((void*)0x4280D8, 0, 0x2008);  // input_history rings (P1+P2)
}

void ApplySessionEvent(const SessionEvent& ev) {
    switch (ev.type) {
        case SessionEventType::PIN_RNG:
            *(uint32_t*)0x41FB1C = ev.u.pin_rng_seed;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "SpectatorNode: applied PIN_RNG=0x%08X", ev.u.pin_rng_seed);
            break;
        case SessionEventType::RESET_INPUT_STATE:
            ApplyResetInputState();
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "SpectatorNode: applied RESET_INPUT_STATE");
            break;
        case SessionEventType::SOUND_INIT:
            SoundRollback::Init();
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "SpectatorNode: applied SOUND_INIT");
            break;
        case SessionEventType::MATCH_START: {
            // Look up the cached 96-byte header by side-table index.
            // Header layout matches Replay::ReplayHeader on-disk; pull
            // seed/state-hash/char/color and re-publish into the playback
            // metadata so any UI consumers (HUD, replay loader handoff)
            // see the live values.
            if (ev.u.match_start_idx < g_state.pb_match_headers.size()) {
                const uint8_t* h = g_state.pb_match_headers[ev.u.match_start_idx].data();
                uint32_t seed = 0, state_hash = 0;
                std::memcpy(&seed,       h + 20, 4);
                std::memcpy(&state_hash, h + 24, 4);
                g_state.pb_initial_seed  = seed;
                g_state.pb_initial_state = state_hash;
                g_state.pb_p1_char       = h[28];
                g_state.pb_p1_color      = h[29];
                g_state.pb_p2_char       = h[30];
                g_state.pb_p2_color      = h[31];
                // Mirror the legacy INITIAL_MATCH packet path so the
                // initial-match cache stays valid for relay-to-sub-spectator.
                std::memcpy(g_state.initial_match.header_bytes, h, 96);
                g_state.initial_match.valid = true;
            }
            g_state.playing_back = true;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "SpectatorNode: applied MATCH_START seed=0x%08X p1=%u/%u p2=%u/%u",
                g_state.pb_initial_seed,
                g_state.pb_p1_char, g_state.pb_p1_color,
                g_state.pb_p2_char, g_state.pb_p2_color);
            break;
        }
        case SessionEventType::MATCH_END:
            // Don't clear pb_queue — let queued post-MATCH_END frames drain
            // (they render the final battle frames). The next MATCH_START
            // resets metadata and flips playing_back back on.
            g_state.playing_back = false;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "SpectatorNode: applied MATCH_END (queued frames=%zu)",
                g_state.pb_queue.size());
            break;
        case SessionEventType::FINGERPRINT: {
            // C9: diagnostic mismatch detection. Host emits its hash here;
            // spectator computes the same hash on its current state and
            // compares. Drain-at-head ordering ensures both sides sample
            // at the same logical frame.
            if (SpectatorFingerprint_Enabled()) {
                const uint32_t host_hash = ev.u.fingerprint_hash;
                const uint32_t local_hash = SpectatorFingerprint_Compute();
                if (host_hash != local_hash) {
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[SPEC-FP-MISMATCH] host=0x%08X spectator=0x%08X — "
                        "DESYNC at this logical frame (next INPUT is the "
                        "first divergent sim step)",
                        host_hash, local_hash);
                } else {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[SPEC-FP-OK] host=0x%08X (matches local)", host_hash);
                }
            }
            break;
        }
        case SessionEventType::INPUT:
            // Should not reach here — INPUT is handled by the pop path in
            // PopFrameInputs, not the drain.
            break;
    }
}

} // namespace

// -----------------------------------------------------------------------------
// JOIN / REDIRECT
// -----------------------------------------------------------------------------

void SpectatorNode_HandleJoinReq(const sockaddr_in& from) {
    char addr_buf[48] = {};
    FormatAddr(from, addr_buf, sizeof(addr_buf));

    // Helper: build SPEC_JOIN_ACK with the host's current session kind so
    // the spectator knows which GekkoSpectateSession config to create.
    auto BuildJoinAck = []() {
        CtrlPacket ack = {};
        ack.header.type = CtrlMsg::SPEC_JOIN_ACK;
        ack.data.spec_join_ack.host_session_kind =
            static_cast<uint8_t>(Netplay_GetSessionKind());
        // Tell the spectator which TCP port to dial for the INPUT_BATCH
        // stream. Zero would mean the listener failed at startup, in which
        // case the spectator refuses the subscription.
        ack.data.spec_join_ack.host_tcp_port = SpectatorTCP::GetListenPort();
        return ack;
    };

    // Helper: if there's a live GekkoNet session on this node (player slot),
    // add the joining spectator as a GekkoSpectator actor so confirmed-input
    // events and (battle) Save/Load events reach them natively. Both CSS
    // and BATTLE sessions get spectators added — CSS doesn't emit Save/Load
    // (lockstep suppresses them) but it does emit GekkoAdvanceEvent per
    // confirmed frame, and that's the source of truth that drives the
    // spectator's local sim 1:1 with the host.
    auto AddSpectatorToSession = [](const sockaddr_in& spec_from) {
        const NetplaySessionKind k = Netplay_GetSessionKind();
        if (k != NetplaySessionKind::CSS && k != NetplaySessionKind::BATTLE) {
            return;
        }
        char ip_str[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, (void*)&spec_from.sin_addr, ip_str, sizeof(ip_str));
        char addr_str[64];
        snprintf(addr_str, sizeof(addr_str), "%s:%u",
                 ip_str, ntohs(spec_from.sin_port));
        GekkoSession* sess = Netplay_GetActiveSession();
        if (!sess) return;
        GekkoNetAddress addr = {};
        addr.data = (void*)addr_str;
        addr.size = (int)strlen(addr_str);
        gekko_add_actor(sess, GekkoSpectator, &addr);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "SpectatorNode: Late-joiner added as GekkoSpectator on %s session -> %s",
            k == NetplaySessionKind::CSS ? "CSS" : "BATTLE",
            addr_str);
    };

    // Already subscribed? Idempotent ACK. INITIAL_MATCH/backfill are
    // deferred until the spectator's TCP socket binds (see TickHealth).
    for (const auto& sub : g_state.subscribers) {
        if (AddrEqual(sub.addr, from)) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "SpectatorNode: JOIN_REQ from existing subscriber %s — re-ACKing",
                        addr_buf);
            CtrlPacket ack = BuildJoinAck();
            ControlChannel_SendTo(ack, from);
            return;
        }
    }

    if (g_state.subscribers.size() < g_state.capacity) {
        Subscriber sub = {};
        sub.addr         = from;
        sub.last_seen_ms = GetTickCount64();
        sub.ack_frame    = 0;
        sub.tcp_bound    = false;
        g_state.subscribers.push_back(sub);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: Accepted subscriber %s (%zu/%zu) — "
                    "deferring INITIAL_MATCH until TCP binds",
                    addr_buf, g_state.subscribers.size(), g_state.capacity);

        CtrlPacket ack = BuildJoinAck();
        ControlChannel_SendTo(ack, from);

        // If we already have a live GekkoNet session, add this late joiner
        // as a GekkoSpectator actor so the input stream reaches them.
        AddSpectatorToSession(from);

        // INITIAL_MATCH + SendSessionBackfillTo are sent by TickHealth's
        // TryBindPendingTCP path the first time the spectator's accepted
        // TCP connection gets paired with this subscriber slot.
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
            SpectatorTCP::DisconnectSubscriber(it->addr);
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

std::vector<sockaddr_in> SpectatorNode_GetSubscriberAddrs() {
    std::vector<sockaddr_in> out;
    out.reserve(g_state.subscribers.size());
    for (const auto& sub : g_state.subscribers) {
        out.push_back(sub.addr);
    }
    return out;
}

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

void SpectatorNode_HandleJoinAck(const sockaddr_in& from, uint8_t host_session_kind,
                                 uint16_t host_tcp_port) {
    // SPEC_JOIN_ACK is dual-purpose:
    //   1. First-time arrival (initial subscribe): completes the JOIN_REQ
    //      handshake, pins RNG, marks subscribed_upstream, opens TCP up.
    //   2. Re-broadcast on host session-kind change (host crosses a
    //      session boundary like CSS->battle or first-CSS-create): the
    //      payload's host_session_kind tells the spectator which kind to
    //      mirror. Treated as authoritative current-host-kind.
    //
    // Both paths funnel through the same handler so the spectator can
    // recover cleanly if the host transitions before the JOIN_REQ ack
    // round-trip lands. Stray ACKs from non-upstream peers are still
    // dropped (sender addr check below).
    const bool first_time = g_state.join_req_pending;

    if (!first_time && !AddrEqual(g_state.upstream_addr, from)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: ignoring SPEC_JOIN_ACK from non-upstream peer");
        return;
    }

    g_state.join_req_pending      = false;
    g_state.upstream_addr         = from;
    g_state.subscribed_upstream   = true;

    // RNG sync + queue clear: ONLY apply on FIRST-TIME subscribe (spectator
    // is still at title/pre-CSS, no game state to lose). On reconnect-after-
    // silence-failover or on re-broadcast ACK, spectator's local sim is
    // mid-match — clobbering RNG / wiping the queue would erase in-progress
    // state. Gate both on first-time only.
    if (first_time) {
        *(uint32_t*)0x41FB1C = 0x12345678;
        g_state.pb_queue.clear();
        g_state.pb_current_p1 = 0;
        g_state.pb_current_p2 = 0;
    }
    g_state.playing_back = true;

    // Dial the host's TCP port. Bulk INPUT_BATCH / INITIAL_MATCH /
    // MATCH_END flow over TCP exclusively; if the host didn't advertise
    // a port, this peer has nothing to listen to and the subscription
    // is unusable. Bail.
    if (host_tcp_port == 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "SpectatorNode: JOIN_ACK from host has no TCP port — refusing");
        g_state.subscribed_upstream = false;
        return;
    }
    {
        char host_ip[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, (void*)&from.sin_addr, host_ip, sizeof(host_ip));
        if (!SpectatorTCP::ConnectUpstream(host_ip, host_tcp_port)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                "SpectatorNode: SpectatorTCP::ConnectUpstream(%s:%u) failed",
                host_ip, (unsigned)host_tcp_port);
            g_state.subscribed_upstream = false;
            return;
        }
    }

    char buf[48] = {}; FormatAddr(from, buf, sizeof(buf));
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "SpectatorNode: JOIN_ACK from %s (host_kind=%u, tcp_port=%u) — subscribed",
                buf, (unsigned)host_session_kind, (unsigned)host_tcp_port);

    // host_session_kind == 0 (NONE): host has no session active yet
    // (e.g. JOIN_REQ landed during host's title-skip phase before its
    // first CSS session was created). DO NOT create a SpectateSession
    // with a guessed config — that caused config mismatch deadlocks
    // before. Wait for host's follow-up SPEC_JOIN_ACK after session create.
    if (host_session_kind == 0) {
        char buf2[48] = {}; FormatAddr(from, buf2, sizeof(buf2));
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: host has no session yet — waiting (from=%s)", buf2);
        return;
    }

    NetplaySessionKind kind = (host_session_kind == 1)
        ? NetplaySessionKind::CSS
        : NetplaySessionKind::BATTLE;

    char host_addr_str[64];
    snprintf(host_addr_str, sizeof(host_addr_str), "%s:%u",
             inet_ntoa(from.sin_addr), ntohs(from.sin_port));

    // If a SpectateSession is already alive, only act on a kind CHANGE.
    // Same-kind re-broadcasts (host re-acks for liveness / new spectator
    // joining the same session) are no-ops via Netplay_StartSpectateSession's
    // own idempotency guard.
    const NetplaySessionKind current = Netplay_GetSessionKind();
    if (current == NetplaySessionKind::SPECTATE) {
        // Wrong-kind alive — treat as a phase swap. swap_frame=0 fires
        // immediately on the next AdvanceEvent.
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: host kind changed mid-session, requesting swap to kind=%d",
                    (int)kind);
        if (kind == NetplaySessionKind::BATTLE) {
            Netplay_OnHostBattleEntering(0);
        } else {
            Netplay_OnHostBattleEnd(0);
        }
        return;
    }

    Netplay_StartSpectateSession(kind, host_addr_str);
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
        case SpecDataType::INITIAL_MATCH:
        case SpecDataType::INPUT_BATCH:
        case SpecDataType::INPUT_REQUEST:
            // Legacy wire types — retired in C12. Production hosts ship
            // MATCH_START / MATCH_END / FINGERPRINT / per-INPUT events
            // inside EVENT_BATCH datagrams. The enum values stay in
            // spectator_node.h for ABI but receivers no longer act on them
            // (silently drop instead of warn — old peers shouldn't exist
            // in practice and a stale packet during a hub-driven session
            // swap shouldn't spam the log).
            break;
        case SpecDataType::MATCH_END:
            // Same: legacy stand-alone MATCH_END packet retired. The
            // SessionEvent::MATCH_END op flowing through EVENT_BATCH
            // handles match-end on the new path.
            break;
        case SpecDataType::EVENT_BATCH: {
            // Primary C2+ ingest. Payload is a packed SessionEvent[] stream
            // (1-byte tag + variant payload per event). Walk the stream
            // sequentially; for INPUT events apply the contiguous-frame
            // dedup gate; for non-INPUT events push if and only if the
            // batch as a whole is at-or-after next_expected_frame (i.e. we
            // haven't already consumed past the INPUTs they precede).
            //
            // Receive-side gate: subscription is the only thing that
            // matters here. `playing_back` was the legacy
            // "between INITIAL_MATCH and MATCH_END" state — but
            // MATCH_END now arrives in-band as an event and flips
            // playing_back=false at apply time (drain-at-head). If we
            // gated receive on playing_back we'd drop the CSS frames
            // + next-match MATCH_START that flow through the same
            // EVENT_BATCH stream right after MATCH_END.
            if (!g_state.subscribed_upstream) return;

            const uint32_t expected_at_entry = g_state.have_frame_baseline
                ? g_state.next_expected_frame : 0xFFFFFFFFu;

            // Establish baseline on first batch.
            if (!g_state.have_frame_baseline) {
                g_state.have_frame_baseline = true;
                g_state.next_expected_frame = hdr.start_frame;
            }

            // If the batch's first INPUT is already behind the cursor, the
            // whole batch is a redundant retransmit (e.g. backfill arriving
            // after live frames during the C5 race) — skip it.
            const bool batch_already_consumed =
                hdr.start_frame + hdr.frame_count <= g_state.next_expected_frame;
            if (batch_already_consumed) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: skipping fully-consumed EVENT_BATCH "
                    "(start=%u count=%u, expected=%u)",
                    hdr.start_frame, hdr.frame_count, expected_at_entry);
                break;
            }

            size_t off = 0;
            uint32_t  cursor_input  = hdr.start_frame;  // next INPUT in this batch
            uint32_t  pushed_inputs = 0;
            uint32_t  skipped_inputs = 0;
            uint32_t  gap_first      = 0xFFFFFFFFu;
            while (off < payload_len) {
                SessionEvent ev{};
                uint8_t hdr_buf[SESSION_EVENT_MATCH_HDR_SIZE];
                size_t consumed = SessionEvent_Decode(payload + off, payload_len - off,
                                                      &ev, hdr_buf);
                if (consumed == 0) {
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "SpectatorNode: EVENT_BATCH decode failed at off=%zu (len=%zu)",
                        off, payload_len);
                    break;
                }

                if (ev.type == SessionEventType::INPUT) {
                    if (cursor_input < g_state.next_expected_frame) {
                        ++skipped_inputs;          // already consumed
                    } else if (cursor_input > g_state.next_expected_frame) {
                        if (gap_first == 0xFFFFFFFFu) gap_first = cursor_input;
                    } else {
                        g_state.pb_queue.push_back(ev);
                        g_state.next_expected_frame = cursor_input + 1;
                        ++pushed_inputs;
                        // Hop-1 relay.
                        SpectatorNode_OnFrameConfirmed(ev.u.input.p1, ev.u.input.p2);
                    }
                    ++cursor_input;
                } else {
                    // Non-INPUT event. Apply when its position (= cursor_input,
                    // i.e. the index of the next INPUT) is at or after the
                    // current dedup cursor — meaning the INPUT it logically
                    // precedes hasn't been consumed yet. Otherwise the op is
                    // stale (the spectator has already executed past that
                    // boundary in a previous batch).
                    if (cursor_input >= g_state.next_expected_frame) {
                        if (ev.type == SessionEventType::MATCH_START) {
                            // Stash header in the side table so the playback
                            // driver can look it up by match_start_idx.
                            MatchHeader hdr_copy;
                            std::memcpy(hdr_copy.data(), hdr_buf, hdr_copy.size());
                            g_state.pb_match_headers.push_back(hdr_copy);
                            ev.u.match_start_idx =
                                static_cast<uint16_t>(g_state.pb_match_headers.size() - 1);
                        }
                        g_state.pb_queue.push_back(ev);

                        // Hop-1 relay: forward non-INPUT ops to sub-spectators
                        // via the same Append* helpers the host uses. AppendOp
                        // pushes to local session_events + flushes if we have
                        // subscribers. INPUT relay is handled separately above
                        // via SpectatorNode_OnFrameConfirmed.
                        switch (ev.type) {
                            case SessionEventType::PIN_RNG:
                                SpectatorNode_AppendPinRng(ev.u.pin_rng_seed);
                                break;
                            case SessionEventType::RESET_INPUT_STATE:
                                SpectatorNode_AppendResetInputState();
                                break;
                            case SessionEventType::SOUND_INIT:
                                SpectatorNode_AppendSoundInit();
                                break;
                            case SessionEventType::FINGERPRINT:
                                SpectatorNode_AppendFingerprint(ev.u.fingerprint_hash);
                                break;
                            case SessionEventType::MATCH_START:
                                // hdr_buf was populated by SessionEvent_Decode;
                                // re-append on this relay node so its sub-
                                // spectators see the same MATCH_START in
                                // their own backfill.
                                SpectatorNode_AppendMatchStart(hdr_buf);
                                break;
                            case SessionEventType::MATCH_END:
                                SpectatorNode_AppendMatchEnd();
                                break;
                            default:
                                break;
                        }
                    }
                }
                off += consumed;
            }
            if (gap_first != 0xFFFFFFFFu) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: EVENT_BATCH out-of-order INPUT %u (expected=%u, "
                    "batch start=%u count=%u, pushed=%u skipped=%u)",
                    gap_first, expected_at_entry,
                    hdr.start_frame, hdr.frame_count, pushed_inputs, skipped_inputs);
            }
            static uint32_t last_log_tick = 0;
            uint32_t now = (uint32_t)GetTickCount64();
            if (now - last_log_tick > 1000) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: pb_queue=%zu (batch inputs=%u, start=%u, subs=%zu)",
                    g_state.pb_queue.size(), hdr.frame_count, hdr.start_frame,
                    g_state.subscribers.size());
                last_log_tick = now;
            }
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
    // Drain non-INPUT events from the head before popping the next INPUT.
    // Each non-INPUT event dispatches to ApplySessionEvent — RNG pin,
    // input-state reset, sound dedup init, etc. The dispatch happens at
    // the moment the spectator's local sim is about to consume the next
    // INPUT, which is the same logical-frame moment the host's pin
    // happened. Eliminates the game_mode-driven mirror race.
    while (!g_state.pb_queue.empty() &&
           g_state.pb_queue.front().type != SessionEventType::INPUT) {
        ApplySessionEvent(g_state.pb_queue.front());
        g_state.pb_queue.erase(g_state.pb_queue.begin());
    }
    if (g_state.pb_queue.empty()) return false;
    if (g_state.pb_queue.front().type != SessionEventType::INPUT) return false;

    const SessionEvent ev = g_state.pb_queue.front();
    g_state.pb_queue.erase(g_state.pb_queue.begin());
    g_state.pb_current_p1 = ev.u.input.p1;
    g_state.pb_current_p2 = ev.u.input.p2;
    if (p1_input) *p1_input = ev.u.input.p1;
    if (p2_input) *p2_input = ev.u.input.p2;
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

        // (Removed: periodic INPUT_REQUEST poll — under TCP the kernel
        // handles retransmit transparently, and there's no host-side
        // RespondToInputRequest anymore.)

        // Silence-based failover. TCP receive activity is the only
        // liveness signal — the bulk stream lives entirely there.
        //
        // Suppressed when:
        //   (a) Catchup is active. The catchup loop runs sim+disk-IO at
        //       full speed and may legitimately go quiet for seconds —
        //       especially during CSS replay when each cursor move blocks
        //       on a .player file load. Tearing down here would force a
        //       backfill replay that re-incurs the same cost, never
        //       finishing.
        //   (b) pb_queue still has events to drain. recv_ms only updates
        //       on NEW TCP bytes; if the host already sent us 16k events
        //       of backfill in one burst and we're slowly chewing through
        //       them, recv_ms goes stale even though we're actively
        //       processing data. The previous gate (a alone) wasn't
        //       enough — once initial catchup ended (s_initial_catchup_done
        //       latched true), reconnect-and-backfill paths bypassed
        //       catchup entirely and the failover here would fire mid-
        //       drain → tear down the connection we're using → reconnect
        //       → re-receive backfill → same cycle. Death loop observed
        //       in P3 logs after a host network blip.
        extern bool g_spectator_catchup;
        const uint64_t recv_ms     = SpectatorTCP::LastUpstreamRecvMs();
        const bool     queue_idle  = SpectatorNode_PendingFrameCount() == 0;
        if (!g_spectator_catchup &&
            queue_idle &&
            recv_ms > 0 &&
            now - recv_ms >= SPECTATOR_SILENCE_FAILOVER_MS)
        {
            char buf[48] = {}; FormatAddr(g_state.upstream_addr, buf, sizeof(buf));
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "SpectatorNode: upstream %s silent for %llu ms — failing over",
                        buf,
                        (unsigned long long)(now - recv_ms));
            CtrlPacket leave = {};
            leave.header.type = CtrlMsg::SPEC_LEAVE;
            ControlChannel_SendTo(leave, g_state.upstream_addr);
            g_state.subscribed_upstream = false;
            SpectatorTCP::DisconnectUpstream();
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

    SpectatorNode_TickHostMaintenance();
}

void SpectatorNode_TickHostMaintenance() {
    const uint64_t now = (uint64_t)GetTickCount64();

    // ---- Upstream-side: bind newly-arrived TCP clients to subscribers ----
    // Spectator's async TCP dial completes some frames after JOIN_ACK; the
    // accept queue carries a fresh socket waiting to be paired by IP. On
    // first successful pair, ship INITIAL_MATCH + backfill — those bytes
    // were deferred at JOIN_REQ accept time because the socket didn't
    // exist yet.
    for (auto& sub : g_state.subscribers) {
        if (sub.tcp_bound) continue;
        if (SpectatorTCP::RegisterAcceptedClient(sub.addr)) {
            sub.tcp_bound = true;
            char buf[48] = {}; FormatAddr(sub.addr, buf, sizeof(buf));
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "SpectatorNode: TCP bound for %s — sending INITIAL_MATCH + backfill",
                        buf);
            // C5 backfill ordering fence:
            //   1. SendSessionBackfillTo iterates session_events, encoding
            //      every event (including the historic MATCH_START at the
            //      head of the current match) into chunked EVENT_BATCH
            //      datagrams. Refreshes sub.last_seen_ms per chunk so the
            //      SUBSCRIBER_EXPIRY_MS sweep can't reap mid-backfill.
            //   2. MarkBackfillComplete flips the TCP-layer fence so future
            //      BroadcastToAll calls finally include this subscriber.
            // Until step 2 fires, BroadcastToAll skips this sub — any live
            // FlushBatch firing in this gap is silently elided for this sub
            // and the sub catches up via the backfill chunks instead.
            SendSessionBackfillTo(sub.addr);
            sub.last_seen_ms = now;            // post-backfill liveness anchor
            SpectatorTCP::MarkBackfillComplete(sub.addr);
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "SpectatorNode: backfill complete for %s — live broadcasts engaged",
                        buf);
        }
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
            SpectatorTCP::DisconnectSubscriber(it->addr);
            it = g_state.subscribers.erase(it);
        } else {
            ++it;
        }
    }
}
