// Spectator-side incoming data dispatch: HandleSpecData (EVENT_BATCH / SNAPSHOT_* /
// OP_BASELINE decode -> playback queue) and HandleUdpInputDatagram (admission-gated
// UDP input fast path). Extracted VERBATIM from spectator_node.cpp. Public API
// (decls in spectator_node.h); reaches specnode helpers via using.
#include "spectator_node.h"
#include "spectator_node_internal.h"  // shared State model + g_state (split for sibling TUs)
#include "spec_wire.h"            // zero-RLE codec (SessionEvent_* live in spectator_node.h)
#include "spec_relay_queue.h"     // hub-relay outbound queue (Phase 2c)
#include "spectator_tcp.h"        // TCP transport for INPUT_BATCH stream
#include "spec_impair.h"          // test-only spectator-downlink loss (FM2K_SPEC_DROP)
#include "control_channel.h"
#include "netplay.h"
#include "replay.h"
#include "savestate.h"            // SaveState_Save / Peek for snapshot capture
#include "netplay_state.h"
#include "../audio/sound_rollback.h"  // Op apply: SOUND_INIT
#include "../hooks/css_autoconfirm.h" // Replay-mode CSS lock-and-confirm
#include "../hooks/per_game_patches.h" // PerGamePatches_SetRuntimeBtbOverrides
#include "../ui/shared_mem.h"         // C10: SharedMem_PublishMatchSession / RoundResult
#include "gekkonet.h"

#include <SDL3/SDL_log.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <array>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>
#include <cstring>
#include <cstdlib>
#include <cstdio>
using namespace specnode;

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
        case SpecDataType::SNAPSHOT_BEGIN: {
            // CURRENT_MATCH-mode bind path opener. Wire layout (see
            // SendSnapshotTo): hdr.start_frame = anchor INPUT-frame for
            // the post-snapshot event stream, hdr.flags = sizeof(meta) =
            // 16, payload = SnapshotMetadata.
            // v1 meta = 16 bytes (no flags/compressed_bytes); v2 = 20.
            constexpr size_t META_V1_BYTES = 16;
            if (payload_len < META_V1_BYTES) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: SNAPSHOT_BEGIN truncated (payload=%zu, need %zu)",
                    payload_len, META_V1_BYTES);
                g_state.pb_snapshot_inbox = State::SnapshotInbox{};
                break;
            }
            SnapshotMetadata meta = {};
            std::memcpy(&meta, payload,
                        payload_len >= sizeof(SnapshotMetadata)
                            ? sizeof(SnapshotMetadata) : META_V1_BYTES);
            if (meta.version == 1) {
                // v1 sender: uncompressed, wire size == total.
                meta.flags            = 0;
                meta.compressed_bytes = meta.total_bytes;
            } else if (meta.version != SPECTATOR_SNAPSHOT_VERSION) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: SNAPSHOT_BEGIN version mismatch "
                    "(got %u, want <= %u) — dropping snapshot, peer must "
                    "rejoin with FULL_SESSION",
                    meta.version, SPECTATOR_SNAPSHOT_VERSION);
                g_state.pb_snapshot_inbox = State::SnapshotInbox{};
                break;
            }
            const uint32_t wire_bytes =
                (meta.flags & SNAPSHOT_FLAG_ZERO_RLE) ? meta.compressed_bytes
                                                      : meta.total_bytes;
            if (wire_bytes == 0 || wire_bytes > meta.total_bytes) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: SNAPSHOT_BEGIN bad wire size %u (total %u)",
                    wire_bytes, meta.total_bytes);
                g_state.pb_snapshot_inbox = State::SnapshotInbox{};
                break;
            }

            const size_t expected_slot_size = SaveState_GetSlotByteSize();
            if (meta.total_bytes == 0 ||
                meta.total_bytes != expected_slot_size) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: SNAPSHOT_BEGIN size mismatch "
                    "(blob=%u, slot=%zu) — engine variant mismatch?",
                    meta.total_bytes, expected_slot_size);
                g_state.pb_snapshot_inbox = State::SnapshotInbox{};
                break;
            }

            auto& inbox = g_state.pb_snapshot_inbox;
            if (inbox.active) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: SNAPSHOT_BEGIN restart "
                    "(prior inbox had %zu/%u bytes, dropping)",
                    inbox.bytes_received, inbox.meta.total_bytes);
            }
            inbox.meta           = meta;
            inbox.anchor_frame   = hdr.start_frame;
            inbox.bytes_received = 0;
            inbox.blob.assign(wire_bytes, 0);   // wire (possibly compressed) bytes
            inbox.active         = true;

            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "SpectatorNode: SNAPSHOT_BEGIN match=%u, %u bytes, "
                "anchor INPUT-frame=%u, captured_game_mode=%u",
                meta.match_index, meta.total_bytes, inbox.anchor_frame,
                meta.captured_game_mode);

            // Battle-snapshot fast-walk for spec: when the snapshot is
            // captured at battle (game_mode=3000) but spec is still in
            // boot/title/CSS, the pb_queue starts at a post-CSS frame
            // and contains no CSS-confirm inputs. CssAutoConfirm gives
            // us programmatic "pick chars + lock in" so the spec's
            // local engine can transition CSS → battle on its own.
            // Targets are arbitrary (0/0/stage 0); the snapshot apply
            // at game_mode=3000 overwrites the entire char-data pool
            // with the host's actual matchup state.
            //
            // For CSS snapshots (captured_game_mode=2000), DON'T arm —
            // the snapshot itself will populate cursor positions +
            // selected chars when it applies at game_mode==2000, and
            // CssAutoConfirm's pinning logic would fight the loaded
            // state.
            if (g_spectator_mode && meta.captured_game_mode == 3000u) {
                CssAutoConfirm_OnReplayMatchStart(
                    /*p1_char=*/0, /*p1_color=*/0,
                    /*p2_char=*/0, /*p2_color=*/0,
                    /*stage_id=*/0);
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: armed CssAutoConfirm for spec "
                    "(battle snapshot — drive CSS forward to game_mode "
                    "3000 with placeholder chars; snapshot apply will "
                    "overwrite)");
            }
            break;
        }
        case SpecDataType::SNAPSHOT_CHUNK: {
            // hdr.start_frame = byte offset, hdr.flags = chunk byte count.
            auto& inbox = g_state.pb_snapshot_inbox;
            if (!inbox.active) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: SNAPSHOT_CHUNK without active inbox — dropping");
                break;
            }
            const size_t   off     = hdr.start_frame;
            const size_t   chunk_n = hdr.flags;
            if (chunk_n == 0 || chunk_n > payload_len) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: SNAPSHOT_CHUNK bad length (flags=%zu, payload=%zu)",
                    chunk_n, payload_len);
                inbox = State::SnapshotInbox{};
                break;
            }
            if (off + chunk_n > inbox.blob.size()) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: SNAPSHOT_CHUNK overruns blob "
                    "(off=%zu chunk=%zu, total=%zu)",
                    off, chunk_n, inbox.blob.size());
                inbox = State::SnapshotInbox{};
                break;
            }
            std::memcpy(inbox.blob.data() + off, payload, chunk_n);
            inbox.bytes_received += chunk_n;
            break;
        }
        case SpecDataType::SNAPSHOT_END: {
            // hdr.flags = 4, payload = uint32 fletcher32 over the host's blob.
            auto& inbox = g_state.pb_snapshot_inbox;
            if (!inbox.active) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: SNAPSHOT_END without active inbox — dropping");
                break;
            }
            if (payload_len < sizeof(uint32_t)) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: SNAPSHOT_END truncated (payload=%zu)",
                    payload_len);
                inbox = State::SnapshotInbox{};
                break;
            }
            uint32_t expected_checksum = 0;
            std::memcpy(&expected_checksum, payload, sizeof(uint32_t));

            const uint32_t expected_wire =
                (inbox.meta.flags & SNAPSHOT_FLAG_ZERO_RLE)
                    ? inbox.meta.compressed_bytes : inbox.meta.total_bytes;
            if (inbox.bytes_received != expected_wire) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: SNAPSHOT_END byte-count mismatch "
                    "(received %zu, want %u) — discarding",
                    inbox.bytes_received, expected_wire);
                inbox = State::SnapshotInbox{};
                break;
            }
            if (inbox.meta.flags & SNAPSHOT_FLAG_ZERO_RLE) {
                std::vector<uint8_t> raw(inbox.meta.total_bytes);
                if (!ZeroRleDecompress(inbox.blob.data(), inbox.blob.size(),
                                       raw.data(), raw.size())) {
                    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                        "SpectatorNode: SNAPSHOT_END zero-RLE decompress "
                        "failed (%zu wire -> %u raw) — discarding",
                        inbox.blob.size(), inbox.meta.total_bytes);
                    inbox = State::SnapshotInbox{};
                    break;
                }
                inbox.blob.swap(raw);
            }
            const uint32_t local_checksum =
                Fletcher32(inbox.blob.data(), inbox.blob.size());
            if (local_checksum != expected_checksum) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: SNAPSHOT_END checksum mismatch "
                    "(local=0x%08X, expected=0x%08X) — discarding",
                    local_checksum, expected_checksum);
                inbox = State::SnapshotInbox{};
                break;
            }

            // Validation passed. DON'T apply SaveState_LoadFromBytes yet —
            // the spectator's local game may still be in pre-WinMain init
            // (game_mode == 0). Smashing battle-state bytes into uninit'd
            // engine memory crashes the next frame's render. Mark the inbox
            // pending_apply; ApplyPendingSnapshot polls each spec tick and
            // runs the actual apply once the engine has progressed past
            // mode 0. inbox.blob + meta + anchor stay populated until then.
            inbox.pending_apply = true;
            inbox.active        = false;  // assembly is done; no more chunks

            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "SpectatorNode: SNAPSHOT_END validated (match=%u, "
                "%zu bytes, fletcher32=0x%08X) — anchor INPUT-frame=%u "
                "(deferred apply pending game-mode != 0)",
                inbox.meta.match_index, inbox.blob.size(),
                local_checksum, inbox.anchor_frame);
            break;
        }
        case SpecDataType::OP_BASELINE: {
            // Phase F: op-count baseline for this connection. Seeds
            // ops_seen with the count of non-INPUT events appended before
            // this connection's backfill start (which we will never
            // receive), and (re-)arms the UDP admission epoch. Sent by the
            // host's bind path BEFORE snapshot/backfill, so by TCP
            // ordering it always precedes the first EVENT_BATCH of the
            // fresh connection.
            if (!g_state.subscribed_upstream) return;
            if (payload_len >= 4) {
                uint32_t baseline = 0;
                std::memcpy(&baseline, payload, 4);
                g_state.conn_ops_baseline = baseline;
                g_state.conn_ops_decoded  = 0;
                g_state.tcp_rejoin_pending = false;
                if (baseline > g_state.ops_seen) {
                    g_state.ops_seen = baseline;
                }
                g_state.udp_epoch_armed = SpecUdpEnabled();
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: OP_BASELINE=%u received -- UDP "
                    "admission %s (ops_seen=%u)", baseline,
                    g_state.udp_epoch_armed ? "armed" : "disabled by env",
                    g_state.ops_seen);
            }
            break;
        }
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
            g_state.live_established = true;

            const uint32_t expected_at_entry = g_state.have_frame_baseline
                ? g_state.next_expected_frame : 0xFFFFFFFFu;

            // Establish baseline on first batch.
            if (!g_state.have_frame_baseline) {
                g_state.have_frame_baseline = true;
                g_state.next_expected_frame = hdr.start_frame;
                // [ANCHOR] proof-first instrumentation (task: in-battle-select
                // divergence). Logs WHICH batch's start_frame the cursor
                // latched to -- a live battle batch beating backfill here
                // anchors the whole session at a wrong absolute frame.
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[ANCHOR] baseline-latch start_frame=%u frame_count=%u",
                    hdr.start_frame, hdr.frame_count);
            }

            // NOTE: there is deliberately NO "fully-consumed batch" early
            // skip here. AppendOpAndFlush emits op-only batches with
            // frame_count=0 at the live edge (MATCH_END / MATCH_START use
            // the flush-then-append pattern, so the boundary ops always
            // ride such a batch). For a caught-up spectator,
            // start_frame + 0 <= next_expected_frame held, and the old
            // skip silently dropped MATCH_END and the next MATCH_START --
            // the spectator never followed into match 2. The per-event
            // walk below already dedups INPUTs positionally and gates ops
            // on cursor_input >= next_expected_frame, so redundant batches
            // cost ~8 decodes and nothing else.

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
                        SpectatorNode_StampInputAdmit();
                        g_state.next_expected_frame = cursor_input + 1;
                        ++pushed_inputs;
                        // Hop-1 relay.
                        SpectatorNode_OnFrameConfirmed(ev.u.input.p1, ev.u.input.p2);
                    }
                    ++cursor_input;
                } else {
                    // Phase F: op identity = global index. The connection
                    // delivers ops in order from its baseline; ops the UDP
                    // tail already accepted are duplicates -- skip their
                    // push/relay entirely (a second MATCH_START apply
                    // would corrupt the boundary).
                    const uint32_t conn_op_idx =
                        g_state.conn_ops_baseline + g_state.conn_ops_decoded;
                    ++g_state.conn_ops_decoded;
                    if (conn_op_idx < g_state.ops_seen) {
                        off += consumed;
                        continue;  // duplicate of a UDP-accepted op
                    }
                    g_state.ops_seen = conn_op_idx + 1;

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
                                SpectatorNode_AppendMatchEnd(ev.u.match_end);
                                break;
                            case SessionEventType::SESSION_ID:
                                SpectatorNode_AppendSessionId(ev.u.session_id);
                                break;
                            case SessionEventType::CSS_ENTERED:
                                SpectatorNode_AppendCssEntered();
                                break;
                            case SessionEventType::ROUND_START: {
                                const auto& p = ev.u.round_start;
                                SpectatorNode_AppendRoundStart(
                                    p.round_idx, p.p1_hp_max, p.p2_hp_max,
                                    p.timer_seconds);
                                break;
                            }
                            case SessionEventType::ROUND_END: {
                                const auto& p = ev.u.round_end;
                                SpectatorNode_AppendRoundEnd(
                                    p.winner_idx, p.p1_hp_remaining,
                                    p.p2_hp_remaining);
                                break;
                            }
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
            // pb_queue / batch / subs status — diagnostic (~1 line/sec).
            // Routed via SDL_LOG_CATEGORY_CUSTOM into quill's backtrace
            // ring; only flushed to disk on a subsequent LOG_ERROR or
            // when FM2K_SPECTATOR_DEBUG=1 is set for live diagnostic.
            {
                static uint32_t last_log_tick = 0;
                uint32_t now = (uint32_t)GetTickCount64();
                if (now - last_log_tick > 1000) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_CUSTOM,
                        "SpectatorNode: pb_queue=%zu (batch inputs=%u, start=%u, subs=%zu)",
                        g_state.pb_queue.size(), hdr.frame_count, hdr.start_frame,
                        g_state.subscribers.size());
                    last_log_tick = now;
                }
            }
            break;
        }
    }
}

void SpectatorNode_HandleUdpInputDatagram(const uint8_t* buf, size_t len,
                                          const sockaddr_in& from) {
    // Narrow by design: only UDP_INPUT_BATCH, only from the current
    // upstream, only while the per-connection admission epoch is armed.
    // Never runs the full HandleSpecData parser -- its dedup/ordering
    // assumptions are TCP-shaped.
    if (!SpecUdpEnabled()) return;
    if (len < sizeof(SpecDataHeader) + 4) return;
    SpecDataHeader hdr;
    std::memcpy(&hdr, buf, sizeof(hdr));
    if (hdr.magic != SPEC_DATA_MAGIC) return;
    if (hdr.type != SpecDataType::UDP_INPUT_BATCH) return;
    if (!g_state.subscribed_upstream) return;
    if (!g_state.have_frame_baseline) return;
    if (!g_state.udp_epoch_armed) return;
    if (!AddrEqual(from, g_state.upstream_addr)) return;  // spoof / stale upstream
    // Test-only spectator-downlink loss (no-op unless FM2K_SPEC_DROP set).
    // Discard BEFORE the liveness stamp + admission so a "dropped" batch is
    // indistinguishable from one lost on the wire.
    if (fm2k::specimpair::ShouldDropUdpInput()) return;
    g_state.last_udp_recv_ms = GetTickCount64();

    uint32_t op_seq = 0;
    std::memcpy(&op_seq, buf + sizeof(SpecDataHeader), 4);
    const size_t frames = hdr.frame_count;
    const size_t inputs_end = sizeof(SpecDataHeader) + 4 + frames * 4;
    if (frames == 0 || len < inputs_end) return;  // malformed
    if (op_seq > g_state.udp_highest_op_seq) {
        g_state.udp_highest_op_seq = op_seq;
    }

    // Redundant ops tail: parsed here, accepted via accept_eligible()
    // interleaved with the input admission below. An op enters the queue
    // only when (a) it is the next op in global order and (b) every
    // input that precedes it positionally has been admitted -- accepting
    // early pushed boundary ops ahead of the battle-tail inputs (the
    // seam engaged early, its mask expired into leftover attack bits,
    // carried cursors insta-locked: the 17/13 battle restart,
    // 2026-06-11). The tail reships in every datagram, so deferred
    // acceptance costs nothing.
    struct TailOp { uint32_t idx; uint32_t pos; const uint8_t* w; uint8_t wlen; bool done; };
    TailOp tail_ops[State::OPS_RING] = {};
    int tail_n = 0;
    if (len > inputs_end + 1) {
        const uint8_t* t   = buf + inputs_end;
        const uint8_t* end = buf + len;
        uint8_t n = *t++;
        for (uint8_t i = 0; i < n && t + 9 <= end &&
                            tail_n < (int)State::OPS_RING; ++i) {
            TailOp& o = tail_ops[tail_n];
            std::memcpy(&o.idx, t, 4); t += 4;
            std::memcpy(&o.pos, t, 4); t += 4;
            o.wlen = *t++;
            if (t + o.wlen > end) break;
            o.w = t; o.done = false; t += o.wlen;
            ++tail_n;
        }
    }
    auto accept_eligible = [&]() {
        for (bool progress = true; progress; ) {
            progress = false;
            for (int i = 0; i < tail_n; ++i) {
                TailOp& o = tail_ops[i];
                if (o.done || o.idx != g_state.ops_seen ||
                    g_state.next_expected_frame < o.pos) continue;
                o.done = true;
                SessionEvent ev{};
                uint8_t hdr_buf[SESSION_EVENT_MATCH_HDR_SIZE];
                const size_t consumed =
                    SessionEvent_Decode(o.w, o.wlen, &ev, hdr_buf);
                if (consumed != o.wlen ||
                    ev.type == SessionEventType::INPUT) continue;
                if (ev.type == SessionEventType::MATCH_START) {
                    MatchHeader hdr_copy;
                    std::memcpy(hdr_copy.data(), hdr_buf, hdr_copy.size());
                    g_state.pb_match_headers.push_back(hdr_copy);
                    ev.u.match_start_idx =
                        (uint16_t)(g_state.pb_match_headers.size() - 1);
                }
                g_state.pb_queue.push_back(ev);
                ++g_state.ops_seen;
                progress = true;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[SPEC-UDP] accepted op #%u type=%u at pos=%u via "
                    "datagram tail", o.idx, (unsigned)ev.type, o.pos);
                switch (ev.type) {
                    case SessionEventType::PIN_RNG:
                        SpectatorNode_AppendPinRng(ev.u.pin_rng_seed); break;
                    case SessionEventType::RESET_INPUT_STATE:
                        SpectatorNode_AppendResetInputState(); break;
                    case SessionEventType::SOUND_INIT:
                        SpectatorNode_AppendSoundInit(); break;
                    case SessionEventType::FINGERPRINT:
                        SpectatorNode_AppendFingerprint(ev.u.fingerprint_hash); break;
                    case SessionEventType::MATCH_START:
                        SpectatorNode_AppendMatchStart(hdr_buf); break;
                    case SessionEventType::MATCH_END:
                        SpectatorNode_AppendMatchEnd(ev.u.match_end); break;
                    case SessionEventType::SESSION_ID:
                        SpectatorNode_AppendSessionId(ev.u.session_id); break;
                    case SessionEventType::CSS_ENTERED:
                        SpectatorNode_AppendCssEntered(); break;
                    case SessionEventType::ROUND_START: {
                        const auto& rp = ev.u.round_start;
                        SpectatorNode_AppendRoundStart(rp.round_idx,
                            rp.p1_hp_max, rp.p2_hp_max, rp.timer_seconds);
                        break;
                    }
                    case SessionEventType::ROUND_END: {
                        const auto& rp = ev.u.round_end;
                        SpectatorNode_AppendRoundEnd(rp.winner_idx,
                            rp.p1_hp_remaining, rp.p2_hp_remaining);
                        break;
                    }
                    default: break;
                }
            }
        }
    };
    accept_eligible();

    // [SPEC-UDP] 1Hz heartbeat BEFORE the gates -- pause periods are
    // exactly when this diagnostic matters (the original tail placement
    // went silent the moment the op gate engaged).
    {
        static uint64_t s_last_log_ms = 0;
        const uint64_t now = GetTickCount64();
        if (now - s_last_log_ms >= 1000) {
            s_last_log_ms = now;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[SPEC-UDP] admitted=%u paused_on_gate=%u dropped_on_gap=%u "
                "op_seq=%u ops_seen=%u q=%zu",
                g_state.udp_admitted_total, g_state.udp_paused_on_gate,
                g_state.udp_dropped_on_gap, op_seq, g_state.ops_seen,
                g_state.pb_queue.size());
        }
    }

    // Admission gate: every op the host appended before these inputs must
    // already be TCP-decoded locally (see invariant in spectator_node.h).
    // Boundary ops in flight (MATCH_END .. MATCH_START) pause admission;
    // the TCP burst that delivers them also jumps the cursor, after which
    // UDP re-engages. Self-healing by redundancy -- nothing is lost.
    {
        bool blocking_op_in_tail = false;
        for (int i = 0; i < tail_n; ++i) {
            if (!tail_ops[i].done && tail_ops[i].idx == g_state.ops_seen) {
                blocking_op_in_tail = true;
                break;
            }
        }
        if (op_seq > g_state.ops_seen && !blocking_op_in_tail) {
            // The op we need fell out of the 8-op tail window and TCP
            // hasn't delivered it -- inputs past it must wait (backfill
            // or a later tail recovers).
            ++g_state.udp_paused_on_gate;
            return;
        }
    }
    // Window entirely ahead of the cursor (loss burst exceeded the
    // redundancy window): drop whole datagram; TCP fills the hole.
    if (hdr.start_frame > g_state.next_expected_frame) {
        ++g_state.udp_dropped_on_gap;
        return;
    }

    uint32_t cursor   = hdr.start_frame;
    uint32_t admitted = 0;
    const uint8_t* p = buf + sizeof(SpecDataHeader) + 4;
    for (size_t i = 0; i < frames; ++i, ++cursor) {
        if (cursor != g_state.next_expected_frame) continue;  // already queued
        // In-order invariant per frame: if an op should precede further
        // inputs and it is not recoverable from this tail, stop here --
        // its position is unknown, so any further admit could jump it.
        if (op_seq > g_state.ops_seen) {
            bool next_op_in_tail = false;
            for (int k = 0; k < tail_n; ++k) {
                if (!tail_ops[k].done &&
                    tail_ops[k].idx == g_state.ops_seen) {
                    next_op_in_tail = true;
                    break;
                }
            }
            if (!next_op_in_tail) {
                ++g_state.udp_paused_on_gate;
                break;
            }
        }
        uint16_t p1 = 0, p2 = 0;
        std::memcpy(&p1, p + i * 4,     2);
        std::memcpy(&p2, p + i * 4 + 2, 2);
        SessionEvent ev{};
        ev.type       = SessionEventType::INPUT;
        ev.u.input.p1 = p1;
        ev.u.input.p2 = p2;
        g_state.pb_queue.push_back(ev);
        SpectatorNode_StampInputAdmit();
        SpectatorNode_StampUdpInputAdmit();  // UDP-borne: feeds the
                                             // TCP-only floor pre-arm
        g_state.next_expected_frame = cursor + 1;
        ++admitted;
        accept_eligible();  // an op positioned right after this input
        // Hop-1 relay parity with the TCP path (line ~3318): the late TCP
        // copy of these frames is positionally skipped there, so without
        // this append a relay node's downstream stream would lose them.
        SpectatorNode_OnFrameConfirmed(p1, p2);
    }
    g_state.udp_admitted_total += admitted;
}
