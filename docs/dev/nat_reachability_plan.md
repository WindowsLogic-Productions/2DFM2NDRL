# NAT Reachability Overhaul -- design + phased plan

Date: 2026-06-12
Author: design by Fable 5 (orchestrator), implementation by Opus 4.8 agents
Status: Phase 0 SHIPPED (hub live, client floor 0.2.73-bleeding).
        Phase 1 committed (wanwan ccaf3db, hub a0df2be pushed); hub
        a0df2be pulled to VPS but NOT yet restarted (held for a quiet
        window, dormant until a UPnP launcher ships). Launcher release
        TBD. Phases 2-3 not started.

## Goal

Everyone can connect to everyone. Direct P2P whenever physically possible;
hub relay strictly as the last-resort floor for the ~0.01% of pairings that
no P2P scheme can solve (both sides CGNAT / double-NAT / unmappable +
unpunchable). A working direct connection must NEVER be routed through the
relay.

## Non-goals

- Symmetric-NAT port prediction (birthday-paradox punching): complex,
  unreliable, made mostly moot by UPnP. Revisit only if telemetry shows a
  residual population after Phase 3.
- Hook reflexive-port propagation: peer-learning already self-corrects on
  the first through-packet (control_channel.cpp:507). Deferred.

## Governing principle

A match needs only ONE reachable side. The unreachable side connects
outbound; the reachable side learns the real source ip:port from the first
inbound datagram (already implemented: control_channel.cpp:507
"Learned peer address") and replies there. So the whole game is:
(1) make at least one side reachable -- UPnP (Phase 1),
(2) know who is reachable -- NAT classification (Phase 2),
(3) orient the match toward the reachable side and gate relay to provably
    hopeless pairings only (Phase 3),
(4) keep relay as the floor so nobody ever gets a dead match (Phase 0).

## Pairing matrix

| pairing                                         | fixed by                       | relay? |
|--------------------------------------------------|--------------------------------|--------|
| cone x cone (~80%)                                | hole-punch (works today)       | no     |
| symmetric x full/restricted-cone                  | punch + peer-learning (today)  | no     |
| symmetric x port-restricted-cone                  | UPnP on either side            | no*    |
| symmetric x symmetric                             | UPnP on either side            | no*    |
| anything x UPnP-mapped                            | explicit inbound mapping       | no     |
| both sides CGNAT/double-NAT, no UPnP effective    | nothing (physically impossible)| YES    |

(*) falls to relay only when neither side has working UPnP/NAT-PMP.
Note: a UPnP/NAT-PMP mapping accepts ANY source address, so one mapped side
makes the pairing work regardless of the other side's NAT behavior.

## Current state (verified 2026-06-12, file:line)

- One UDP socket per game does everything: STUN, punch, control channel
  (0xCC), GekkoNet, NAT layer (0xCD), relay envelope (0xCF). Binds
  FM2K_LOCAL_PORT (default 7000, UI-configurable). control_channel.cpp:190.
- Spectator TCP listener on udp_port+100 (fallback +1000, then OS-pick).
  spectator_node.cpp:1499. Skipped when FM2K_SPEC_TRANSPORT=relay.
- Launcher preflight (LauncherStunProbe + HubPreflightPunch) binds the same
  port and closes before spawn; hook re-binds with a 10s retry loop.
  FM2K_LauncherUI.cpp:232,357; control_channel.cpp:233.
- Hub STUN responder UDP 7711 learns reflexive from datagram source, stores
  user.udp_addr (hub.py:1759). match_start advertises peer as
  [WS-source-IP, STUN-learned-port], fallback port 7000 (hub.py:1239).
- TCP-STUN responder 7713 (hub.py:1911). UDP relay responder 7712
  (hub.py:1858), STARTED in main (hub.py:2288), advertise address in
  RELAY_LISTEN = (advertise_host, port+1) (hub.py:2287).
- Relay data plane is FULLY BUILT and dormant end-to-end:
  - hub: _RelayProto + RelaySession.route() learns both peers dynamically
    from first packets, forwards by 16-byte session id (hub.py:1834-1878).
  - hook send: RawSend wraps everything in [0xCF 0x01 session16 payload]
    to g_relay_addr when relay mode on (control_channel.cpp:368-399).
  - hook recv: RawReceive unwraps 0xCF, processes inner as-if-direct, and
    SKIPS peer-addr learning for relayed packets (control_channel.cpp:478).
  - session id derivation matches both sides (match token hex -> 16 bytes).
  - Engagement is strictly direct-first: 2s grace after punch burst, skip
    if 0xCC handshake completed, and DISENGAGE if a direct punch lands late
    (nat_traversal.cpp:271-300, 358-370).
  - The ONLY gap: hub.py:1226 no longer puts the relay block in match_start
    ("hole-punch only"), so ConfigureRelay() never sees the env vars.
- Client relay parsing/env plumbing (FM2K_HubClient.cpp:1031,
  FM2K_LauncherUI.cpp:5783) shipped in commit 29e2a48 which IS in
  stable/0.2.71 -> Phase 0 is hub-deploy-only, all current users benefit.
- RELAY_SESSIONS dict is never cleaned up (leak; fix in Phase 0).
- No UPnP/NAT-PMP/PCP anywhere in the codebase.
- No NAT classification anywhere; hub has a single UDP STUN port.

## Locked design decisions

- D1 Relay policy: Phase 0 advertises relay on every match_start;
  engagement stays punch-fail-only (existing hook logic). Phase 3 gates
  the advertisement itself off when both sides are provably
  direct-capable. Relay is last-resort by construction at every stage.
- D2 UPnP library: miniupnpc (UPnP-IGD v1/v2), vendored as a git submodule
  (repo convention), static lib, MinGW i686. PortMapper interface is
  backend-pluggable; libnatpmp (NAT-PMP/PCP) is a fast-follow backend.
- D3 Mapping moment: launcher maps once when the hub WS connects
  (Connected event), keeps it alive across matches with a renewal thread
  (lease 1800s, renew at half-life), unmaps on launcher exit. Never for
  LOCAL/offline sessions or loopback peers.
- D4 External port selection: request ext==local first; on conflict retry
  local+1000, then 3 random high ports; advertise whatever was granted.
- D5 Advertisement schema: extend the existing udp_addr WS message with
  optional fields: "ext_udp_port": int, "upnp": bool, "nat":
  "cone"|"symmetric"|"blocked"|"cgnat"|"unknown". Hub endpoint precedence
  for match_start: verified-UPnP ext_port > STUN-learned > client-claimed.
  Old hubs ignore unknown fields; old clients simply do not send them.
- D6 CGNAT detection: if IGD-reported external IP != hub WS-source IP, the
  router's WAN side is private -> mapping is useless -> upnp=false,
  nat hint "cgnat".
- D7 Second STUN port: hub UDP base+3 (7714 default; 7712 relay, 7713
  TCP-STUN are taken). Classification = RFC-4787 mapping test: probe both
  ports from one local socket, same external port -> endpoint-independent
  (cone), different -> symmetric, no replies -> blocked.
- D8 Reachability verification (Phase 2): hub actively probes the claimed
  endpoint from its STUN socket (new 0xCD tag REACH_CHECK 0x03 + nonce);
  launcher reports receipt via WS "reach_ack". Verified flag drives D5
  precedence and Phase 3 scoring. Trust-but-verify: an unverified UPnP
  claim is still harmless because punch + peer-learning + relay remain.
- D9 No hook changes through Phases 0-2 (keeps the determinism surface and
  replay/netplay paths untouched). Phase 3 adds at most a relay-policy env.
  AMENDED 2026-06-12: one relay-mode-only hook fix allowed in Phase 0 (see
  the CSS-stall bug below); it is guarded by from_relay so the direct path
  stays bit-identical.
- D10 Rollout: every change lands on bleeding first. Hub changes must stay
  backward-compatible with stable 0.2.71 clients (verified for Phase 0).

## Phases

### Phase 0 -- relay floor ON, gated (hub-only, immediate)

Changes (hub/hub.py):
1. match_start: add to BOTH guest_msg and host_msg:
   "relay": {"addr": [RELAY_LISTEN[0], RELAY_LISTEN[1]], "session_id": token}
   (relay_addr var at hub.py:1246 is already computed and unused).
   Launcher parser expects addr as ["ip-or-host", port] (FM2K_HubClient.cpp:1034).
2. RELAY_SESSIONS lifecycle: stamp creation time on RelaySession; delete on
   match end/commit/disconnect (wherever IN_FLIGHT_MATCHES is retired) plus
   a periodic sweep (>6h old). Fixes the unbounded-growth leak.
3. --test-relay-session <hex32> CLI arg: pre-register a session id so test
   harnesses can exercise the relay without WS matchmaking.
4. Operational: hub should be deployed with an IP (or pre-resolved)
   advertise_host so stable clients do not do DNS inside the hook.
   Bleeding launcher additionally pre-resolves relay_ip before setting
   FM2K_HUB_RELAY_ADDR (same pattern as the FM2K_HUB_UDP_ADDR
   pre-resolution at FM2K_LauncherUI.cpp:5712).

Test (acceptance):
- tools/replay_netplay_selftest.py --relay mode: local hub (or minimal
  relay stub if websockets dep unavailable) with pre-registered session;
  both peers get FM2K_HUB_RELAY_ADDR=127.0.0.1:7712 +
  FM2K_HUB_RELAY_SESSION, remote addr pointed at blackhole 192.0.2.1 so
  direct punch fails. PASS = both logs show "relay mode ENGAGED" then
  "Full handshake complete - CONNECTED!", match completes through CSS ->
  battle, harness parity checks green.
- Regression: standard (non-relay) selftest still green.

Deploy: user restarts hub service. IMPORTANT findings from the first
end-to-end exercise of the dormant relay path (2026-06-12):

- CSS-STALL BUG (hook, present in stable 0.2.71): RawReceive queues
  relayed GekkoNet packets with the relay's sockaddr (recvfrom source)
  instead of the peer's (control_channel.cpp:575). GekkoNet drops packets
  whose source string mismatches the gekko_add_actor address
  (netplay.cpp:2501), so relayed matches handshake fine then stall at CSS
  lockstep forever. Fix: stamp relayed gekko-queue packets with the
  configured peer sockaddr (relay-mode-only, from_relay guard). Ships in
  0.2.72.
- VERSION GATE (hub): because 0.2.71 clients carry the bug, the hub
  advertises the relay block ONLY when both peers' client_version >=
  RELAY_MIN_CLIENT_VERSION (0.2.72). Older pairs keep today's clean
  fast-fail instead of a CSS hang. The hub already blocks cross-version
  matches, so both peers always share a version at match_start.
- ADVERTISE HOST: prod runs --advertise-host hub.2dfm.org (DNS name,
  verified in fm2k-hub.service ExecStart 2026-06-12). Safe AFTER the
  version gate: only 0.2.72+ clients receive the relay block and their
  launcher pre-resolves it before env injection, so no DNS ever reaches
  the hook's DllMain path. No service-file change needed.
- HUB DEPLOY FLOW (verified 2026-06-12): hub/ inside wanwan is a NESTED
  git checkout of Armonte/fm2k-hub (wanwan gitignores /hub). Local
  checkout == origin/master == server checkout (/home/fm2k/hub), all at
  33b6052. Deploy = commit in hub/, push to fm2k-hub master, then on the
  VPS (ssh 2dfm): cd /home/fm2k/hub && git pull && sudo systemctl
  restart fm2k-hub. Server-side untracked runtime files (matches.json,
  .env*) are expected; tracked files carry no hot edits.
- The original "no client release needed" claim is therefore WRONG: the
  relay floor reaches users only as they update to 0.2.72+. The hub
  deploy is still safe to do immediately thanks to the version gate.

### Phase 1 -- UPnP port mapping (launcher-only, the hero)

Changes:
1. vendored/miniupnp (submodule), static build of miniupnpc for
   i686-w64-mingw32 in CMakeLists (MINIUPNP_STATICLIB; link ws2_32,
   iphlpapi). UPNPC_BUILD_STATIC=ON, SHARED/TESTS/SAMPLE off.
   IMPLEMENTED 2026-06-12: submodule pinned to tag miniupnpc_2_3_3
   (commit bf4215a). Built with a HAND-ROLLED add_library(miniupnpc STATIC)
   listing the 14 canonical MINIUPNPC_SOURCES (mirrors upstream's own
   library set) rather than add_subdirectory, so we never pull in the
   shared lib / upnpc+listdevices samples / test suite / python module /
   install rules. miniupnpcstrings.h is configure_file-generated from the
   upstream .h.cmake template into the build dir (no hand-edited copy that
   could drift from the pin). MINIUPNP_STATICLIB is PUBLIC on the target so
   the wrapper sees the dllimport-free declarations.
2. New FM2K_PortMapper.{h,cpp} (launcher):
   - StartAsync(udp_port [, tcp_port]) -> background thread: SSDP discover
     (2s budget), select valid IGD, GetExternalIPAddress, AddPortMapping
     UDP (+TCP optional later), lease 1800s, renewal at half-life.
   - Status() -> {state: idle|discovering|mapped|no_igd|failed|cgnat,
     ext_ip, ext_udp_port, backend}.
   - Stop() -> DeletePortMapping + thread join. Called on launcher exit.
   - Backend interface so libnatpmp slots in later.
3. Wire-in at hub Connected event (FM2K_LauncherUI.cpp:5300): start
   mapping asynchronously; SendUdpAddr fires immediately as today, then
   when the mapping completes the launcher RE-SENDS udp_addr with the
   ext fields (the hub already accepts udp_addr updates at any time, and
   STUN re-sends the same way). Poll PortMapper status from the UI loop
   and re-send on state transition. CGNAT check per D6.
3b. Router quirk handling (pinned): UPnP error 725
   (OnlyPermanentLeasesSupported) -> retry AddPortMapping with lease 0
   and rely on Stop()'s DeletePortMapping; error 718
   (ConflictInMappingEntry) -> try local+1000, then 3 alternate high
   ports per D4. Mapping description string: "FM2K Rollback". Env escape
   hatch FM2K_NO_UPNP=1 skips the whole subsystem.
3c. Phase 1 precedence note: an (unverified) UPnP claim outranks the
   STUN-learned port at the hub -- an explicit any-source inbound mapping
   beats an observed outbound mapping when they differ, and punch +
   relay still back it up if the claim is stale. Phase 2's REACH_CHECK
   upgrades this to verified.
4. hub.py: parse new udp_addr fields; store on User; peer_dict precedence
   per D5.
5. UI: small status in the lobby/network area: "Port: open via UPnP
   (ext 7000)" / "closed (UPnP unavailable)". Keep it one line.
6. --upnp-test launcher CLI flag: discover -> map -> query -> unmap with
   verbose logs, for manual router validation.
7. Skip entirely for LOCAL session mode / loopback remotes / offline.

Test:
- --upnp-test on the user's real router (manual).
- Loopback regression: full selftest matrix unaffected (UPnP skipped on
  loopback by design).
- Live: a hub match where one side previously failed (the affected user
  from FM2K_P1_Debug(10).log is the ideal guinea pig).

Effort: M-L. Risk: medium (router variance) but strictly best-effort:
any failure falls through to exactly today's behavior.

### Phase 2 -- NAT classification + reachability verification

Changes:
1. hub.py: second UDP STUN listener on base+3 (7714), same _StunProto.
2. Launcher LauncherStunProbe: probe 7711 AND 7714 from the same socket,
   compare reflexive ports -> nat type (D7); send in udp_addr.
3. hub.py: REACH_CHECK active probe (D8) after receiving a UPnP claim or
   on demand; launcher WS "reach_ack" handler; reach_verified on User.
4. match_start includes both sides' nat type (peer dict) for hook logs.
5. UI: show own NAT type; optionally a "can host" badge in the user list.

Test:
- Loopback => cone classification, plumbing verified.
- Symmetric emulation (stretch): Linux netns + iptables MASQUERADE
  --random on a WSL bridge to validate the symmetric branch end-to-end.
- Live telemetry: hub logs nat type distribution.

Effort: M. Risk: low.

### Phase 3 -- anchor orientation + relay gating to the floor

Changes (hub.py + tiny launcher passthrough):
1. Reachability score per user: upnp_verified(3) > cone(2) > symmetric(1)
   > blocked/cgnat(0).
2. match_start: include "anchor": "host"|"guest" (higher score side).
   Launcher exposes to hook as env for logging/diagnostics.
3. Relay gating: OMIT the relay block entirely when min-score pairing is
   provably direct-capable (both >=2, or either side verified-UPnP).
   Include it only for at-risk pairings. Result: relay is structurally
   unreachable for everyone who can go direct; engagement logic in the
   hook stays as-is for the rest.
4. Telemetry: launcher reports "match_path": direct|relay once connected
   (WS message); hub logs + counts. This measures the 0.01% target.
5. Optional hook env FM2K_RELAY_POLICY=never|auto for user override.

Test: pairing-matrix unit tests for the scoring/gating function (pure
python); live soak on bleeding.

Effort: S-M. Risk: low.

## Orchestration model

- Fable 5 (main session): architecture, this doc, work-package specs,
  diff review, builds (./make_build.sh + ./go.sh), harness runs, release
  notes. No implementation by agents is merged without review here.
- Opus 4.8 subagents: one per work package, given exact file:line specs
  from this doc. No commits; working-tree edits only; never touch the
  user's uncommitted WIP (trampoline/netplay/globals dirty files); build
  on top of dirty tools/replay_netplay_selftest.py, never revert it.
- User: hub deploys (VPS), real-router UPnP tests, live-match validation
  with affected users, stable promotion decisions.
- Both test paths (replay determinism AND real netplay) must stay green
  per feedback_replay_vs_netplay_determinism_tradeoff.

## Risks / notes

- Router UPnP variance is the big unknown in Phase 1; mitigated by
  --upnp-test, best-effort fallthrough, and libnatpmp fast-follow.
- The launcher preflight/STUN socket handoff already has a 10s bind-retry
  in the hook; PortMapper does not bind sockets (mappings are router-side)
  so it adds no new handoff hazards.
- Multi-instance local testing never maps (LOCAL mode skip), so no
  ext-port collisions from dual clients.
- Spectator TCP (udp+100) mapping is deliberately deferred until the UDP
  path is proven; TCP-STUN + spec relay transport already cover specs.
- Relay bandwidth: input-sized packets at 100fps are tens of kbps per
  direction per relayed match; trivial for the VPS at expected volume.
