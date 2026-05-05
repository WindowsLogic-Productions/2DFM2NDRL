#!/usr/bin/env python3
"""Diff two FM2K parity .pty captures.

Aligns by KgtParitySnapshot.frame. Reports the first frame where ANY field
differs, and prints the diverging fields side-by-side. Walk forward from the
divergence to confirm whether it's a one-shot leak or a permanent drift.

Usage:
  python3 tools/parity_diff.py path/to/parity_p1.pty path/to/parity_p3.pty

Layout matches kgtengine/include/kgt/kgt_parity_snapshot.h v3.
"""

import os
import struct
import sys

HDR = struct.Struct('<4s I I I I I 8s')   # 32 B
PLAYER = struct.Struct('<12i 3i 16h')      # 92 B (12 i32 + 3 i32 + 16 i16)
assert PLAYER.size == 92, PLAYER.size
SNAP_HEAD = struct.Struct('<4I 4i')        # 32 B (frame, rng, in_p1, in_p2, phase, timer, cam_x, cam_y)
SNAP_TAIL = struct.Struct('<I 16h 2I')     # 4 + 32 + 8 = 44 B (rng_after, sysvars, pad)
assert SNAP_TAIL.size == 44, SNAP_TAIL.size
SNAP_SIZE = SNAP_HEAD.size + 2 * PLAYER.size + SNAP_TAIL.size  # 32 + 184 + 44 = 260
assert SNAP_SIZE == 260, SNAP_SIZE

PLAYER_FIELDS = [
    'script_idx', 'item_idx', 'pos_x', 'pos_y',
    'vel_x', 'vel_y', 'hp', 'super_meter',
    'facing', 'state_flags', 'hitstun', 'hitstop',
    'hit_box_active_count', 'hurt_box_active_count', 'cancel_script_item',
] + [f'task_vars[{i}]' for i in range(16)]

def parse_player(buf, off):
    fields = PLAYER.unpack_from(buf, off)
    return dict(zip(PLAYER_FIELDS, fields))

def parse_snap(buf, off):
    s = {}
    head = SNAP_HEAD.unpack_from(buf, off)
    keys_h = ['frame', 'rng', 'input_p1', 'input_p2',
              'match_phase', 'round_timer', 'camera_x', 'camera_y']
    s.update(dict(zip(keys_h, head)))
    s['p1'] = parse_player(buf, off + SNAP_HEAD.size)
    s['p2'] = parse_player(buf, off + SNAP_HEAD.size + PLAYER.size)
    tail = SNAP_TAIL.unpack_from(buf, off + SNAP_HEAD.size + 2 * PLAYER.size)
    s['rng_after_frame'] = tail[0]
    s['system_vars'] = list(tail[1:17])
    return s

def load(path):
    with open(path, 'rb') as f:
        data = f.read()
    if len(data) < HDR.size:
        raise SystemExit(f"{path}: too small")
    magic, version, snap_size, flags, frame_count, init_seed, _ = HDR.unpack_from(data, 0)
    if magic != b'KGTP':
        raise SystemExit(f"{path}: bad magic {magic!r}")
    if snap_size != SNAP_SIZE:
        raise SystemExit(f"{path}: unexpected snap_size {snap_size} (this tool expects v3 / 260)")
    print(f"{path}: version={version} flags=0x{flags:x} frame_count={frame_count} "
          f"init_seed=0x{init_seed:08x}")
    snaps = []
    pos = HDR.size
    while pos + snap_size <= len(data):
        snaps.append(parse_snap(data, pos))
        pos += snap_size
    print(f"  parsed {len(snaps)} snapshots ({len(data)} bytes)")
    return snaps

def diff_snap(a, b):
    """Returns list of (field_name, a_val, b_val) for differences."""
    out = []
    for k in ('rng', 'input_p1', 'input_p2', 'match_phase', 'round_timer',
              'camera_x', 'camera_y', 'rng_after_frame'):
        if a.get(k) != b.get(k):
            out.append((k, a.get(k), b.get(k)))
    if a.get('system_vars') != b.get('system_vars'):
        for i in range(16):
            av, bv = a['system_vars'][i], b['system_vars'][i]
            if av != bv:
                out.append((f'system_vars[{i}]', av, bv))
    for who in ('p1', 'p2'):
        for k in PLAYER_FIELDS:
            av, bv = a[who][k], b[who][k]
            if av != bv:
                out.append((f'{who}.{k}', av, bv))
    return out

def main(a_path, b_path):
    a = load(a_path)
    b = load(b_path)
    print()
    # Align by "first battle-phase snapshot" (match_phase == 3000) on each
    # side, then walk forward index-paired. The `frame` field by itself is
    # FM2K's match-counter and shares 0..N between CSS and battle phases,
    # so blind-merging by frame number aligns CSS-on-A with battle-on-B.
    BATTLE_PHASE = 3000
    def first_battle_idx(snaps):
        for i, s in enumerate(snaps):
            if s['match_phase'] == BATTLE_PHASE:
                return i
        return None
    ai = first_battle_idx(a)
    bi = first_battle_idx(b)
    if ai is None or bi is None:
        print(f'No battle phase found: a_idx={ai} b_idx={bi}')
        return 2
    print(f'A battle starts at index {ai} (frame={a[ai]["frame"]})')
    print(f'B battle starts at index {bi} (frame={b[bi]["frame"]})')
    n = min(len(a) - ai, len(b) - bi)
    print(f'comparing {n} battle-aligned snapshots')
    # Walk index-paired from battle start; report first divergence
    for k in range(n):
        sa, sb = a[ai + k], b[bi + k]
        d = diff_snap(sa, sb)
        if d:
            print(f'\nFIRST DIVERGENCE at battle-aligned k={k} '
                  f'(A.frame={sa["frame"]} B.frame={sb["frame"]})\n')
            for fld, av, bv in d:
                if isinstance(av, int):
                    print(f'  {fld:32s}  A=0x{av & 0xFFFFFFFF:08X}  B=0x{bv & 0xFFFFFFFF:08X}'
                          f'  delta={bv - av}')
                else:
                    print(f'  {fld:32s}  A={av}  B={bv}')
            # Show 1 frame of context BEFORE divergence (last matching)
            if k > 0:
                pa = a[ai + k - 1]
                pb = b[bi + k - 1]
                print(f'\n  Last matching frame: A.frame={pa["frame"]} B.frame={pb["frame"]}')
                print(f'    rng={pa["rng"]:#x} input_p1=0x{pa["input_p1"]:03X} input_p2=0x{pa["input_p2"]:03X}')
                print(f'    p1.pos=({pa["p1"]["pos_x"]},{pa["p1"]["pos_y"]}) '
                      f'p1.vel=({pa["p1"]["vel_x"]},{pa["p1"]["vel_y"]}) p1.script={pa["p1"]["script_idx"]}/{pa["p1"]["item_idx"]}')
                print(f'    p2.pos=({pa["p2"]["pos_x"]},{pa["p2"]["pos_y"]}) '
                      f'p2.vel=({pa["p2"]["vel_x"]},{pa["p2"]["vel_y"]}) p2.script={pa["p2"]["script_idx"]}/{pa["p2"]["item_idx"]}')
            return 0
    print('\nALL ALIGNED FRAMES IDENTICAL — no divergence detected.')
    return 0

if __name__ == '__main__':
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(2)
    sys.exit(main(sys.argv[1], sys.argv[2]))
