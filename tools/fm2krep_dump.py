#!/usr/bin/env python3
"""
fm2krep_dump.py — decode and pretty-print a .fm2krep / .fm2kset file.

Usage: tools/fm2krep_dump.py <path-to-file>

Verifies the on-disk format produced by SpectatorNode_WriteCurrentBattleFile
(v2 256-byte FM2KSessionFileHeader + packed SessionEvent body). Prints:
- The header's decoded fields (magic, version, flags, counts, session_id,
  rounds_won, round_offsets[]).
- Every event in the body, in order, with its payload decoded.
- A summary at the end (event-type counts, frame-elapsed totals, etc).

Use this to verify rounds_won / hp_max / timer / session_id are populated
correctly without having to xxd + decode by hand.
"""

import struct
import sys


SESSION_FILE_MAGIC = 0x53534D46  # 'FMSS' little-endian
SESSION_FILE_VERSION = 2
HEADER_SIZE = 256

# Mirrors FM2KHook/src/netplay/spectator_node.h::SessionEventType.
EVENT_NAMES = {
    1: "INPUT",
    2: "PIN_RNG",
    3: "RESET_INPUT_STATE",
    4: "SOUND_INIT",
    5: "MATCH_START",
    6: "MATCH_END",
    7: "FINGERPRINT",
    8: "ROUND_START",
    9: "ROUND_END",
    10: "SESSION_ID",
}

EVENT_PAYLOAD_SIZE = {
    1: 4,   # INPUT (u16, u16)
    2: 4,   # PIN_RNG (u32 seed)
    3: 0,   # RESET_INPUT_STATE
    4: 0,   # SOUND_INIT
    5: 96,  # MATCH_START (96-byte ReplayHeader)
    6: 7,   # MATCH_END (winner_idx, rounds_won_p1, rounds_won_p2, frames_total)
    7: 4,   # FINGERPRINT (u32)
    8: 7,   # ROUND_START (round_idx, p1_hp_max, p2_hp_max, timer_seconds)
    9: 9,   # ROUND_END (winner_idx, p1_hp, p2_hp, frames_elapsed)
    10: 8,  # SESSION_ID (u64)
}


def fmt_winner(idx: int) -> str:
    return {0: "P1", 1: "P2", 2: "DRAW"}.get(idx, f"?({idx})")


def decode_header(buf: bytes) -> dict:
    if len(buf) < HEADER_SIZE:
        raise ValueError(f"Header truncated: {len(buf)} < {HEADER_SIZE}")
    fields = struct.unpack_from("<IHHQQII", buf, 0)
    magic, version, flags, started, finished, event_count, input_count = fields
    if magic != SESSION_FILE_MAGIC:
        raise ValueError(f"Bad magic 0x{magic:08x} (want 0x{SESSION_FILE_MAGIC:08x} 'FMSS')")
    if version != SESSION_FILE_VERSION:
        raise ValueError(f"Unsupported version {version} (want {SESSION_FILE_VERSION})")
    game_id = buf[0x20:0x40].rstrip(b"\x00").decode("ascii", errors="replace")
    p1_nick = buf[0x40:0x60].rstrip(b"\x00").decode("utf-8", errors="replace")
    p2_nick = buf[0x60:0x80].rstrip(b"\x00").decode("utf-8", errors="replace")
    p1_char_id, p2_char_id, p1_color, p2_color = struct.unpack_from("<BBBB", buf, 0x80)
    rounds_won_p1, rounds_won_p2, match_count, match_index = struct.unpack_from("<BBBB", buf, 0x84)
    session_id = struct.unpack_from("<Q", buf, 0x88)[0]
    round_count = buf[0x90]
    round_offsets = list(struct.unpack_from("<8I", buf, 0x94))
    return {
        "magic": "FMSS" if magic == SESSION_FILE_MAGIC else f"0x{magic:08x}",
        "version": version,
        "flags": flags,
        "is_battle_slice": bool(flags & 0x01),
        "has_round_offsets": bool(flags & 0x02),
        "started_at_unix": started,
        "finished_at_unix": finished,
        "event_count": event_count,
        "input_count": input_count,
        "game_id": game_id,
        "p1_nick": p1_nick,
        "p2_nick": p2_nick,
        "p1_char_id": p1_char_id,
        "p2_char_id": p2_char_id,
        "p1_color": p1_color,
        "p2_color": p2_color,
        "rounds_won_p1": rounds_won_p1,
        "rounds_won_p2": rounds_won_p2,
        "match_count": match_count,
        "match_index": match_index,
        "session_id": session_id,
        "round_count": round_count,
        "round_offsets": round_offsets[:round_count] if round_count <= 8 else round_offsets,
    }


def print_header(h: dict) -> None:
    print("=" * 72)
    print("FM2KSessionFileHeader (v2, 256 B)")
    print("=" * 72)
    print(f"  magic:              {h['magic']}")
    print(f"  version:            {h['version']}")
    print(f"  flags:              0x{h['flags']:04x} "
          f"(battle_slice={h['is_battle_slice']}, has_round_offsets={h['has_round_offsets']})")
    print(f"  started_at_unix:    {h['started_at_unix']}")
    print(f"  finished_at_unix:   {h['finished_at_unix']}")
    print(f"  event_count:        {h['event_count']}")
    print(f"  input_count:        {h['input_count']}")
    print(f"  game_id:            {h['game_id']!r}")
    print(f"  p1_nick / p2_nick:  {h['p1_nick']!r} / {h['p2_nick']!r}")
    print(f"  p1_char/color:      {h['p1_char_id']}/{h['p1_color']}")
    print(f"  p2_char/color:      {h['p2_char_id']}/{h['p2_color']}")
    print(f"  rounds_won:         {h['rounds_won_p1']}-{h['rounds_won_p2']}")
    print(f"  match_count:        {h['match_count']}")
    print(f"  match_index:        {h['match_index']}")
    print(f"  session_id:         0x{h['session_id']:016x}")
    print(f"  round_count:        {h['round_count']}")
    print(f"  round_offsets[]:    {[hex(x) for x in h['round_offsets']]}")
    print()


def decode_event(body: bytes, off: int) -> tuple[int, dict]:
    """Returns (bytes_consumed, decoded_event_dict)."""
    if off >= len(body):
        return 0, {}
    tag = body[off]
    if tag not in EVENT_NAMES:
        raise ValueError(f"Unknown event tag 0x{tag:02x} at offset 0x{off:x}")
    name = EVENT_NAMES[tag]
    payload_size = EVENT_PAYLOAD_SIZE[tag]
    if off + 1 + payload_size > len(body):
        raise ValueError(f"Event {name} truncated at offset 0x{off:x}")
    payload = body[off + 1:off + 1 + payload_size]
    ev = {"_tag": name, "_off": off}

    if name == "INPUT":
        p1, p2 = struct.unpack("<HH", payload)
        ev.update({"p1": p1, "p2": p2})
    elif name == "PIN_RNG":
        ev["seed"] = struct.unpack("<I", payload)[0]
    elif name == "FINGERPRINT":
        ev["hash"] = struct.unpack("<I", payload)[0]
    elif name == "MATCH_START":
        # 96-byte ReplayHeader-compatible payload
        magic, version = struct.unpack_from("<IH", payload, 0)
        game_hash = struct.unpack_from("<I", payload, 16)[0]
        seed = struct.unpack_from("<I", payload, 20)[0]
        state_hash = struct.unpack_from("<I", payload, 24)[0]
        p1_char, p1_color, p2_char, p2_color = struct.unpack_from("<BBBB", payload, 28)
        ev.update({
            "magic": f"0x{magic:08x}",
            "version": version,
            "game_hash": game_hash,
            "initial_rng_seed": seed,
            "initial_state_hash": state_hash,
            "p1_char/color": f"{p1_char}/{p1_color}",
            "p2_char/color": f"{p2_char}/{p2_color}",
        })
    elif name == "MATCH_END":
        winner_idx, rw_p1, rw_p2 = struct.unpack_from("<BBB", payload, 0)
        frames_total = struct.unpack_from("<I", payload, 3)[0]
        ev.update({
            "winner": fmt_winner(winner_idx),
            "rounds_won": f"{rw_p1}-{rw_p2}",
            "frames_total": frames_total,
        })
    elif name == "ROUND_START":
        round_idx = payload[0]
        p1_hp_max, p2_hp_max, timer = struct.unpack_from("<HHH", payload, 1)
        ev.update({
            "round_idx": round_idx,
            "p1_hp_max": p1_hp_max,
            "p2_hp_max": p2_hp_max,
            "timer_seconds": timer,
        })
    elif name == "ROUND_END":
        winner_idx = payload[0]
        p1_hp, p2_hp = struct.unpack_from("<HH", payload, 1)
        frames_elapsed = struct.unpack_from("<I", payload, 5)[0]
        ev.update({
            "winner": fmt_winner(winner_idx),
            "p1_hp": p1_hp,
            "p2_hp": p2_hp,
            "frames_elapsed": frames_elapsed,
        })
    elif name == "SESSION_ID":
        ev["session_id"] = f"0x{struct.unpack('<Q', payload)[0]:016x}"

    return 1 + payload_size, ev


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print(f"Usage: {argv[0]} <path-to-fm2krep-or-fm2kset>", file=sys.stderr)
        return 2

    path = argv[1]
    with open(path, "rb") as f:
        data = f.read()

    print(f"File: {path}  ({len(data)} bytes)")
    header = decode_header(data[:HEADER_SIZE])
    print_header(header)

    body = data[HEADER_SIZE:]
    off = 0
    counts: dict[str, int] = {}
    last_input_run = 0
    print("=" * 72)
    print("Body events (INPUT runs collapsed; non-INPUT printed individually)")
    print("=" * 72)
    while off < len(body):
        consumed, ev = decode_event(body, off)
        if consumed == 0:
            break
        name = ev["_tag"]
        counts[name] = counts.get(name, 0) + 1

        if name == "INPUT":
            last_input_run += 1
        else:
            if last_input_run > 0:
                print(f"  [INPUT × {last_input_run}]")
                last_input_run = 0
            details = " ".join(
                f"{k}={v}" for k, v in ev.items()
                if not k.startswith("_") and k not in ("magic", "version")
            )
            print(f"  @0x{ev['_off']:06x}  {name}: {details}")

        off += consumed
    if last_input_run > 0:
        print(f"  [INPUT × {last_input_run}]")

    print()
    print("=" * 72)
    print("Summary")
    print("=" * 72)
    print(f"  Total events parsed: {sum(counts.values())} (header.event_count = {header['event_count']})")
    for name in EVENT_NAMES.values():
        if name in counts:
            print(f"    {name:<18}  {counts[name]}")
    if off != len(body):
        print(f"  WARNING: body parse stopped at 0x{off:x}, file body ends at 0x{len(body):x}")

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
