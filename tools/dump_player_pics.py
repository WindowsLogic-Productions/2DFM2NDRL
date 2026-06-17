#!/usr/bin/env python3
# Dump all sprite pictures out of a 2DFM .player / .stage / .demo / .kgt file
# as 8bpp indexed BMPs. Mirrors the parse logic in 2dfm/2dfmFileReader.cpp +
# 2dfm/2dfmCommon.cpp::decompress.
#
# Pictures with a private palette use that palette. Pictures without one use
# one of the 8 shared palettes — by default we pick palette 0, but you can
# override per-picture by passing --shared N (or dump every shared variant
# with --all-shared).

import argparse
import glob as globmod
import struct
import sys
from pathlib import Path

KGT_HEADER_SIZE = 16 + 256
SCRIPT_SIZE = 39
SCRIPT_ITEM_SIZE = 16
PICTURE_HEADER_SIZE = 20
PALETTE_SIZE = 1024              # 256 BGRA colors
SHARED_PALETTE_TRAILER = 8 * 4   # 8 ints per palette block
SOUND_HEADER_SIZE = 42

# Script.flags value used for "basic background" scripts — the standing,
# walking, ..., portrait, stage-avatar, shadow scripts that the player
# system locates by counting BACKGROUND-flagged scripts in order. From
# 2dfm/CommonResource.hpp:16.
SCRIPT_FLAG_BACKGROUND = 1

# Position of the portrait script in the BACKGROUND-flagged script
# sequence. Mirrors KgtPlayer::initBasicScriptInfos (2dfm/KgtPlayer.cpp):
# stand, forward, backward, jumpUp, jumpFwd, jumpBack, falling, crouch,
# crouching, standUp, crouchMoveFwd, crouchMoveBack, turnAround,
# crouchTurnAround, btnBlock, btnCrouchBlock, btnAirBlock, startAction,
# victory, fail, drawGame, PORTRAIT (22), stageAvatar, shadow.
PORTRAIT_BG_INDEX = 22

# ShowPic script item — type=12. Layout from
# 2dfm/2dfmScriptItem.hpp::ShowPic. picIdx = idxAndFlip & 0x3FFF.
SCRIPT_ITEM_TYPE_PIC = 12


def decompress(buf: bytes, dest_size: int) -> bytes:
    # Direct port of _2dfm::decompress (2dfm/2dfmCommon.cpp:17). LZ-ish RLE
    # with 4 opcodes encoded in the top 2 bits of the first byte.
    out = bytearray(dest_size)
    rp = 0
    ip = 0
    n = len(buf)
    while ip < n:
        cur = buf[ip]
        op = cur >> 6
        cur &= 0x3F
        if cur == 0:
            ip += 1
            cur = buf[ip]
            if cur == 0:
                ip += 1
                cur = buf[ip] | (buf[ip + 1] << 8)
                ip += 2
                cur = cur + (buf[ip] << 16) + 0x013F
            else:
                cur += 0x3F
        if op == 0:
            rp += cur
        elif op == 1:
            if cur > 0:
                out[rp:rp + cur] = buf[ip + 1:ip + 1 + cur]
                rp += cur
                ip += cur
        elif op == 2:
            ip += 1
            b = buf[ip]
            for i in range(cur):
                out[rp + i] = b
            rp += cur
        else:  # op == 3
            ip += 1
            back = buf[ip]
            if back == 0:
                ip += 1
                back = (buf[ip] + 1) << 8
            start = rp - back
            for i in range(cur):
                out[rp + i] = out[start + i]
            rp += cur
        ip += 1
    return bytes(out)


class Picture:
    __slots__ = ("idx", "width", "height", "has_private_palette",
                 "compressed", "private_palette", "raw")

    def __init__(self, idx, header, payload):
        # Sprite header order is HEIGHT then WIDTH (+4/+8), per closercombat's
        # kgtImageHeader (iHeight, iWidth). Previously read as width,height --
        # swapped -- which transposed the row stride for non-square pics.
        _, h, w, has_priv, size = header
        self.idx = idx
        self.width = w
        self.height = h
        self.has_private_palette = bool(has_priv)
        self.compressed = size != 0
        real_size = w * h + (1024 if has_priv else 0)
        if self.compressed:
            data = decompress(payload, real_size)
        else:
            data = payload
        if self.has_private_palette:
            self.private_palette = data[:1024]
            self.raw = data[1024:1024 + w * h]
        else:
            self.private_palette = None
            self.raw = data[:w * h]


def parse_player_or_kgt(path: Path):
    blob = path.read_bytes()
    pos = KGT_HEADER_SIZE
    sig = blob[:7].decode("latin-1", errors="replace")
    name = blob[16:16 + 256].split(b"\x00", 1)[0].decode("cp932", errors="replace")

    def take(n):
        nonlocal pos
        chunk = blob[pos:pos + n]
        pos += n
        return chunk

    def take_u32():
        return struct.unpack_from("<i", take(4))[0]

    # Common-resource: scripts → script items → pictures → 8 shared palettes
    # → sounds. Same walk as readCommonResourcePart (2dfmFileReader.cpp:295).
    script_count = take_u32()
    # Script struct: char[32] name, uint16 startIdx, byte gap, int32 flags
    # (39 bytes). We capture (start_idx, flags) per script — that's all we
    # need to slice scriptItems and to count BACKGROUND-flagged scripts.
    scripts = []
    for i in range(script_count):
        sname = blob[pos:pos + 32].split(b"\x00", 1)[0]
        start_idx, _gap, flags = struct.unpack_from("<HBi", blob, pos + 32)
        scripts.append((sname.decode("cp932", errors="replace"),
                        start_idx, flags))
        pos += SCRIPT_SIZE

    item_count = take_u32()
    script_items_blob = blob[pos:pos + item_count * SCRIPT_ITEM_SIZE]
    pos += item_count * SCRIPT_ITEM_SIZE

    pic_count = take_u32()
    pictures = []
    for i in range(pic_count):
        hdr = struct.unpack_from("<iiiii", blob, pos)
        pos += PICTURE_HEADER_SIZE
        _, w, h, has_priv, size = hdr
        payload_len = size if size != 0 else (w * h + (1024 if has_priv else 0))
        payload = blob[pos:pos + payload_len]
        pos += payload_len
        pictures.append(Picture(i, hdr, payload))

    shared_palettes = []
    for _ in range(8):
        shared_palettes.append(blob[pos:pos + PALETTE_SIZE])
        pos += PALETTE_SIZE + SHARED_PALETTE_TRAILER

    return {
        "sig": sig,
        "name": name,
        "scripts": scripts,
        "script_item_count": item_count,
        "script_items": script_items_blob,
        "pictures": pictures,
        "shared_palettes": shared_palettes,
    }


def find_bg_script(parsed, bg_slot: int):
    """Return (script_index, script_name) for the Nth BACKGROUND-flagged
    script in the player file (1-indexed), or (None, None) if not found.
    Mirrors KgtPlayer::initBasicScriptInfos: skips scripts[0] and counts
    BACKGROUND scripts in encounter order."""
    scripts = parsed["scripts"]
    bg_count = 0
    for i in range(1, len(scripts)):
        if scripts[i][2] == SCRIPT_FLAG_BACKGROUND:
            bg_count += 1
            if bg_count == bg_slot:
                return i, scripts[i][0]
    return None, None


def find_largest_pic_in_script(parsed, script_index: int):
    """Walk the script's script_items, collect every PIC reference, and
    return the (picIdx, w*h) for the visually largest one (largest pixel
    area in the picture table). Returns (None, 0) if no PICs found.

    The "first PIC" heuristic picks an arbitrary frame which is often a
    flipped/back-facing variant; size-ranking gives the canonical portrait
    art reliably across players."""
    scripts = parsed["scripts"]
    pictures = parsed["pictures"]
    items_blob = parsed["script_items"]
    start = scripts[script_index][1]
    if script_index + 1 < len(scripts):
        end = scripts[script_index + 1][1]
    else:
        end = parsed["script_item_count"]
    best = (None, 0)
    for j in range(start, end):
        off = j * SCRIPT_ITEM_SIZE
        if items_blob[off] != SCRIPT_ITEM_TYPE_PIC:
            continue
        idx_and_flip = struct.unpack_from("<H", items_blob, off + 3)[0]
        idx = idx_and_flip & 0x3FFF
        if idx >= len(pictures):
            continue
        p = pictures[idx]
        area = p.width * p.height
        if area > best[1]:
            best = (idx, area)
    return best


# Canonical KgtPlayer BG slots that may carry a CSS portrait. Slot 22 is
# the formal portraitScriptId per limen's parser, but pkmncc-style games
# put the big "selected character" art at slot 24 ("shadow" canonically,
# repurposed/relabeled by the modder). Dumping all three gives the user
# every candidate without guessing per-game.
PORTRAIT_BG_SLOTS = [22, 23, 24]
PORTRAIT_SLOT_LABELS = {22: "portrait", 23: "stageAvatar", 24: "shadow"}


def write_indexed_bmp(out_path: Path, w: int, h: int,
                      pixels: bytes, palette_bgra: bytes) -> None:
    # 8bpp indexed BMP. Stride padded to 4 bytes per scanline. BMP scanlines
    # are bottom-up so we emit rows in reverse order.
    stride = (w + 3) & ~3
    pad = stride - w
    image_size = stride * h
    palette_bytes = bytearray(1024)
    # BMP palette entries are BGRA (Windows quad). Force alpha=0 — many
    # viewers honor it and we don't want the funky alpha=1 sentinel from
    # 2DFM leaking through.
    for i in range(256):
        b = palette_bgra[i * 4 + 0]
        g = palette_bgra[i * 4 + 1]
        r = palette_bgra[i * 4 + 2]
        palette_bytes[i * 4 + 0] = b
        palette_bytes[i * 4 + 1] = g
        palette_bytes[i * 4 + 2] = r
        palette_bytes[i * 4 + 3] = 0

    file_header_size = 14
    info_header_size = 40
    pixel_offset = file_header_size + info_header_size + 1024
    file_size = pixel_offset + image_size

    bf = struct.pack("<2sIHHI", b"BM", file_size, 0, 0, pixel_offset)
    bi = struct.pack("<IiiHHIIiiII",
                     info_header_size, w, h, 1, 8, 0,
                     image_size, 2835, 2835, 256, 0)

    rows = bytearray()
    pad_bytes = b"\x00" * pad
    # bottom-up
    for y in range(h - 1, -1, -1):
        rows += pixels[y * w:(y + 1) * w]
        if pad:
            rows += pad_bytes

    out_path.write_bytes(bf + bi + bytes(palette_bytes) + bytes(rows))


def write_picture(p: Picture, out_path: Path, shared_palettes, shared_idx: int):
    pal = p.private_palette if p.has_private_palette else shared_palettes[shared_idx]
    write_indexed_bmp(out_path, p.width, p.height, p.raw, pal)


def process_one(src: Path, args, shared_idx_default: int = 0) -> tuple[int, int]:
    parsed = parse_player_or_kgt(src)
    pictures = parsed["pictures"]
    shared = parsed["shared_palettes"]
    print(f"file: {src.name}  sig: {parsed['sig']!r}  name: {parsed['name']!r}  "
          f"pictures: {len(pictures)}  scripts: {len(parsed['scripts'])}")

    out_dir = Path(args.out) if args.out else src.with_name(src.stem + "_pics")

    if args.portrait:
        # Smart path — walk BACKGROUND scripts and grab the largest PIC
        # referenced by each candidate slot. Different games put the big
        # CSS art in different slots; dumping all three covers it.
        out_dir.mkdir(parents=True, exist_ok=True)
        written = 0
        for slot in PORTRAIT_BG_SLOTS:
            si, sname = find_bg_script(parsed, slot)
            if si is None:
                continue
            idx, area = find_largest_pic_in_script(parsed, si)
            if idx is None:
                print(f"  bg#{slot} ({PORTRAIT_SLOT_LABELS[slot]}) "
                      f"name={sname!r}: no PICs")
                continue
            p = pictures[idx]
            if p.width == 0 or p.height == 0:
                continue
            tag = "_priv" if p.has_private_palette else ""
            label = PORTRAIT_SLOT_LABELS[slot]
            out = out_dir / (f"{src.stem}_bg{slot:02d}_{label}_"
                             f"{idx:04d}_{p.width}x{p.height}{tag}.bmp")
            write_picture(p, out, shared, shared_idx_default)
            print(f"  bg#{slot} ({label}) name={sname!r} -> "
                  f"pic #{idx} {p.width}x{p.height} → {out.name}")
            written += 1
        return (written, 0)

    if args.list or args.min_pixels > 0:
        ranked = sorted(pictures, key=lambda p: p.width * p.height, reverse=True)
        for p in ranked[:50]:
            tag = "PRIV" if p.has_private_palette else "shared"
            print(f"  #{p.idx:4d}  {p.width:4d} x {p.height:4d}  "
                  f"({p.width * p.height:>7d} px)  {tag}")
        if args.list:
            return (0, 0)

    out_dir.mkdir(parents=True, exist_ok=True)
    written = 0
    skipped = 0
    for p in pictures:
        if p.width == 0 or p.height == 0:
            skipped += 1
            continue
        if p.width * p.height < args.min_pixels:
            skipped += 1
            continue
        if p.has_private_palette:
            out = out_dir / f"pic_{p.idx:04d}_{p.width}x{p.height}_priv.bmp"
            write_indexed_bmp(out, p.width, p.height, p.raw, p.private_palette)
            written += 1
        else:
            variants = range(8) if args.all_shared else [shared_idx_default]
            for sh in variants:
                pal = shared[sh]
                tag = f"_pal{sh}" if args.all_shared else ""
                out = out_dir / f"pic_{p.idx:04d}_{p.width}x{p.height}{tag}.bmp"
                write_indexed_bmp(out, p.width, p.height, p.raw, pal)
                written += 1

    print(f"  wrote {written} BMPs to {out_dir}  ({skipped} skipped)")
    return (written, skipped)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("file", nargs="+",
                    help=".player / .stage / .demo / .kgt path(s) or globs")
    ap.add_argument("--out", default=None,
                    help="output dir (default: <file>_pics, or current dir if --portrait)")
    ap.add_argument("--shared", type=int, default=0,
                    help="shared palette index (0-7) for non-private pics")
    ap.add_argument("--all-shared", action="store_true",
                    help="emit every shared palette variant for non-private pics")
    ap.add_argument("--min-pixels", type=int, default=0,
                    help="skip pictures with width*height below this")
    ap.add_argument("--list", action="store_true",
                    help="just list pictures + sizes; no BMPs written")
    ap.add_argument("--portrait", action="store_true",
                    help="dump only the CSS portrait (via portraitScriptId lookup)")
    args = ap.parse_args()

    files = []
    for pat in args.file:
        if any(c in pat for c in "*?["):
            files.extend(globmod.glob(pat))
        else:
            files.append(pat)
    if not files:
        sys.exit("no files matched")

    total_written = 0
    total_skipped = 0
    for f in files:
        src = Path(f)
        if not src.exists():
            print(f"not found: {src}")
            continue
        try:
            w, s = process_one(src, args, shared_idx_default=args.shared)
            total_written += w
            total_skipped += s
        except Exception as e:
            print(f"  ERROR on {src.name}: {e}")
    print(f"== total: wrote {total_written}, skipped {total_skipped} across {len(files)} files ==")


if __name__ == "__main__":
    main()
