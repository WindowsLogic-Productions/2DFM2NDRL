#!/usr/bin/env python3
"""Drop the canonical clean FM2K engine exe into every game folder.

The vanilla FM2K_2nd build outputs a 1,204,224-byte .exe with the
four standard PE sections (.text / .rdata / .data / .rsrc). Most
2dfm-scene games we have on disk ship a packed exe instead — either
URO-wrapped (1.21 MB, +.uro\\x07 section) or heavy-packed with
NUL-named sections (1.86 MB MoleBox?-style). Either form blocks
the launcher's hooks and parity recorder.

This pass replaces the dirty engine with the clean one, renamed to
match the existing .kgt in each folder (FM2K convention is
`<name>.exe` paired with `<name>.kgt`). Game data files (.kgt /
.player / .stage / etc.) are completely engine-version-independent
and run fine against the clean engine — that's why the swap is
safe.

Source baseline (default): /mnt/d/games/2dfm/wanwan/WonderfulWorld_ver_0946.exe
which we've verified at 1,204,224 bytes with sections
['.text', '.rdata', '.data', '.rsrc'] only. Override with --source.

Behavior per .kgt-bearing folder:
  - target name = `<kgt_stem>.exe`
  - if target exists and is identical to source: skip (already clean)
  - if target exists and is different: overwrite (the whole point)
  - if target doesn't exist: create

Skipped automatically:
  - anything under `fm95/` (different engine — 384 KB, not FM2K_2nd)
  - directories under --skip-dirs (default empty)

Always dry-run by default. Pass --apply to actually copy.

Usage:
    python3 tools/apply_clean_engine.py /mnt/d/games/2dfm
    python3 tools/apply_clean_engine.py /mnt/d/games/2dfm --apply
    python3 tools/apply_clean_engine.py /mnt/d/games/2dfm \\
            --source /path/to/your/preferred_clean.exe
"""

from __future__ import annotations

import argparse
import shutil
import struct
import sys
from pathlib import Path


# After the engine-bucket sort (tools/sort_by_engine.py), the clean
# baseline lives under fm2k/_NODEV/wanwan/. Fall back to the old
# pre-sort path so the tool stays runnable on a fresh tree.
_BASELINE_CANDIDATES = [
    Path("/mnt/d/games/fm2k/_NODEV/wanwan/WonderfulWorld_ver_0946.exe"),
    Path("/mnt/d/games/2dfm/wanwan/WonderfulWorld_ver_0946.exe"),
]
DEFAULT_SOURCE = next((p for p in _BASELINE_CANDIDATES if p.exists()),
                     _BASELINE_CANDIDATES[0])
EXPECTED_SIZE = 1_204_224  # 1.148 MiB
EXPECTED_SECTIONS = {".text", ".rdata", ".data", ".rsrc"}


def read_pe_section_names(path: Path) -> list[str]:
    """Parse PE header, return section names (lowercased)."""
    try:
        with path.open("rb") as f:
            head = f.read(0x40)
            if len(head) < 0x40 or head[:2] != b"MZ":
                return []
            (e_lfanew,) = struct.unpack("<I", head[0x3C:0x40])
            f.seek(e_lfanew)
            sig = f.read(4)
            if sig != b"PE\x00\x00":
                return []
            coff = f.read(20)
            num_sections = struct.unpack("<H", coff[2:4])[0]
            optional_size = struct.unpack("<H", coff[16:18])[0]
            f.seek(e_lfanew + 4 + 20 + optional_size)
            names: list[str] = []
            for _ in range(num_sections):
                rec = f.read(40)
                if len(rec) < 40:
                    break
                names.append(rec[:8].split(b"\x00", 1)[0]
                             .decode("ascii", errors="replace").lower())
            return names
    except OSError:
        return []


def validate_source(src: Path) -> tuple[bool, str]:
    """Confirm `src` is the canonical clean FM2K_2nd exe.
    Returns (ok, reason)."""
    if not src.exists():
        return False, f"source not found: {src}"
    size = src.stat().st_size
    if size != EXPECTED_SIZE:
        return False, (f"source size {size} != expected "
                       f"{EXPECTED_SIZE} ({size/1024:.1f} KB vs "
                       f"{EXPECTED_SIZE/1024:.1f} KB). pass a "
                       f"verified-clean exe via --source.")
    sections = read_pe_section_names(src)
    extras = set(s for s in sections if s) - EXPECTED_SECTIONS
    if extras:
        return False, ("source has extra sections "
                       f"{sorted(extras)} — not pristine. pass a "
                       f"verified-clean exe via --source.")
    if not EXPECTED_SECTIONS.issubset(set(sections)):
        return False, ("source is missing one of the standard "
                       "sections — not a typical FM2K_2nd build.")
    return True, "verified clean baseline"


def find_kgt_dirs(root: Path, skip_substrings: list[str]
                  ) -> dict[Path, list[Path]]:
    """Return {dir: [kgt_paths]} for every dir under root that
    contains at least one .kgt (case-insensitive — some 2dfm
    distributions ship `.KGT` uppercase: MK vs SF, Axel City),
    excluding skipped subtrees."""
    out: dict[Path, list[Path]] = {}
    # Case-insensitive glob via per-character class — `[Kk][Gg][Tt]`
    # matches .kgt / .KGT / mixed-case alike. WSL on /mnt/d (NTFS
    # via 9P) is case-sensitive, so plain "*.kgt" misses uppercase.
    for kgt in root.rglob("*.[Kk][Gg][Tt]"):
        if not kgt.is_file():
            continue
        s = str(kgt)
        if any(skip in s for skip in skip_substrings):
            continue
        out.setdefault(kgt.parent, []).append(kgt)
    return dict(sorted(out.items()))


def files_identical(a: Path, b: Path) -> bool:
    try:
        if a.stat().st_size != b.stat().st_size:
            return False
    except OSError:
        return False
    # Same size — cheap content check (one read of each, cmp).
    try:
        with a.open("rb") as fa, b.open("rb") as fb:
            while True:
                ba = fa.read(64 * 1024)
                bb = fb.read(64 * 1024)
                if ba != bb:
                    return False
                if not ba:
                    return True
    except OSError:
        return False


def is_already_clean(target: Path) -> bool:
    """Already-clean = correct size AND only standard sections.
    These are working engine builds (possibly different compile but
    both pristine) — don't touch unless --force."""
    try:
        if target.stat().st_size != EXPECTED_SIZE:
            return False
    except OSError:
        return False
    sections = read_pe_section_names(target)
    if not sections:
        return False
    nonempty = set(s for s in sections if s)
    return (bool(nonempty)
            and nonempty <= EXPECTED_SECTIONS
            and EXPECTED_SECTIONS.issubset(set(sections)))


def is_smaller_than_baseline(target: Path) -> bool:
    """No legitimate FM2K_2nd exe is smaller than the WonderfulWorld
    1,204,224-byte baseline — the engine has a fixed minimum size
    after the standard sections. Anything smaller is a different
    engine entirely (FM95 at 384 KB, urobura's cadenza variant at
    1.12 MB, possibly other older builds we haven't catalogued).
    Replacing those with FM2K_2nd would break the game.

    This subsumes the previous narrow `is_fm95_engine` size band
    and catches every too-small case in one rule."""
    try:
        return target.stat().st_size < EXPECTED_SIZE
    except OSError:
        return False


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("root", type=Path,
                    help="dir to walk; usually /mnt/d/games/2dfm")
    ap.add_argument("--source", type=Path, default=DEFAULT_SOURCE,
                    help="path to the verified-clean FM2K_2nd "
                         f"engine exe. default: {DEFAULT_SOURCE}")
    ap.add_argument("--apply", action="store_true",
                    help="actually copy; default is dry run")
    ap.add_argument("--force", action="store_true",
                    help="also overwrite already-clean engine exes "
                         "(byte-different but same-shape clean "
                         "builds — pkmncc, vanpri, etc.). default "
                         "is to leave those alone, since they're "
                         "working clean engines just from a "
                         "different compile.")
    ap.add_argument("--skip-dirs", default="/fm95/",
                    help="comma-separated path-substring blacklist. "
                         "default: '/fm95/' (the FM95 engine is a "
                         "different binary entirely; FM95 exes "
                         "anywhere in the tree are also auto-detected "
                         "by their ~384 KB size)")
    args = ap.parse_args()

    if not args.root.exists():
        print(f"error: {args.root} not found", file=sys.stderr)
        return 1

    ok, reason = validate_source(args.source)
    if not ok:
        print(f"error: source rejected — {reason}", file=sys.stderr)
        return 1
    print(f"source: {args.source} ({reason})")

    skip = [s.strip() for s in args.skip_dirs.split(",") if s.strip()]
    kgt_map = find_kgt_dirs(args.root, skip)
    n_dirs = len(kgt_map)
    n_kgts = sum(len(v) for v in kgt_map.values())
    print(f"found {n_kgts} .kgt(s) across {n_dirs} dir(s) "
          f"(skipping: {skip})")
    print()

    n_byte_identical = 0
    n_already_clean = 0
    n_fm95_skipped = 0
    n_no_target = 0
    n_replaced = 0
    n_failed = 0

    for d, kgts in kgt_map.items():
        try:
            rel = d.relative_to(args.root)
        except ValueError:
            rel = d
        # One clean exe per .kgt stem.
        for kgt in kgts:
            stem = kgt.stem
            target = d / f"{stem}.exe"
            if not target.exists():
                # Don't fabricate exes from scratch — if there's a
                # .kgt without a matching .exe at the same stem,
                # treat that as a non-canonical layout we shouldn't
                # touch.
                n_no_target += 1
                continue
            if files_identical(target, args.source):
                n_byte_identical += 1
                continue  # already exactly our baseline
            if is_smaller_than_baseline(target):
                n_fm95_skipped += 1
                print(f"  [SKIP small]  {rel}/{target.name}  "
                      f"({target.stat().st_size:,} B — smaller than "
                      f"FM2K_2nd baseline, different engine)")
                continue
            if is_already_clean(target) and not args.force:
                n_already_clean += 1
                print(f"  [keep clean] {rel}/{target.name}  "
                      f"(byte-different but already-clean engine; "
                      f"pass --force to unify)")
                continue
            action = "would replace" if not args.apply else "replaced"
            size = target.stat().st_size
            tag = ("[force]" if args.force and is_already_clean(target)
                   else "[packed]")
            print(f"  [{action}] {tag} {rel}/{target.name}  "
                  f"({size:,} B → {EXPECTED_SIZE:,} B)")
            if args.apply:
                try:
                    shutil.copy2(str(args.source), str(target))
                    n_replaced += 1
                except OSError as e:
                    print(f"     ERR: {e}", file=sys.stderr)
                    n_failed += 1
            else:
                n_replaced += 1

    print()
    print(f"summary:")
    print(f"  byte-identical (no-op):       {n_byte_identical}")
    print(f"  already-clean (kept):         {n_already_clean}")
    print(f"  smaller engine (kept):        {n_fm95_skipped}")
    print(f"  no matching exe (skipped):    {n_no_target}")
    print(f"  replaced packed exe:          {n_replaced}")
    if n_failed:
        print(f"  failed:                   {n_failed}")
    if not args.apply:
        print()
        print("(dry run — pass --apply to actually copy)")
    return 0 if not n_failed else 2


if __name__ == "__main__":
    sys.exit(main())
