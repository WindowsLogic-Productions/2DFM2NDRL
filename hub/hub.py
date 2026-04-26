"""
FM2K Hub — minimal Fightcade-style lobby/matchmaking server.

Phase 1 scope (LilithPort parity):
  - Connect, declare nick + supported games
  - Join/leave per-game rooms (room_id == game_id)
  - List rooms, list users in your room
  - Challenge another user (target must be idle)
  - Accept/decline; on accept, server sends both clients each other's
    learned ip:port + a match_token. Clients then do NAT traversal
    + GekkoNet handshake on their own.
  - Server pings every 5s, publishes per-user RTT to the room
  - Status broadcast: idle | challenging | in_match (with opponent)

Out of scope (Phase 2+): accounts/auth, persistence, replay upload,
friends, blocking, news/MOTD, Elo, peer-to-peer probe RTT.

Run:    python hub.py [--port 7711]
Deps:   pip install websockets
"""

import argparse
import asyncio
import json
import socket
import struct
import time
import uuid
from typing import Any, Optional
from websockets.asyncio.server import serve, ServerConnection
from websockets.exceptions import ConnectionClosed


USERS: dict[str, "User"] = {}
ROOMS: dict[str, "Room"] = {}

# Pre-populated rooms — visible to clients on connect even before
# anyone joins. Keyed by canonical game id (later: master game list
# will provide stable ids). Display name shown in the launcher UI.
SEED_ROOMS: list[tuple[str, str]] = [
    ("pkmncc",            "Pokemon: CardCaptor"),
    ("SCWU",              "SCWU Infinity"),
    ("WonderfulWorld",    "Wonderful World"),
    ("StudioS_Fighters",  "StudioS Fighters"),
    ("STRIPFIGHTER_ZERO", "Strip Fighter Zero"),
]


class User:
    def __init__(self, ws: ServerConnection, nick: str, peer_addr: tuple[str, int]):
        self.id = uuid.uuid4().hex[:12]
        self.ws = ws
        self.nick = nick
        self.peer_addr = peer_addr  # (ip, tcp_port_of_websocket)
        self.udp_addr: Optional[tuple[str, int]] = None  # learned later, when client tells us
        self.room_id: Optional[str] = None
        self.status = "idle"           # idle | challenging | in_match
        self.opponent_id: Optional[str] = None
        self.rtt_ms = 0
        self.pending_ping_ts = 0.0     # 0 = not waiting

    def to_dict(self) -> dict[str, Any]:
        return {
            "id": self.id,
            "nick": self.nick,
            "room_id": self.room_id,
            "status": self.status,
            "opponent_id": self.opponent_id,
            "rtt_ms": self.rtt_ms,
        }


class Room:
    def __init__(self, room_id: str, name: str, *, seeded: bool = False):
        self.id = room_id
        self.name = name
        self.seeded = seeded     # if True, do not delete when last user leaves
        self.user_ids: set[str] = set()

    def to_dict(self) -> dict[str, Any]:
        return {"id": self.id, "name": self.name, "user_count": len(self.user_ids)}


async def send(user: User, msg: dict[str, Any]) -> None:
    try:
        await user.ws.send(json.dumps(msg))
    except ConnectionClosed:
        pass


async def broadcast_room(room: Room, msg: dict[str, Any], exclude: Optional[str] = None) -> None:
    payload = json.dumps(msg)
    for uid in list(room.user_ids):
        if uid == exclude:
            continue
        u = USERS.get(uid)
        if u is None:
            continue
        try:
            await u.ws.send(payload)
        except ConnectionClosed:
            pass


async def broadcast_all(msg: dict[str, Any], exclude: Optional[str] = None) -> None:
    """Send a message to every connected user."""
    payload = json.dumps(msg)
    for uid, u in list(USERS.items()):
        if uid == exclude:
            continue
        try:
            await u.ws.send(payload)
        except ConnectionClosed:
            pass


async def broadcast_room_list() -> None:
    """Push the current room list to every connected user. Used after
    any membership change so non-members of the room see the user_count
    update — and after seeded rooms gain/lose users — without needing
    to poll list_rooms."""
    msg = {"type": "room_list", "rooms": [r.to_dict() for r in ROOMS.values()]}
    await broadcast_all(msg)


async def leave_room(user: User) -> None:
    if user.room_id is None:
        return
    room = ROOMS.get(user.room_id)
    if room is not None and user.id in room.user_ids:
        room.user_ids.remove(user.id)
        await broadcast_room(room, {"type": "user_left", "room_id": room.id, "user_id": user.id})
        if not room.user_ids and not room.seeded:
            ROOMS.pop(room.id, None)
        # Push refreshed room list to everyone — user_count changed,
        # and an unseeded room may have just been deleted.
        await broadcast_room_list()
    user.room_id = None


async def set_status_and_broadcast(user: User, status: str, opponent_id: Optional[str]) -> None:
    user.status = status
    user.opponent_id = opponent_id
    if user.room_id and user.room_id in ROOMS:
        await broadcast_room(ROOMS[user.room_id], {"type": "user_status", "user": user.to_dict()})


async def handle_message(user: User, msg: dict[str, Any]) -> None:
    t = msg.get("type")

    if t == "udp_addr":
        # Client tells us where it's listening for the actual game UDP traffic.
        # We don't validate — clients can lie; the GekkoNet handshake will fail
        # if they do, which is their problem. The hub only relays addresses.
        ip = msg.get("ip"); port = msg.get("port")
        if isinstance(ip, str) and isinstance(port, int):
            user.udp_addr = (ip, port)

    elif t == "list_rooms":
        await send(user, {"type": "room_list", "rooms": [r.to_dict() for r in ROOMS.values()]})

    elif t == "join_room":
        gid = msg.get("game_id")
        if not isinstance(gid, str):
            return
        if user.room_id is not None:
            await leave_room(user)
        room = ROOMS.get(gid)
        if room is None:
            room = Room(gid, msg.get("name") or gid)
            ROOMS[gid] = room
        room.user_ids.add(user.id)
        user.room_id = gid
        await send(user, {
            "type": "room_joined",
            "room": room.to_dict(),
            "users": [USERS[uid].to_dict() for uid in room.user_ids if uid in USERS],
        })
        await broadcast_room(room, {"type": "user_joined", "room_id": gid, "user": user.to_dict()},
                             exclude=user.id)
        # Refresh room list for everyone — user_count went up, and
        # a freshly user-created room (non-seeded) needs to appear in
        # the lobby browser of users not in any room.
        await broadcast_room_list()

    elif t == "leave_room":
        await leave_room(user)
        await send(user, {"type": "room_left"})

    elif t == "challenge":
        target_id = msg.get("target_id")
        target = USERS.get(target_id) if isinstance(target_id, str) else None
        if target is None or target.status != "idle" or target.id == user.id:
            await send(user, {"type": "challenge_failed", "target_id": target_id, "reason": "unavailable"})
            return
        await send(target, {"type": "challenge_received",
                            "from_id": user.id, "from_nick": user.nick,
                            "room_id": user.room_id})
        await set_status_and_broadcast(user, "challenging", target.id)

    elif t == "cancel_challenge":
        target_id = msg.get("target_id")
        target = USERS.get(target_id) if isinstance(target_id, str) else None
        if target is not None:
            await send(target, {"type": "challenge_cancelled", "by_id": user.id})
        await set_status_and_broadcast(user, "idle", None)

    elif t == "decline_challenge":
        challenger_id = msg.get("challenger_id")
        challenger = USERS.get(challenger_id) if isinstance(challenger_id, str) else None
        if challenger is not None:
            await send(challenger, {"type": "challenge_declined", "by_id": user.id, "by_nick": user.nick})
            if challenger.status == "challenging" and challenger.opponent_id == user.id:
                await set_status_and_broadcast(challenger, "idle", None)

    elif t == "accept_challenge":
        challenger_id = msg.get("challenger_id")
        challenger = USERS.get(challenger_id) if isinstance(challenger_id, str) else None
        if challenger is None or challenger.status != "challenging" or challenger.opponent_id != user.id:
            await send(user, {"type": "challenge_failed", "reason": "expired"})
            return
        token = uuid.uuid4().hex
        # Each side gets the OTHER side's addresses + a shared token.
        # IP comes from the WebSocket TCP source, NOT the client-supplied
        # value. Clients can lie about their IP, and the WS source is
        # accurate for the 95% case (cone NATs map UDP+TCP to the same
        # external IP). Port still comes from the client because UDP NAT
        # mapping is per-port and the launcher knows which one its game
        # will bind. The Phase-2 STUN responder will refine the port
        # for symmetric NAT cases.
        def peer_dict(u: "User") -> dict[str, Any]:
            ws_ip = u.peer_addr[0]
            udp_port = u.udp_addr[1] if u.udp_addr else 7000
            return u.to_dict() | {
                "udp_addr": [ws_ip, udp_port],
                "ws_addr":  list(u.peer_addr),
            }
        await send(user, {
            "type": "match_start",
            "token": token,
            "role": "guest",  # accepting side is the guest by convention
            "peer": peer_dict(challenger),
        })
        await send(challenger, {
            "type": "match_start",
            "token": token,
            "role": "host",   # original challenger hosts
            "peer": peer_dict(user),
        })
        await set_status_and_broadcast(user, "in_match", challenger.id)
        await set_status_and_broadcast(challenger, "in_match", user.id)

    elif t == "match_ended":
        opp_id = user.opponent_id
        opp = USERS.get(opp_id) if isinstance(opp_id, str) else None
        await set_status_and_broadcast(user, "idle", None)
        if opp is not None and opp.status == "in_match" and opp.opponent_id == user.id:
            await set_status_and_broadcast(opp, "idle", None)

    elif t == "pong":
        if user.pending_ping_ts > 0:
            user.rtt_ms = max(0, int((time.time() - user.pending_ping_ts) * 1000))
            user.pending_ping_ts = 0
            if user.room_id and user.room_id in ROOMS:
                await broadcast_room(ROOMS[user.room_id],
                                     {"type": "user_rtt", "user_id": user.id, "rtt_ms": user.rtt_ms})


# =============================================================================
# UDP STUN responder
# =============================================================================
# The hub binds a UDP socket on the same port as the WebSocket (TCP and UDP
# share namespaces independently — no conflict). It serves two purposes:
#
#   1. Reflexive-address discovery: a client sends a probe; we reply with the
#      (ip, port) we observed. This is the client's actual external UDP
#      mapping — the only correct value for cross-NAT play.
#   2. Implicit udp_addr learning: the probe carries the client's user_id
#      token; we update USERS[token].udp_addr from the source address. No
#      separate `udp_addr` WS message needed once STUN is wired everywhere.
#
# Wire format (1 message-type today, more later):
#
#   STUN probe (client -> hub):
#     0xCD 0x01 [16-byte hex user_id, padded right with NUL]
#
#   STUN ack (hub -> client):
#     0xCD 0x02 [4-byte ip_be] [2-byte port_be]
#
# Magic byte 0xCD is reserved for NAT-layer packets in the master design
# (see docs/FM2K_Matchmaking_Design.md §15.4). 0xCC is launcher control
# channel; 0xCE is spectator-tree datagrams.

STUN_MAGIC      = 0xCD
STUN_PROBE_TAG  = 0x01
STUN_ACK_TAG    = 0x02
STUN_USER_ID_LEN = 12  # matches User.id length (uuid4().hex[:12])

class _StunProto(asyncio.DatagramProtocol):
    def __init__(self) -> None:
        self.transport: Optional[asyncio.DatagramTransport] = None

    def connection_made(self, transport: asyncio.BaseTransport) -> None:
        self.transport = transport  # type: ignore[assignment]

    def datagram_received(self, data: bytes, addr: tuple[str, int]) -> None:
        if len(data) < 2 or data[0] != STUN_MAGIC:
            return
        if data[1] == STUN_PROBE_TAG:
            uid = data[2:2 + STUN_USER_ID_LEN].rstrip(b"\x00").decode("ascii", errors="ignore")
            user = USERS.get(uid)
            if user is not None:
                user.udp_addr = addr
                print(f"[STUN] {user.nick} ({uid}) -> {addr[0]}:{addr[1]}")
            try:
                ip_bytes = socket.inet_aton(addr[0])
                port_bytes = struct.pack("!H", addr[1])
                ack = bytes([STUN_MAGIC, STUN_ACK_TAG]) + ip_bytes + port_bytes
                if self.transport:
                    self.transport.sendto(ack, addr)
            except OSError:
                pass


async def start_stun_responder(host: str, port: int) -> None:
    loop = asyncio.get_running_loop()
    await loop.create_datagram_endpoint(
        _StunProto, local_addr=(host, port), reuse_port=False)
    print(f"FM2K Hub STUN responder listening on udp://{host}:{port}")


async def ping_loop() -> None:
    """Server pings each user every 5s. Two clients in the same room can
    estimate their pairwise RTT as ~ rtt_a + rtt_b (rough but cheap).
    Phase 2 should add direct UDP probing for accurate pairwise RTT."""
    while True:
        await asyncio.sleep(5.0)
        for u in list(USERS.values()):
            if u.pending_ping_ts > 0:
                # Stale ping — assume timeout, reset
                if time.time() - u.pending_ping_ts > 10.0:
                    u.pending_ping_ts = 0
                continue
            u.pending_ping_ts = time.time()
            try:
                await u.ws.send(json.dumps({"type": "ping"}))
            except ConnectionClosed:
                pass


async def handler(ws: ServerConnection) -> None:
    user: Optional[User] = None
    try:
        peer = ws.remote_address  # (ip, port)
        async for raw in ws:
            try:
                msg = json.loads(raw)
            except json.JSONDecodeError:
                continue
            if not isinstance(msg, dict):
                continue
            if user is None:
                if msg.get("type") != "hello":
                    await ws.send(json.dumps({"type": "error", "reason": "expected hello"}))
                    return
                nick = (msg.get("nick") or "anon").strip()[:24] or "anon"
                user = User(ws, nick, (peer[0], peer[1]) if peer else ("?", 0))
                USERS[user.id] = user
                await send(user, {
                    "type": "hello_ack",
                    "user_id": user.id,
                    "server_version": "0.1",
                    "rooms": [r.to_dict() for r in ROOMS.values()],
                })
                print(f"[+] {user.nick} ({user.id}) from {user.peer_addr}")
            else:
                await handle_message(user, msg)
    finally:
        if user is not None:
            await leave_room(user)
            opp = USERS.get(user.opponent_id) if user.opponent_id else None
            if opp is not None and opp.opponent_id == user.id:
                await set_status_and_broadcast(opp, "idle", None)
                await send(opp, {"type": "peer_disconnected"})
            USERS.pop(user.id, None)
            print(f"[-] {user.nick} ({user.id})")


async def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=7711)
    args = ap.parse_args()
    for game_id, display_name in SEED_ROOMS:
        ROOMS[game_id] = Room(game_id, display_name, seeded=True)
    asyncio.create_task(ping_loop())
    await start_stun_responder(args.host, args.port)
    async with serve(handler, args.host, args.port):
        print(f"FM2K Hub listening on tcp://{args.host}:{args.port} (WebSocket)")
        print(f"  seeded rooms: {[r.id for r in ROOMS.values()]}")
        await asyncio.Future()


if __name__ == "__main__":
    asyncio.run(main())
