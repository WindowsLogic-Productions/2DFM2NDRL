# fm2k-relay

A standalone, volunteer-hostable UDP relay for FM2K rollback netplay. It exists
to carry the rare matches that can't hole-punch (both peers behind symmetric
NAT) **and**, more importantly, to do it from a box **near the players** so the
relay path doesn't add a giant geographic detour. A pair in SEA relaying through
a US hub eats ~400-500ms RTT; a SEA-local relay collapses that to tens of ms.

## What it is NOT

It is **not** the hub. It carries no lobby, matchmaking, auth, database, or user
data, and it never sees any of the hub's source or secrets. It only forwards
opaque UDP between the two peers of a match the hub has authorized. The entire
operator surface is: this binary + one shared key + an open UDP port.

## What it does

- Listens UDP for `0xCF 0x01 [16-byte session_id] [payload]` packets.
- For each authorized session, learns the two peers (self-learning 2 slots) and
  forwards each packet to the other peer, as-is. Byte-identical to the hub's
  in-process relay.
- Forwards **only** sessions the hub authorized over a WSS control channel, so
  it is not an open relay. Caps: per-session rate limit, packet-size, idle TTL,
  3rd-source drop.

## Build

```
cd relay
go build -o fm2k-relay .
# cross-compile for a Linux box:
GOOS=linux GOARCH=amd64 go build -o fm2k-relay .
```

One static binary, no runtime deps (just the Go stdlib + gorilla/websocket).

## Run

```
./fm2k-relay \
  -hub    wss://hub.2dfm.org/ \
  -key    <shared-hex-key-from-the-hub-operator> \
  -region ap-southeast \
  -public relay.yourbox.example:7712
```

Then open the `-listen` UDP port (default `7712`) inbound on your firewall.
All flags also read from env (`FM2K_RELAY_HUB`, `FM2K_RELAY_REGISTER_KEY`,
`FM2K_RELAY_REGION`, `FM2K_RELAY_PUBLIC`, `FM2K_RELAY_LISTEN`).

| flag | default | meaning |
|---|---|---|
| `-hub` | (required) | hub control WSS URL |
| `-key` | (required) | shared HMAC registration key (hex or raw); get it from the hub operator |
| `-region` | `unknown` | region tag the hub uses for geo-selection (e.g. `ap-southeast`, `eu-west`) |
| `-public` | (WS source IP + port) | host:port clients dial; set if your public addr differs from the WS source |
| `-listen` | `:7712` | UDP listen address |
| `-rate-kbps` | `512` | per-session forward cap (0 = unlimited) |
| `-maxpkt` | `2048` | drop forwarded packets larger than this |
| `-session-ttl` | `21600` | idle-session eviction seconds |

## Security model

- **Registration** is HMAC-SHA256 authenticated with the shared key; without it
  the hub rejects the relay. The key is revocable and grants nothing but "may
  forward hub-blessed sessions."
- **Sessions** are gated: the relay forwards a `session_id` only after the hub
  pushes `relay_authorize_session` for it. Unknown ids are dropped.
- **Honest caveat:** a relay is a man-in-the-middle for the UDP it forwards.
  Payload is gekko inputs + checksums (not sensitive); the client desync-detects
  corruption and fails cleanly rather than being exploited. Run relays you trust.

## Status

Code-complete; **not yet compile/integration-tested** (the dev box has no Go
toolchain). Build + the loopback test live under task #6 / the Phase 4 test
plan in `docs/dev/nat_reachability_plan.md`.
