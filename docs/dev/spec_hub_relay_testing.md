# Spec hub-relay testing runbook

Companion to `docs/dev/spec_hub_relay_design.md`. Practical steps for
verifying the relay end-to-end. **Nothing in Phase 1-3 has been
exercised against real network conditions** -- builds clean and unit
tests pass, but the multi-process interaction needs live testing.

## Where the bits live

Local source after Phase 1-3 (commits `33a7890..d9c2590` in wanwan,
`33a7890..83a6de5` in fm2k-hub):

| What                              | Where                                            |
|-----------------------------------|--------------------------------------------------|
| Hub `SPEC_RELAY` registry         | `hub/hub.py:60-120`                              |
| Hub WS binary frame handler       | `hub/hub.py:handle_spec_relay_frame`             |
| Hub `spec_transport` negotiation  | `hub/hub.py` (hello + spectate_grant)            |
| Hook env read + skip TCP plane    | `FM2KHook/src/netplay/spectator_node.cpp` Init   |
| Hook outbound ring producer       | `OutboundBroadcast / OutboundSendTo` helpers     |
| Hook inbound ring consumer        | `SpectatorNode_TickHealth` drain                 |
| Launcher hello advertise          | `FM2K_HubClient.cpp` (FM2K_SPEC_TRANSPORT)       |
| Launcher outbound drain           | `FM2K_RollbackClient::Update`                    |
| Launcher inbound producer         | `on_spec_relay_bytes` callback                   |
| Shared mem queues                 | `FM2KHook/src/netplay/spec_relay_queue.{h,cpp}`  |

## Pre-flight (one-time)

1. Deploy hub changes to production hub:
   ```
   ssh 2dfm
   cd /home/fm2k/hub
   git pull origin master   # or local push if you keep a checkout
   sudo systemctl restart fm2k-hub
   ```
2. Verify hub started + SPEC_RELAY exists:
   ```
   journalctl -u fm2k-hub --since "5 minutes ago" | grep -iE "spec_relay|started"
   ```

## Smoke test 1: env detection only (no spec, no match)

Goal: confirm hook detects `FM2K_SPEC_TRANSPORT=relay` and skips TCP
listener creation without breaking anything else.

```bash
# In one terminal, run launcher with env set:
FM2K_SPEC_TRANSPORT=relay /mnt/c/games/FM2K_RollbackLauncher.exe

# Launch any game from the lobby.
# Verify hook log shows:
#   SpectatorNode: FM2K_SPEC_TRANSPORT=relay -- skipping TCP listener + TCP-STUN
#   SpectatorNode: Init (capacity=4, batch=8 frames, transport=relay, out=ok, in=ok)
```

If `out=failed` or `in=failed`, check Windows perm for named-mapping
creation (rare; usually only happens on locked-down corp domains).

## Smoke test 2: host advertises, hub sees it

Goal: hub stores `spec_transport=relay` on User and would forward in
spectate_grant.

```bash
# Launcher env: FM2K_SPEC_TRANSPORT=relay
# Connect to hub, sign in via Discord.

# In hub log (hub journalctl):
[auth] ACCEPT <peer> ... 
[+] <nick> ... v0.X.Y from (<ip>, port)
# No explicit "spec_transport=relay" log line yet -- check User in
# repl:  python -c "from hub.hub import USERS; print(USERS[<id>].spec_transport)"
# (or add a debug print)
```

**TODO**: add an operator-visible log line when User.spec_transport
becomes "relay" so this step is easier to verify.

## Smoke test 3: end-to-end with mock host (no game)

This requires a small python helper to act as a fake host. We'll
write it when actual testing happens; for now, the unit test
`hub/tests/test_spec_relay.py:test_fanout_to_all_subscribed_specs`
covers the hub-side fan-out correctness.

## Real test: VPN rig, 2 machines

Both machines on the same VPN so they can both reach the production
hub at port 443 (or use a local hub).

**Setup**:
- Machine A (host): `FM2K_SPEC_TRANSPORT=relay` set before launcher start
- Machine B (spec): same env
- Both signed into Discord, both with Patreon role

**Steps**:
1. A + B both enter the same room
2. A challenges B; B accepts; battle starts on both via GekkoNet UDP P2P
   (this path is unchanged -- spec relay only affects spec data, not
   player-to-player inputs).
3. Open a third VPN machine OR use machine B in a second instance
   with `FM2K_DEV_USER_SUFFIX=b` for testing.
4. Spec clicks "Spectate" on A in the lobby.
5. Hub forwards spectate_grant with `spec_transport: "relay"`.
6. Spec hook receives SPEC_JOIN_ACK over UDP (unchanged) but does NOT
   dial host's TCP port (look for log: `JOIN_ACK accepted in relay
   mode -- skipping ConnectUpstream`).
7. Host hook ships SNAPSHOT + EVENT_BATCH via outbound ring.
8. Launcher A drains ring + ships as WS binary frames to hub.
9. Hub fans out to spec via `SPEC_RELAY[host_id].specs`.
10. Spec launcher receives WS binary, writes to inbound ring.
11. Spec hook drain feeds `SpectatorNode_HandleSpecData`.
12. Spec sees host's match in real time.

**Expected log lines on host (A)**:
```
SpecRelay: opened outbound ring for game pid <pid>
SpectatorNode: hub-coordinated NAT punch toward spectator ... user_id=<id>
SpectatorNode: Accepted subscriber <addr> ...
```

**Expected on spec (B)**:
```
SpectatorNode: JOIN_ACK accepted in relay mode -- skipping ConnectUpstream
SpecRelay: opened inbound ring for spec game pid <pid>
SpectatorNode: applied PIN_RNG=0x...      (from EVENT_BATCH)
SpectatorNode: MATCH_START frame=X ...    (from snapshot+events)
```

**Hub log**:
```
[spec_relay] subscribed <spec_nick> to <host_nick> (host has 1 specs)
[+] <spec_nick> ... 
```

## Failure modes to look for

| Symptom                                        | Likely cause                                            |
|------------------------------------------------|---------------------------------------------------------|
| Spec sees JOIN_ACK but no data                 | Inbound ring not opened or dropped frames               |
| Host outbound ring fills (total_dropped > 0)   | Launcher drain too slow, or WS bandwidth saturated      |
| Spec crashes in HandleSpecData                 | Inbound frame corruption -- check launcher WS recv      |
| `relay SendTo ... no spec_user_id, dropping`   | spec_user_id propagation broke -- check shared mem v13  |
| Hub says "Not registered as a relay host"      | Host wasn't in_match when fan-out fired                 |
| Spec stays on "Connecting..."                  | Mixed mode (host TCP + spec relay or vice versa)        |
| `SpecRelayQueue: Enqueue payload_len=N > MAX`  | Some send path exceeded SLOT_PAYLOAD_MAX (32 KB).       |
|                                                | Means a new payload type is bigger than the slot;       |
|                                                | bump SLOT_PAYLOAD_MAX or fragment that payload.         |
| `SpecRelayQueue: ring full ...`                | Producer (hook) faster than consumer (launcher drain).  |
|                                                | Often = WS upload saturated. Subsequent drops are       |
|                                                | counted in total_dropped but silent.                    |

## Bugs caught in the post-Phase-3 audit (already fixed; here so we
## remember they EXISTED if symptoms reappear)

- **SLOT_PAYLOAD_MAX was 16 KB, snapshot chunks are 16 KB + 10 B
  SpecDataHeader = 16394 B.** Every snapshot chunk would have been
  silently dropped. Fixed by bumping to 32 KB.
- **Ring of 64 slots overflowed during snapshot transfer.** Bumped
  to 128.
- **Launcher static cache didn't retry OpenInboundFor/OpenOutbound-
  For when the first attempt raced past hook init.** Fixed: retry
  every tick while ring is null.
- **Snapshot+backfill never shipped in relay mode** because the
  bind loop's RegisterAcceptedClient always returned false (no
  TCP listener). Fixed: relay mode treats sub as auto-bound the
  moment spec_user_id is populated.
- **TARGET_BROADCAST hub fan-out raced pre-bind specs into
  receiving live events before their snapshot.** Fixed: relay-mode
  OutboundBroadcast iterates bound subs and Enqueues TARGET_DIRECT
  per spec, exactly mirroring the TCP path's backfill_done fence.
- **HandleJoinReq dispatched BEFORE the punch-target poll updated
  the user_id dict in the same tick** (ControlChannel_Poll runs
  RawReceive then TickHostMaintenance). Fixed: dict miss falls
  back to direct shared-mem read of the latest punch target.

## Diagnostic counters

Each ring tracks 3 counters (zeroed at create):
- `total_enqueued`: bytes worth of slots filled
- `total_dropped`: producer-side drops (ring full)
- `total_dequeued`: consumer-side pops

Reading them currently requires a debugger -- worth a future
`/status` slash command or status line item that surfaces them in
the launcher UI for quick diagnostics during a test session.

## Rollback

To disable relay mode entirely:
- Unset `FM2K_SPEC_TRANSPORT` env on both peers (returns to legacy TCP).
- No hub-side change required; clients without the env send no
  `spec_transport` field which the hub treats as "tcp".

## Not yet covered

- Spec-side parsing of `spec_transport` from `spectate_grant`. Right
  now spec relies on its own env; mixed mode produces silent failure.
  Phase 4 should make spec branch on grant's `spec_transport`.
- Operator-visible logging when relay mode is active (currently only
  init-time log line).
- Hub-side metrics: how many active relays, bytes/sec, drops.
- Status bar item in launcher UI showing inbound/outbound ring
  fill + counters.
- Soak test against simulated packet loss / high latency on the WS
  link (snapshot 850 KB takes long enough that intermittent disconnect
  during transfer is a real risk).
