# Spec hub-relay design (v0.3 spectator rebuild)

**Status**: design / pre-implementation, 2026-05-18.
**Author**: this thread, after the v0.2.49 -> v0.2.57 firefighting cycle made it clear the P2P spec architecture isn't salvageable.

## Why we're rebuilding spec

Reread `docs/dev/spectator_repair_plan_2026-05-13.md` for the history. TL;DR of where we ended up:

- Spec was bolted onto a P2P-pair design with its own TCP listener, NAT punch, simultaneous-open, and TCP-STUN. ~4000 lines.
- Works on cone NAT, silently fails on symmetric / port-restricted NAT with no fallback. ~30% of users get stuck on "subscribed but never bound".
- The current 5s retry loop interacts badly with GekkoNet's no-remove-actor API to leak GekkoSpectator actors, melting host frame rate. (Hotfix in `spec: dedup gekko_add_actor`.)
- Each "fix" has uncovered another edge case. We're past the point where incremental patching pays.

## The shape of the rebuild

**One sentence**: stop trying to do P2P for spec data. Route it through the hub.

**One picture**:

```
                                  hub (cloud, public IP, TLS:443)
                                  +-------------------+
                                  |  match registry   |
                                  |  match_id -> {    |
                                  |    host: UserA,   |
                                  |    specs: [B,C..] |
                                  |  }                |
                                  +-------------------+
                                       ^         |
                          spec_data_   |         | spec_data_to_spec
                          to_host (WS) |         | (WS fan-out)
                                       |         v
   +------------+ udp gekko +------+   |    +------+    +------+
   | player A   |<--------->|host B|<--+--->|spec C|    |spec D|
   | (player 2) |           |peer  |        +------+    +------+
   +------------+           +------+        (no P2P to host at all)
```

- **Player-to-player inputs**: unchanged. GekkoNet UDP, two-peer rollback session, direct P2P.
- **Host-to-spec data**: routes through hub WebSocket. No P2P TCP, no NAT punch.
- **Spec-to-host control** (subscribe, leave): already over hub WS today. Unchanged.
- **Live input replay to specs**: GekkoSpectator path stays (GekkoNet UDP, already works). The relay is for the OTHER payloads (INITIAL_MATCH, snapshot, backfill, EVENT_BATCH).

## What this deletes

From `FM2KHook/src/netplay/spectator_tcp.cpp` (~700 lines):

- `StartListener`, `PollAccepts`, `PollIncoming`, `g_pending_clients`, accept queue (-150 lines)
- `RegisterAcceptedClient`, IP-only matching, `g_subs` (-80 lines)
- `BroadcastToAll`, `SendTo`, `DisconnectSubscriber` (TCP versions; replaced by hub-write equivalents) (-100 lines)
- `PerformTcpStun`, raw-winsock STUN client (-80 lines)
- `ConnectUpstream`, `PollUpstream`, `g_upstream_sock`, the spec-side TCP dial (-150 lines)
- All framing logic stays; just rewires the I/O.

From `FM2KHook/src/netplay/spectator_node.cpp`:

- TCP simultaneous-open punch block (line 3220-3325, ~100 lines)
- `g_pending_punch_sockets` + deferred-close sweep (-20 lines)
- Per-subscriber `tcp_bound` state machine + the TryBindPendingTCP loop (-60 lines)
- `host_tcp_port` + `external_tcp_addr` fields (no longer relevant)

From `hub/hub.py`:

- `PerformTcpStun` server side (TCP STUN responder) -- if we have one
- `_send_spectator_incoming`'s TCP port reporting -- spec_incoming no longer needs to carry a TCP punch target

From `FM2K_RollbackLauncher`:

- `external_tcp_addr` reporting via WS `tcp_addr` msg (-20 lines)
- Launcher's pre-stamp STUN coordination (-30 lines)

**Total**: ~1200 lines deleted. ~300 lines added (the host-to-hub write path + hub-side fan-out + spec-side WS receive). Net **-900 lines** with no NAT-class limitations.

## What stays

- `SpecDataHeader` + all `SpecDataType` enum values (INITIAL_MATCH, EVENT_BATCH, INPUT_BATCH, MATCH_END, INPUT_REQUEST, SNAPSHOT_BEGIN/CHUNK/END). Wire format unchanged.
- The host-side `g_state.subscribers` list. Just addressed by `spec_user_id` string instead of `sockaddr_in`.
- Snapshot capture (`current_snapshot`, `SendSnapshotTo`, `SendSessionBackfillFromFrame`).
- GekkoSpectator add path for live UDP inputs (separate codepath, untouched).
- Hub authentication / OAuth / Patreon gate.
- Hub's `spectate_request` -> `spec_incoming` notification flow (just simpler payloads).

## Protocol design

### Transport

- WebSocket binary frames over the existing hub TLS:443 connection.
- aiohttp already terminates WS in `hub/hub.py`. Adding binary-frame support is ~5 lines.
- Hub fans out: one host write -> N spec writes.

### New WS message types

**Host -> Hub** (`spec_data` binary frame):

```
[u32 magic = 0x53504442 ("SPDB")]
[u32 frame_count]    -- mirrors SpecDataHeader.frame_count
[u16 type]           -- SpecDataType
[u16 flags]          -- mirrors SpecDataHeader.flags
[u32 payload_len]
[u8  payload[payload_len]]
[u8  target_kind]    -- 0 = all subs, 1 = specific spec_user_id below
[u8  spec_user_id_len]
[char spec_user_id[spec_user_id_len]]  -- empty if target_kind=0
```

Hub validates host is a registered match host, looks up `match_id -> specs`, fans out.

**Hub -> Spec** (`spec_data` binary frame, same shape, minus the target fields):

```
[u32 magic = 0x53504442]
[u32 frame_count]
[u16 type]
[u16 flags]
[u32 payload_len]
[u8  payload[payload_len]]
```

Spec parses, dispatches through the existing receive path that currently handles TCP-arrived frames.

### Subscription / match-tracking

Hub maintains `MATCH_REGISTRY: dict[match_id, MatchInfo]` where `MatchInfo` has:

```python
@dataclass
class MatchInfo:
    host_id: str             # User.id of the host
    player_id: str           # User.id of P2
    specs: set[str]          # User.ids of subscribed specs
    created: float           # wall clock for stale-match GC
```

- `match_started` WS msg from host populates an entry (today host already sends this for hub's match-display purposes -- extend it to track host_id explicitly).
- `spectate_request` from spec adds spec_id to `specs[match_id]`.
- `spectate_leave` (or WS disconnect) removes it.
- Stale matches GC'd after ~30 min of no activity.

### Flow: spec joins mid-match

1. Spec sends `spectate_request {target_user_id}` over WS.
2. Hub validates Patreon gate, locates `match_id` from `target_user_id`, adds spec to `specs[match_id]`.
3. Hub sends `spec_incoming {spec_user_id, mode=CURRENT_MATCH or FULL_SESSION}` to host. (No `spec_tcp_port`, no `spec_udp_addr` -- relay doesn't care about peer addresses.)
4. Host's hook adds spec to `g_state.subscribers` keyed by `spec_user_id`. Marks "needs initial backfill".
5. Host writes INITIAL_MATCH + (snapshot or from-frame-0 backfill) via `FM2K_HubClient::SendSpecDataToSpec(spec_user_id, ...)` -- one WS write per chunk, hub fans out only to this spec.
6. Hub forwards to spec's WS.
7. Spec parses, applies via existing receive dispatcher.
8. Live event batches: host writes via `FM2K_HubClient::SendSpecDataToAll(...)`. Hub fans out to every spec in `specs[match_id]`. Specs apply normally.

### Flow: host shuts down or match ends

- Host emits MATCH_END via `SendSpecDataToAll`.
- Hub forwards, then removes match entry. Specs see MATCH_END and disconnect their spec session.

## Migration / feature-flag plan

We can't switch atomically -- spec hooks running v0.2.57 won't speak relay, and we can't force-update everyone simultaneously. Both paths need to coexist for ~1 release cycle.

**Phase 0 (this design doc)**: agreed.

**Phase 1 (hub scaffold)**: hub-side WS message handlers + match registry + fan-out. Untested by game, tested with a python stub client. Ships in a hub-only update (no client change). Available for use, not used yet.

**Phase 2 (host opt-in)**: host hook gets `FM2K_SPEC_TRANSPORT=relay` env var. When set:
- Host advertises `spec_transport=relay` in WS `hello`.
- Spec checks host's transport in spec_incoming; if relay, dials hub instead of P2P.
- Default off; flip on for willing testers.

**Phase 3 (spec opt-in)**: spec hook gets same env var. Both paths active concurrently in spec; whichever frame arrives first wins. Disable P2P spec-side TCP dial if `spec_transport=relay` was advertised.

**Phase 4 (flip default)**: once relay has soaked for ~2 weeks across diverse NAT setups, default `FM2K_SPEC_TRANSPORT=relay` in a release. P2P path stays in code as a fallback.

**Phase 5 (delete P2P)**: a release or two later, delete the P2P TCP listener, punch, STUN paths entirely. Net ~900 line deletion.

## Open questions

- **Bandwidth cost on the hub**: snapshot is ~850 KB once per match-start + per spec. N specs = ~850 KB * N. Live input batches are tiny (~200 B / batch / 12 batches per sec = ~2 KB/s per spec). At 100 concurrent matches with avg 2 specs each, that's a one-time ~170 MB at match-start spikes + a steady ~400 KB/s. Hub VPS bandwidth is fine.
- **Hub CPU for fan-out**: O(N) per match write. Cheap.
- **Hub becomes a SPOF for spec**: yes. Tradeoff we accept -- hub already is a SPOF for matchmaking. Spec losing connection when hub bounces is acceptable; matches don't.
- **Multiple specs joining same match concurrently**: hub fans out the snapshot to each. Snapshots aren't deduped server-side because they're stateful per-spec (frame anchor differs).
- **Spec-to-spec relay (subscriber tree)**: currently we have `SpectatorNode_HandleJoinReq` that can route a spec through another spec's "subscriber tree" if direct connection fails. With hub relay, the subscriber-tree feature is moot -- hub does the multiplexing. Delete it in Phase 5.

## Out of scope for this design

- WebRTC / browser-based spectator (could come later, hub-relay is the prerequisite).
- TURN-style relay for player-to-player inputs (no, GekkoNet UDP punching works for the player case; this is spec-only).
- ICE-style direct-first / relay-fallback for low-latency spec when both ends are cone NAT. Maybe later; relay-always is simpler and the latency cost is fine for non-interactive spec.

## Scope of work to ship Phase 1 today

1. `hub/hub.py`: add `MATCH_REGISTRY`, WS message handlers for `spec_data` binary frames, fan-out routing. (~150 lines)
2. `tests/hub/test_spec_relay.py`: pytest with two stub WS clients (host + spec), verifies host->hub->spec roundtrip of a sample SpecDataHeader payload.
3. No client-side changes. Phase 2+ commits handle host/spec hook integration.

Phase 1 is shippable as a hub-only update once tested. The hub-relay primitive becomes available without affecting any running clients.
