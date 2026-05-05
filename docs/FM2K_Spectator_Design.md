# FM2K Spectator Design

**Status:** Design + scaffold. Playback driver + UI pending.
**Depends on:** `#5 replays` (complete), `#4 NAT traversal` (design only; MVP works over existing direct-connect + multiplexed UDP).

---

## 1. Goal

Any number of viewers can watch a live match in near-real-time. Viewers join the match via lobby (or direct IP), walk automatically through CSS, watch the battle, loop to the next match. No spectator count cap that's fundamentally tied to host uplink.

---

## 2. Model: CCCaster-style hub-with-redirect

**Not a star topology** (host serves everyone — breaks at ~10 viewers on home uplink).
**Not a strict tree** (fragile; single-point-of-failure cascades).

Instead — every client is a potential **relay**:

```
                HOST
              /  |   \
           S1   S2    S3
          / \    |
        S4  S5   S6
             |
             S7
```

- Host accepts up to N direct subscribers (capacity set from measured uplink).
- Over capacity → host replies `JOIN_REDIRECT` with one of its existing subscribers (`getRandomRedirectAddress` in CCCaster).
- Each accepted subscriber runs the same "SpectatorNode" code and can accept its own subscribers.
- Result: dynamic multi-level tree. If any intermediate node drops, its children reconnect to a sibling / root.

Key properties:
- **Bandwidth is bounded per node**, regardless of total viewer count.
- **Upstream doesn't depend on downstream.** Players' match quality is unaffected by spectator load on downstream nodes.
- **Uses the same punched UDP socket** as gameplay — no new NAT hole per spectator.

---

## 3. What gets streamed

**Not** full save states. Size-prohibitive and unnecessary.

Following the replay format (`docs/FM2K_Rollback_Production.md` §3 + `replay.h`):

| Data | When | Size |
|------|------|------|
| `InitialMatch` | Once on join — replay header equivalent: game_hash, initial_rng_seed, initial_state_hash, char selects, start-frame | 96 B |
| `InputBatch` | Every K frames (tunable, e.g. K=8) — N frames of (p1_input, p2_input) starting at frame F | 4*N + 8 B hdr |
| `MatchEnd` | Battle ends, host moves to CSS | — |
| `CssUpdate` | CSS cursor / char selects propagated during between-match phase | tiny |
| `Heartbeat` | Every 1s, keeps tree alive, detects dropped links | — |

Total bandwidth per downstream link at K=8, 100 FPS match: ~12.5 batches/sec × (32 B input + 8 B hdr) = ~500 B/s. Trivially small. A single node can comfortably serve 30-50 children before saturating residential upload.

---

## 4. Spectator playback

Spectator is a **local replay player** (§9 of master doc):
1. Receive `InitialMatch` → seed the replay engine (RNG + char selects + start fingerprint).
2. Receive `InputBatch` → append to local input queue.
3. Drive the trampoline in replay mode: fetch next (p1, p2) from queue each frame, run sim.
4. If queue empty → pause (waiting for upstream).
5. On `MatchEnd` → return to CSS, wait for next `InitialMatch`.

This is the same trampoline phase-dispatch machinery we already have (`LoopPhase::TRAMPOLINE_BATTLE`) — we just add `LoopPhase::SPECTATOR_PLAYBACK` which sources inputs from the spectator queue instead of GekkoNet.

**Late joiners** start at the host's current cursor. They see the match "from wherever it is now" — not from frame 0. This is consistent with Twitch-style live viewing. If a viewer wants to see the whole match, they watch the recorded replay afterward.

---

## 5. Protocol (control-channel extension)

Reuses the existing 0xCC multiplexed control channel. Spectator packets fit in the existing 64-byte CtrlPacket for small messages; `InputBatch` needs a variable payload so we add a secondary 0xCE-prefixed datagram path just for spectator-data — same socket, just a larger payload format.

New `CtrlMsg` values:
- `SPEC_JOIN_REQ` — "I want to spectate"
- `SPEC_JOIN_ACK` — "accepted" (small payload)
- `SPEC_JOIN_REDIRECT` — "full, try X.Y.Z.W:P" (includes `IpAddrPort`)
- `SPEC_HEARTBEAT` — 1s keepalive (bidirectional)
- `SPEC_LEAVE` — clean disconnect

New 0xCE datagram type (not in CtrlPacket — variable length):
- `SpecDataHeader { magic=0xCE, type: u8, start_frame: u32, frame_count: u16 }`
- `type = INITIAL_MATCH` → 96-byte ReplayHeader as payload
- `type = INPUT_BATCH` → `ReplayFrame[frame_count]` (4 B each)
- `type = MATCH_END`
- `type = CSS_UPDATE`

All packets unreliable UDP. Input batches are cumulative + idempotent: if spectator missed frames 100-107 and receives batch starting at 108, they request a rewind via `SPEC_REWIND_REQ` (or wait for the next cycle if upstream re-broadcasts).

For MVP we'll just send each batch twice with 50ms spacing — packet loss on LAN/decent internet is <1% so redundancy is cheaper than retransmit logic.

---

## 6. Capacity + redirect policy

Per node:
```
max_direct_subscribers = f(measured_uplink_bandwidth)
  default: 8
  tunable via UI
```

On `SPEC_JOIN_REQ` received:
```
if len(subscribers) < max_direct:
  accept → send SPEC_JOIN_ACK + SpecDataHeader{INITIAL_MATCH}
else if len(subscribers) > 0:
  # random redirect à la CCCaster
  pick one subscriber uniformly at random
  send SPEC_JOIN_REDIRECT { redirect_to: subscriber.public_addr }
else:
  # edge case: cap=0, no subscribers — should not happen in practice
  send SPEC_JOIN_REDIRECT { redirect_to: null } → spectator gives up
```

Subscriber's public address is what upstream saw as the source address when it subscribed. This is already the punched reflexive address — it's reusable for sideways connections from a new peer because the NAT binding is still open (as long as we send keepalives, which `SPEC_HEARTBEAT` does).

**Caveat:** Port-restricted NATs only accept inbound from IPs the owner has sent to. A redirect target needs to have talked to the new spectator first, or we need host-mediated hole-punch (host introduces the two via a brief trilateral probe). Phase 2 problem.

---

## 7. Relation to GekkoNet's native spectator

`GekkoSpectateSession` exists natively (`vendored/GekkoNet/.../gekkonet.h:61`). But:
- It's **host-centric**: `max_spectators` is set on the *game* session, all spectators connect direct to the host.
- No relay / daisy-chain — doesn't scale past the host's uplink.
- Tightly couples spectators to the live GekkoNet session; spectator has to be up before match start.

We **don't use** `GekkoSpectateSession`. Instead:
- Gameplay peers use `GekkoGameSession` (existing).
- Spectators are a **separate replay-streaming layer** on top. They never talk to GekkoNet. They receive inputs over our protocol and drive a local replay engine via the trampoline.

This decouples spectator count from gameplay rollback stability. More spectators → more tree layers → zero impact on the two players.

---

## 8. CCCaster reference points

From `old/cccaster/DllSpectatorManager.cpp` (235 lines):

| Concept | CCCaster | Ours |
|---------|----------|------|
| Join | `pushSpectator(sock, serverAddr)` | `SpectatorNode_AcceptJoin(peer, reply_addr)` |
| Initial handoff | `newSocket->send(RngState + InitialGameState)` | `InitialMatch` datagram (replay header + cursor) |
| Redirect | `getRandomRedirectAddress()` → one of self's subscribers OR `clientServerAddr` | `SpectatorNode_PickRedirectTarget()` — same random policy |
| Broadcast | `frameStepSpectators()` — round-robin per frame | `SpectatorNode_Broadcast()` every K frames, full list |
| Backpressure | `preserveStartIndex = min(spectator.pos.index)` | `retain_from_frame = min(all subscribers' ack frame)` |
| State size | Inputs + RngState + menu index | Inputs + initial RNG + initial fingerprint |

CCCaster interleaves broadcast with game frame stepping (every match frame, broadcast one update to one spectator in round-robin, interval scales with count). We **batch** instead (every K frames, send to all). Simpler, lower per-packet overhead. Tune K if jitter is visible in testing.

---

## 8.5 Spectators during sets (CSS between matches)

GekkoNet's native spectator support attaches to a **live battle session only**. A set is multiple battles back-to-back separated by CSS, so between battles the GekkoNet spectator stream dies. We have to drive spectators through CSS ourselves; otherwise they fall off after match 1.

This is the entire reason we don't use `GekkoSpectateSession` — it's match-scoped, and we need set-scoped.

### Lifecycle on the host (one set, three matches)

```
INITIAL_MATCH(seed=A) → INPUT_BATCH × N → MATCH_END
   ↓
CSS phase  (players reselect chars; CSS_UPDATE packets stream)
   ↓
INITIAL_MATCH(seed=B) → INPUT_BATCH × N → MATCH_END
   ↓
CSS phase
   ↓
INITIAL_MATCH(seed=C) → INPUT_BATCH × N → MATCH_END  (set over)
```

Each `INITIAL_MATCH` carries a fresh RNG seed + initial state hash so the spectator's replay engine resets cleanly between matches.

### Spectator-side phase transitions

The trampoline needs three spectator phases:

| Phase | Driven by | Sourced from |
|-------|-----------|--------------|
| `SPECTATOR_PLAYBACK` | INPUT_BATCH stream | replay engine consumes (p1, p2) per frame |
| `SPECTATOR_CSS_MIRROR` | CSS_UPDATE packets | host's cursor/char-lock state, rendered read-only |
| `SPECTATOR_IDLE` | nothing in flight | between sets, or upstream disconnected |

Transitions:
- `MATCH_END` received → `SPECTATOR_PLAYBACK` → `SPECTATOR_CSS_MIRROR` (don't drop subscription, don't render disconnect UI)
- `INITIAL_MATCH` received → `SPECTATOR_CSS_MIRROR` → `SPECTATOR_PLAYBACK` (reset replay engine with new seed)
- Upstream silent > 30 s → any phase → `SPECTATOR_IDLE`

### CSS state mirror payload

```cpp
struct SpecCssUpdate {
    uint8_t  p1_cursor_x, p1_cursor_y;
    uint8_t  p2_cursor_x, p2_cursor_y;
    uint8_t  p1_locked_char, p1_locked_color;  // 0xFF = unlocked
    uint8_t  p2_locked_char, p2_locked_color;
    uint16_t flags;  // both-ready, timer, etc.
};
```

Host emits `SpecDataType::CSS_UPDATE` whenever local CSS state changes (cursor move, char highlight, lock/unlock). Spectator renders the same CSS view players see, read-only.

### Set continuity

- `SPEC_HEARTBEAT` every 1 s in both directions
- Upstream drops subscriber after 30 s silence (tolerates a player taking forever to pick a char)
- Spectator's input queue pauses cleanly when empty — no "disconnect" flash, just a held frame until the next batch arrives

---

## 9. Implementation plan (this task)

Done here:
- Design doc ← this file
- `FM2KHook/src/netplay/spectator_node.{h,cpp}` — scaffold with data structures, API, stubbed join/broadcast
- `CtrlMsg` extended with `SPEC_*` values
- Hook into `Netplay_StartBattle` / `GekkoAdvance` / `Netplay_EndBattle` for host-side "push inputs to subscribers" path

Deferred to follow-up:
- Spectator-side playback driver (`LoopPhase::SPECTATOR_PLAYBACK`) — needs trampoline integration
- Launcher UI (lobby → spectate button; spectator queue list; disconnection banner)
- Join-flow wiring (direct-IP works now; lobby-backed join waits for matchmaking phase 2)
- Redirect hole-punch trilateral — symmetric/port-restricted NAT corner case
- `SPEC_REWIND_REQ` if MVP packet-loss turns out bad

---

## 10. Files

```
FM2KHook/src/netplay/
  spectator_node.h          — API: SpectatorNode_{Init,AcceptJoin,OnFrameConfirmed,OnMatchStart,OnMatchEnd,Broadcast,...}
  spectator_node.cpp        — subscriber list, redirect policy, input-batch assembly, broadcast loop
  netplay_state.h           — SPEC_* CtrlMsg values (extension)
  control_channel.{h,cpp}   — 0xCE datagram path for variable-length spectator data

docs/FM2K_Spectator_Design.md  — this file
```
