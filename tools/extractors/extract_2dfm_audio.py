#!/usr/bin/env python3
"""
Extract embedded WAV/MIDI sound entries from FM2K .demo / .stage / .player files.

File layout (from KGT2nd_EDITOR.exe `ReadCommonResourcePart@0x428BE0` +
`LoadStageFile@0x4257A0` + `LoadDemoFile`):

  16     file signature ("2DKGT2K" or "2DKGT2G" + 9 reserved bytes)
  256    KgtNameInfo header.name
  4      script_count        (<= 0x400)
  39*N   scripts
  4      item_count          (<= 0x10000)
  16*N   script items
  4      pic_count           (<= 0x2000)
  N x:
    20   KgtPictureSlotEntry: data_ptr/_, width, height, hasPrivatePalette, sizeOverride
    var  pixel data: sizeOverride if non-zero, else width*height (+1024 if private palette)
  0x2100 sharedPalettes
  4      sound_count         (<= 0x100)
  N x:
    42   KgtSoundSlotEntry: data_ptr/_(4) + name[32] + size(4) + soundType(1) + track(1)
    var  raw WAV / MIDI bytes (size bytes)
  4      trailer
  0x409  stage/demo extension (only present on .stage/.demo)

soundType low nibble: 0=none, 1=WAV, 2=MIDI, 3=CDDA-track. Bit 4 = loop flag.
WAV blobs are full RIFF/WAVE files; MIDI blobs are SMF (start with "MThd").
CDDA entries have size==0, only `track`.
"""

import argparse
import os
import re
import struct
import sys


SIG_PLAIN = b'2DKGT2K'
SIG_FLAGGED = b'2DKGT2G'  # editor accepts both; payload is not actually encrypted on disk


def safe_name(raw: bytes) -> str:
    s = raw.split(b'\0', 1)[0]
    try:
        text = s.decode('cp932')
    except UnicodeDecodeError:
        text = s.decode('latin-1', errors='replace')
    text = re.sub(r'[<>:"/\\|?*\x00-\x1f]', '_', text).strip()
    return text or 'unnamed'


def parse(data: bytes, label: str, out_dir: str, *, is_player=False) -> int:
    if data[:7] not in (SIG_PLAIN, SIG_FLAGGED):
        print(f"[skip] {label}: bad magic {data[:7]!r}")
        return 0

    p = 16
    if p + 256 > len(data):
        return 0
    p += 256  # KgtNameInfo header.name

    def u32(off):
        return struct.unpack_from('<I', data, off)[0]

    # Scripts
    script_count = u32(p); p += 4
    if script_count > 0x400:
        print(f"[skip] {label}: script_count={script_count} too large")
        return 0
    p += 39 * script_count

    # Script items
    item_count = u32(p); p += 4
    if item_count > 0x10000:
        print(f"[skip] {label}: item_count={item_count} too large")
        return 0
    p += 16 * item_count

    # Pictures
    pic_count = u32(p); p += 4
    if pic_count > 0x2000:
        print(f"[skip] {label}: pic_count={pic_count} too large")
        return 0
    for _ in range(pic_count):
        if p + 20 > len(data):
            print(f"[trunc] {label}: in picture headers")
            return 0
        _, width, height, flags, size_override = struct.unpack_from('<iiiii', data, p)
        p += 20
        if size_override:
            blob_size = size_override
        else:
            blob_size = width * height
            if flags & 1:
                blob_size += 1024
        if blob_size < 0 or p + blob_size > len(data):
            print(f"[trunc] {label}: picture blob size={blob_size} would exceed file")
            return 0
        p += blob_size

    # Shared palettes
    p += 0x2100
    if p > len(data):
        print(f"[trunc] {label}: shared palettes")
        return 0

    # Sounds
    sound_count = u32(p); p += 4
    if sound_count > 0x100:
        print(f"[skip] {label}: sound_count={sound_count} too large")
        return 0

    n_written = 0
    for i in range(sound_count):
        if p + 42 > len(data):
            print(f"[trunc] {label}: sound header {i}")
            break
        _, sound_name_raw, size, sound_type, track = struct.unpack_from('<I32sIBB', data, p)
        p += 42
        sname = safe_name(sound_name_raw)
        kind = sound_type & 0xF
        loop = bool(sound_type & 0x10)
        if size == 0:
            if kind == 3:
                print(f"  [{i:03d}] CDDA track={track} name={sname!r} (no embedded data)")
            elif kind == 0:
                pass  # empty slot
            else:
                print(f"  [{i:03d}] type={kind} size=0 name={sname!r}")
            continue

        if p + size > len(data):
            print(f"[trunc] {label}: sound blob {i} ({size} bytes) would exceed file")
            break
        blob = data[p:p + size]
        p += size

        ext_map = {1: 'wav', 2: 'mid'}
        ext = ext_map.get(kind, 'bin')
        # Quick sanity check on first few bytes for expected magic
        if kind == 1 and not blob.startswith(b'RIFF'):
            note = ' [no RIFF magic]'
        elif kind == 2 and not blob.startswith(b'MThd'):
            note = ' [no MThd magic]'
        else:
            note = ''

        out_name = f"{label}__{i:03d}__{sname}.{ext}"
        out_name = re.sub(r'[<>:"/\\|?*]', '_', out_name)
        out_path = os.path.join(out_dir, out_name)
        with open(out_path, 'wb') as fout:
            fout.write(blob)
        loop_tag = ' loop' if loop else ''
        print(f"  [{i:03d}] {ext.upper()} {size:>8d}B{loop_tag}  {sname!r} -> {out_name}{note}")
        n_written += 1

    return n_written


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('source_dir')
    ap.add_argument('out_dir')
    ap.add_argument('--players', action='store_true',
                    help='also scan .player files (character voice/SFX)')
    args = ap.parse_args()

    os.makedirs(args.out_dir, exist_ok=True)

    exts = {'.demo', '.stage', '.kgt'}
    if args.players:
        exts.add('.player')

    total_files = 0
    total_blobs = 0
    for entry in sorted(os.listdir(args.source_dir)):
        ext = os.path.splitext(entry)[1].lower()
        if ext not in exts:
            continue
        full = os.path.join(args.source_dir, entry)
        if not os.path.isfile(full):
            continue
        with open(full, 'rb') as f:
            data = f.read()
        label = os.path.splitext(entry)[0]
        print(f"\n=== {entry} ({len(data):,} bytes) ===")
        n = parse(data, label, args.out_dir, is_player=(ext == '.player'))
        total_files += 1
        total_blobs += n

    print(f"\n--- done: scanned {total_files} files, wrote {total_blobs} sound blobs to {args.out_dir} ---")


if __name__ == '__main__':
    sys.exit(main())
