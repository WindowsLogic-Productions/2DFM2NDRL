# FM2K Matchmaking: Lobby + NAT Traversal Design

**Status:** Design — not yet implemented.
**Scope:** Replaces LilithPort's UPnP approach with lobby-backed matchmaking, hole-punching, and relay fallback.
**Hard requirement:** the socket that gets hole-punched is **the same socket** used for the netplay session (UDP multiplex, no reconnect).

---

## 1. Goals

1. **Zero router configuration** — no UPnP, no port forwarding in the common case.
2. **High reliability** — hole-punch covers most home NATs; relay covers the rest (symmetric NAT, carrier-grade NAT, corporate firewalls).
3. **Lobby / friends** — discover opponents without IP exchange, ranked/casual/private.
4. **Single socket** — hole-punched UDP socket is the same one GekkoNet + control-channel use. Already multiplexed (§4.1 of master doc), so architecturally free.
5. **No trust boundary crossing** — peers exchange addresses via lobby server but **never** trust the server with game state or input. Server is a directory + relay, nothing else.

---

## 2. Actors

| Actor | Role | Lives where |
|-------|------|-------------|
| **Lobby / Matchmaking Server** | Account directory, lobby list, match coordination, STUN-like address reflection, relay fallback | Our hosted service (1 public IP) |
| **Game Client** | FM2KLauncher + FM2KHook | User's PC |
| **Peer A / Peer B** | Two clients in a match | Behind arbitrary NAT |

Server is **stateful but small**: accounts + live-session registry. Not in the game-state path (unless relay is active).

---

## 3. Protocol layers

```
┌──────────────────────────────────────────────────────────┐
│  Match protocol (GekkoNet + 0xCC control channel)        │  ← existing
│  UDP over punched socket                                 │
├──────────────────────────────────────────────────────────┤
│  NAT traversal (STUN + ICE-lite hole-punch)              │  ← new
│  UDP control → same socket                               │
├──────────────────────────────────────────────────────────┤
│  Lobby protocol (login, matchmake, signal exchange)      │  ← new
│  TCP (TLS) to lobby server                               │
└──────────────────────────────────────────────────────────┘
```

- **Lobby:** TCP/TLS because it's long-lived, low-traffic, needs ordered delivery for chat + state updates.
- **NAT + match:** single UDP socket. NAT signaling *data* passes through lobby TCP; hole-punch *probes* use the UDP socket.

---

## 4. Socket lifecycle (critical)

The punched UDP socket must be the one used for gameplay. Sequence:

1. **Hook creates UDP socket** at launch, binds to configured local port (or OS-picked ephemeral).
2. **Hook registers socket with STUN probe** → learn public `(ip, port)` as seen by lobby server (the "reflexive address").
3. **Hook announces reflexive address to lobby** via TCP.
4. **Lobby pairs two clients**, exchanges each peer's `(reflexive, local_candidates[])`.
5. **Both peers send UDP probes** to each other's candidates simultaneously (hole-punch).
6. **First successful RX of peer's probe on either side** latches connectivity. Peer address goes into `control_channel.cpp`'s existing peer-learning slot (§4.2 of master doc).
7. **GekkoNet session starts** — uses the same socket, same learned peer address. No reconnect, no handshake retry.

The existing `NetSocket_Init` already supports empty `remote_addr` and learns peer from first authenticated packet. **Hole-punching slots in cleanly above that** — the "first authenticated packet" becomes the hole-punch probe.

---

## 5. NAT types + coverage

| NAT type | Hole-punch works? | Fallback |
|----------|-------------------|----------|
| Full-cone / open | ✓ trivial | — |
| Address-restricted | ✓ simultaneous open | — |
| Port-restricted | ✓ with correct timing | — |
| Symmetric | ✗ port prediction unreliable | Relay |
| CGNAT | Partial | Relay |
| Corporate firewall (UDP blocked) | ✗ | TCP fallback? (later) |

Target: 90%+ direct hole-punch success, relay for the rest. Relay adds ~one-way latency = RTT to relay server / 2, so we'd host relays regionally.

---

## 6. Lobby server

### 6.1 What it stores

Minimal, non-game-state:
- Account (id, display name, auth token)
- Online presence + current lobby id
- Open lobbies (host, name, password?, game variant, privacy)
- Live sessions (peer pairs, for relay routing only)

### 6.2 What it doesn't store

- Game inputs
- Rollback state
- Match results (we'll add that later as a separate service if wanted)

### 6.3 Hosting / peer-hosted?

**Recommendation: hosted matchmaking server.** Reasons:
- Symmetric-NAT peers need a stable public endpoint for relay. Peer-hosted doesn't work (the hosting peer likely has the same NAT problem).
- STUN-reflexive address discovery requires a publicly-reachable endpoint.
- Lobby browser scales better server-side than via gossip.
- Accounts / identity is much simpler with a server.

**Cost:** one small VPS handles thousands of idle lobbies. Match traffic only relays when direct fails.

### 6.4 Self-hostable?

Yes — open-source the server, let anyone run their own instance. Client has a "server" dropdown. Community can run regional servers.

---

## 7. Protocol sketch

### 7.1 Lobby (TCP/TLS, JSON or length-prefixed binary)

```
C → S: HELLO       { version, client_id }
S → C: HELLO_ACK   { session_token }
C → S: LOGIN       { username, password }  (or OAuth / Steam later)
S → C: LOGIN_OK    { account_id, display_name }

C → S: LOBBY_LIST
S → C: LOBBIES     [ { id, name, host, game, privacy, players } ]

C → S: LOBBY_HOST  { name, game, privacy, password? }
S → C: LOBBY_OK    { lobby_id }

C → S: LOBBY_JOIN  { lobby_id, password? }
S → C: LOBBY_OK    { lobby_id }  → broadcasts to both peers
S → C: LOBBY_PEER  { peer_id, peer_display_name }

// Chat on TCP (out-of-band from game)
C → S: LOBBY_CHAT  { lobby_id, text }
S → C: LOBBY_CHAT  { from, text }

// When both peers ready → NAT signaling
C → S: NAT_CANDIDATES { reflexive, local_candidates[] }
S → C: NAT_CANDIDATES_PEER { peer's reflexive, peer's local_candidates[] }
S → C: NAT_PUNCH_NOW   (simultaneous open trigger)

// Relay fallback
C → S: NAT_RELAY_REQUEST { lobby_id }
S → C: NAT_RELAY_ASSIGN  { relay_addr, session_id }
// peers then send UDP to relay instead of each other
```

### 7.2 STUN-lite (UDP, on the game socket)

Use a lightweight STUN handshake over the existing `0xCC` control-channel magic byte — or add a `0xCD` prefix for NAT-layer packets:

```
C → STUN server: STUN_BIND { txid }
STUN server → C: STUN_RESP { txid, reflexive_ip, reflexive_port }
```

Only a few hundred bytes, happens once per session start.

### 7.3 Hole-punch probes (UDP, on the game socket)

After `NAT_PUNCH_NOW`:
```
Peer A → Peer B (all candidates, in parallel): CTRL_PUNCH { my_id }
Peer B → Peer A (all candidates, in parallel): CTRL_PUNCH { my_id }
```
First successful RX on either side → peer address latched → existing control-channel HELLO/HELLO_ACK → existing flow takes over.

### 7.4 Relay (UDP, on the game socket)

Relay is a plain UDP forwarder with per-session routing:
```
Peer A → Relay: RELAY_PACKET { session_id, payload }
Relay → Peer B: RELAY_PACKET { payload }  (rewrites src)
```
Stateless after session assignment. Single server handles thousands of concurrent relays at ~Kbps per match.

---

## 8. Backwards compat with LAN / manual IP

Keep the manual "connect by IP:port" path as a hidden advanced option. Useful for:
- LAN play (bypasses lobby entirely)
- Dev/testing
- Power users

UI: "Quick Match" (lobby) is default, "Direct Connect" is under advanced.

---

## 9. Phased implementation

Each phase ships usable.

### Phase 1: Port bbbr_holepunch.cpp + STUN-lite + direct connect UI
- **Port `bbbr/revolve_input_sdl3/src/hooks/network/bbbr_holepunch.{h,cpp}` to `FM2KHook/src/netplay/nat_traversal.{h,cpp}`** — adapt socket access to go through `NetSocket_GetHandle`. Keep the burst/priority/auto-stop techniques intact.
- Add lightweight STUN client (spin up a public STUN server or use Google's)
- UI shows public reflexive address — users paste to opponent via Discord/etc.
- No lobby server yet. Manual IP exchange, but public IP is auto-detected.
- **Unlocks:** ~98% NAT coverage (per bbbr's matrix) with zero server infrastructure.

### Phase 2: Lobby server (MVP) — fork punch-check
- **Fork `bbbr/revolve_input_sdl3/punch-check/server/server.go`** (548 lines) as the base. Already has STUN reflection + peer pairing.
- Extend with: lobby list (they're per-session only), simple lobby commands
- Simple TCP server, in-memory lobby list, no persistence yet
- No accounts — just display name + generated id per session
- Hole-punch coordination via `NAT_CANDIDATES` + `NAT_PUNCH_NOW`
- No relay yet. Symmetric-NAT peers see "direct connection failed."

### Phase 3: Relay fallback
- **Fork `bbbr/revolve_input_sdl3/punch-check/relay/relay.go`** (174 lines — trivial to port)
- Stateless UDP relay colocated with lobby server
- Client auto-falls-back when hole-punch fails
- **Unlocks:** near-100% connect success rate

### Phase 4: Accounts + persistence
- DB-backed accounts, login, friends list
- Persistent lobbies / rooms
- Chat history

### Phase 5: Matchmaking + ranking
- Ranked queue, MMR
- Match history
- Regional relay selection

Spectator mode (task #6) lives on top of this — spectator is a third party the lobby assigns to a live session, streams replays from host per §9 of the master doc.

---

## 10. Open decisions

| Question | Options | Recommendation |
|----------|---------|----------------|
| Language for lobby server | Go, Rust, Node | **Go** — simplest ops, great UDP/TCP stdlib, easy cross-compile |
| Lobby TCP transport | Plain + TLS | **TLS** from day one; cert via Let's Encrypt |
| STUN server | Our own, or Google public | **Our own** — bundle with lobby, one less dependency |
| Hosting | Single VPS, multi-region | Start **single** (any decent VPS), add regional relays as relay traffic grows |
| Client auth | Username/password, OAuth, Steam | Phase 4 concern. **Username/password** is fine for MVP. |
| NAT signaling over TCP or UDP? | Either | **TCP** (lobby channel) for candidates; **UDP** (game socket) for punch probes |

---

## 11. Security notes

- Lobby TLS mandatory; self-signed cert accepted on first connect (TOFU), pinned after.
- Relay cannot inspect game state — it just forwards opaque UDP. Already true, but worth calling out for self-hosters.
- No PII collected beyond display name + optional email (for account recovery, Phase 4+).
- Per-session authentication: lobby issues short-lived session token, peers exchange on control channel, mismatch → reject.
- Rate limit lobby server aggressively (hole-punch coordination is cheap; relay traffic is expensive).

---

## 12. Files this will touch (when implemented)

```
FM2KHook/src/netplay/
  control_channel.cpp       — accept NAT-layer 0xCD packets, STUN probe sender
  nat_traversal.{h,cpp}     — NEW: STUN client, candidate gathering, punch probe
  lobby_client.{h,cpp}      — NEW: TCP/TLS client, lobby protocol marshalling

FM2K_LauncherUI.cpp         — lobby browser UI, match queue
FM2K_RollbackClient.cpp     — lobby-driven session start path

lobby-server/               — NEW: separate project
  main.go                   — TCP/TLS server, lobby directory
  stun.go                   — UDP STUN responder
  relay.go                  — UDP packet forwarder
  proto/*.go                — wire format
```

---

## 13. Reference implementations

We don't need to design from first principles — proven implementations exist in the adjacent projects.

### 13.1 `bbbr/revolve_input_sdl3/src/hooks/network/bbbr_holepunch.{h,cpp}`
**1207 lines of working C++ hole-punch client.** Already solves the "UDP hole-punch on an existing game socket" problem. Key techniques documented in `HOLE_PUNCH_IMPROVEMENTS.md`:

- **Burst punching over continuous**: 30 packets in ~30ms with `Sleep(1)` between, instead of 30 seconds of trickle. Connects in <1s typically. Matches Touhou AOCF's proven approach.
- **Windows priority boost during punch**: `SetPriorityClass(HIGH_PRIORITY_CLASS)`, `SetThreadPriority(THREAD_PRIORITY_TIME_CRITICAL)`, `timeBeginPeriod(1)` for 1ms timer. Without this, Windows scheduler adds 10-15ms jitter that breaks Port-Restricted NAT traversal.
- **Auto-stop on success**: `hooked_recvfrom` detects first inbound game packet from opponent IP → sets `g_punching_active = false`. Saves bandwidth, cleaner logs.
- **NAT coverage achieved**: Full Cone / Restricted / Port-Restricted all work reliably. Symmetric requires delay measurement (relay is simpler).

We should **port this file wholesale** as the starting point for `nat_traversal.cpp`, adapt socket access to go through our `NetSocket_GetHandle`. Rewriting from scratch would be wasted work.

### 13.2 `bbbr/revolve_input_sdl3/punch-check/` (Go)
**Complete Go NAT-probe project** (from delthas, autopunch author): client + server + relay + client-gui. Roughly 1200 lines total:

| File | Lines | Role |
|------|-------|------|
| `common.go` | 170 | Wire format, shared types |
| `client/client.go` | 143 | Client-side probe + punch driver |
| `client-gui/client.go` | 166 | GUI variant |
| `relay/relay.go` | 174 | Stateless UDP relay |
| `server/server.go` | 548 | Coordination server (STUN + pairing) |

This is almost exactly the lobby-server architecture we want. The relay is 174 lines — actually trivial. Server is 548 lines — covers STUN reflection + peer pairing + relay handoff.

We should **fork punch-check as the base for our lobby server**, extend it with:
- Persistent lobby list (their server is per-session only)
- TLS on the coordination channel (they use plain TCP)
- Account layer (Phase 4)

### 13.3 Lessons from `HOLE_PUNCH_IMPROVEMENTS.md`
From battle-tested implementations studied (Touhou AOCF, squiroll, autopunch):
1. **Priority boosting is non-negotiable** — every working implementation does it.
2. **Burst beats continuous** — faster and more reliable.
3. **Keep delay measurement optional** — only helps symmetric NAT; relay is cleaner.

These inform Phase 1 of our plan (§9): port bbbr_holepunch.cpp's burst/priority approach from day one, don't start with continuous punching.

---

## 14. Not in scope (for now)

- **UPnP** — explicitly rejected. Fails on ~50% of home routers, requires elevated privileges on some, can't do relay fallback. Hole-punch + relay subsumes it.
- **IPv6** — worth adding later, not critical. Hole-punch works the same.
- **Native TCP fallback** — for corp firewalls blocking UDP. Phase 6+.
- **Tournament brackets, leaderboards** — separate service, not matchmaking core.

---

## 15. Phase-1 actual: Python hub.py + WebSocket lobby

The original plan in §9 / §13 was to fork punch-check (Go). We pivoted to Python (`hub/hub.py`) for iteration speed; the protocol is JSON over WebSocket. This section reconciles the design with what's now scaffolded.

### 15.1 What's in `hub/hub.py` today

- WebSocket lobby on `ws://hub:7711/`. JSON message catalogue lives in §15.2.
- Per-game rooms (`room_id == game_id`). Idle/challenging/in_match presence.
- Hub-to-client RTT measurement (5-second `ping`/`pong`). Pairwise RTT in the room view is approximated as `rtt_a + rtt_b` until Phase 2 peer-direct probing lands.
- Match coordination: when `accept_challenge` lands, the hub sends both peers a `match_start` carrying:
  - `peer.udp_addr` — the peer's reported listen `(ip, port)` for the **single multiplexed UDP socket** (see §15.3).
  - `peer.ws_addr` — the peer's TCP/WebSocket source address as observed by the hub. Useful as a fallback hint and for the reflexive-IP cross-check.
  - `token` — shared session token, included in punch probes so peers can authenticate the punch.
  - `role: "host" | "guest"` — the challenger is host, the accepter is guest. Determines who initiates the GekkoNet `gekko_add_actor` ordering.

### 15.2 Hub message types (current)

| Direction | `type` | Body | Notes |
|---|---|---|---|
| C→S | `hello` | `{nick}` | First message; server replies `hello_ack` |
| C→S | `udp_addr` | `{ip, port}` | Client's UDP listen — must match the socket used for game traffic (§15.3) |
| C→S | `list_rooms` / `join_room` / `leave_room` | | |
| C→S | `challenge` / `cancel_challenge` / `accept_challenge` / `decline_challenge` | | |
| C→S | `match_ended` | | |
| S→C | `room_list` / `room_joined` / `user_joined` / `user_left` / `user_status` / `user_rtt` | | |
| S→C | `match_start` | `{token, role, peer:{id, nick, udp_addr, ws_addr}}` | Triggers punch sequence (§15.4) |
| S↔C | `ping` / `pong` | | RTT cadence |

### 15.3 Single-socket multiplex (HARD requirement)

The same UDP socket the hook opens at process start must serve, in order over its lifetime:

1. STUN-style address reflection (`udp_addr` → hub).
2. NAT punch probes to the peer (§15.4).
3. The existing `0xCC` control channel (HELLO/HELLO_ACK/BATTLE_*).
4. The existing `0xCE` spectator-tree datagrams.
5. GekkoNet input/state packets via `MultiplexAdapter`.

Implementation lives in `FM2KHook/src/netplay/control_channel.cpp:RawReceive()` — its first-byte demux already routes `0xCC` and `0xCE` to handlers and forwards everything else to GekkoNet's adapter queue. We need to add a **`0xCD` punch demux branch** here that consumes punch probes (calls into `nat_traversal.cpp`) without polluting GekkoNet's queue. No new socket; no rebind.

The hub never sees the UDP traffic — it only learns `(ip, port)` via the `udp_addr` message and relays it. Reflexive IP comes from the WebSocket TCP source address (`peer.ws_addr` in `match_start`). For most NATs this matches what the peer's UDP socket emits; full STUN reflection (§7.2) is a Phase-2 addition for the rare cases where TCP and UDP get different mappings.

### 15.4 Punch sequence (peer-side state machine)

Triggered by `match_start`:

```
state PUNCHING:
  for ~30 iterations, ~10ms apart:
    send 0xCD CTRL_PUNCH { token } -> peer.udp_addr
    if recv 0xCD CTRL_PUNCH from peer with matching token:
      peer_addr := from_addr
      goto CONNECTED

  # if 30 iterations elapsed with no peer punch received:
  goto RELAY_REQUEST    # Phase 3, currently goto FAILED
```

- **Burst, not trickle** (per §13.1): 30 packets over ~300 ms is the proven shape from `bbbr_holepunch.cpp`. Don't slowly trickle.
- **Priority boost during the 300 ms punch window**: `timeBeginPeriod(1)` + `THREAD_PRIORITY_TIME_CRITICAL`. Without this, Windows scheduler jitter (~10-15 ms) breaks port-restricted NAT pairs that need synchronized pings.
- **Both peers must enter PUNCHING simultaneously**. The hub fires `match_start` to both at the same instant (single-threaded async loop), so wall-clock skew is bounded by transport delay (~RTT/2). This is good enough for the first probe; subsequent retries cover any residual skew.
- **Token in payload** authenticates the punch. The hub generates a random token per match and includes it in both peers' `match_start`. A peer rejects punch packets whose token doesn't match — prevents stale punches from a previous session leaking into a new one (the same socket gets reused across matches, so OS UDP buffer can hold zombie packets).
- **First good packet from peer latches the address**. Existing `control_channel.cpp` peer-learning slot does exactly this for `0xCC` HELLO. The punch path uses the same latch — once `peer_addr` is set, HELLO/HELLO_ACK and then GekkoNet just work without re-binding.

### 15.5 What the hub does NOT do

To keep the trust boundary clean:
- **No game state forwarding** in non-relay mode. Hub never sees inputs, RNG, save state.
- **No emulation of STUN responder.** The hub uses the WebSocket TCP source for the reflexive IP hint; if more accurate reflection is needed (Phase 2), we add a tiny separate STUN service alongside the hub.
- **No persistent identity** in Phase 1. `user_id` is a per-session UUID. Phase 2 adds accounts.

### 15.6 Files this lands in

```
hub/
  hub.py                       — done (Phase 1)
  hub_relay.py                 — Phase 3, stateless UDP forwarder

FM2KHook/src/netplay/
  control_channel.cpp          — extend RawReceive() with 0xCD branch
  nat_traversal.{h,cpp}        — NEW: punch driver, port from bbbr_holepunch.cpp
                                 (adapt socket access to NetSocket_GetHandle)

FM2K_RollbackClient.cpp        — HubClient (WS transport) + match_start handler
FM2K_LauncherUI.cpp            — Hub panel: real room_list / user_list rendering
                                 (replace TODO markers added in cde564a)
```

Settings work (server URL, account creds in Phase 2) goes in a separate `RenderSettings()` panel — out of scope for this section.
