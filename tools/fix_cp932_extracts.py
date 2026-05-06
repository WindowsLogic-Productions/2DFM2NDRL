#!/usr/bin/env python3
"""Re-extract zips whose first 7z run produced mojibake filenames.

Some FM2K zips (older Japanese distributions) stored their inner
filenames in Shift_JIS / CP932 without the UTF-8 BOM that newer
zip tools use. When those zips were extracted with `7z x` under our
default WSL locale, the cp932 bytes got replaced with U+FFFD (the
Unicode REPLACEMENT CHARACTER, encoded as `EF BF BD` in UTF-8). The
original Shift_JIS bytes are gone — there's no way to reconstruct
them from the on-disk filenames.

But the source zip is fine. `unzip -O cp932 archive.zip` correctly
interprets the headers and produces proper UTF-8 Japanese
filenames. This pass:

  1. Walks /mnt/d/games/{fm2k,fm95}/ for ANY descendant whose name
     contains U+FFFD — that's the mojibake signal.
  2. For each affected game-dir: locate the original .zip(s), wipe
     everything except metadata.json + the .zip(s), re-extract with
     `unzip -O cp932`.
  3. Caller then runs flatten_nested_dirs.py + apply_clean_engine.py
     to collapse nesting and re-drop the clean engine binary.

Always dry-run by default. Pass --apply to actually delete + re-
extract.

Usage:
    python3 tools/fix_cp932_extracts.py
    python3 tools/fix_cp932_extracts.py --apply
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from pathlib import Path


DEFAULT_ROOTS = [Path("/mnt/d/games/fm2k"),
                 Path("/mnt/d/games/fm95")]
MOJIBAKE_CHAR = "�"  # U+FFFD


def has_mojibake(name: str) -> bool:
    return MOJIBAKE_CHAR in name


def discover_affected(roots: list[Path]) -> list[Path]:
    """For each root, find game-dirs containing any descendant
    whose filename has the U+FFFD char. A game-dir is the dir at
    the level of metadata.json (canonical) — bubble up to the
    nearest metadata.json ancestor."""
    affected_paths: set[Path] = set()
    for root in roots:
        if not root.exists():
            continue
        for p in root.rglob("*"):
            if has_mojibake(p.name):
                # Walk up to nearest metadata.json
                cur = p.parent if p.is_file() else p
                while cur != root and cur != cur.parent:
                    if (cur / "metadata.json").is_file():
                        affected_paths.add(cur)
                        break
                    cur = cur.parent
    return sorted(affected_paths)


def find_zips(game_dir: Path) -> list[Path]:
    out: list[Path] = []
    try:
        for p in game_dir.iterdir():
            if p.is_file() and p.suffix.lower() == ".zip":
                out.append(p)
    except OSError:
        pass
    return out


def wipe_extract(game_dir: Path, *, dry_run: bool) -> int:
    """Remove everything in game_dir EXCEPT .zip files and
    metadata.json. Returns count removed."""
    keep_suffix = (".zip",)
    keep_names = ("metadata.json",)
    n = 0
    try:
        for entry in game_dir.iterdir():
            if entry.name in keep_names:
                continue
            if entry.is_file() and entry.suffix.lower() in keep_suffix:
                continue
            if dry_run:
                print(f"      would rm  {entry.name}")
                n += 1
                continue
            try:
                if entry.is_dir():
                    shutil.rmtree(entry)
                else:
                    entry.unlink()
                n += 1
            except OSError as e:
                print(f"      ERR rm {entry}: {e}", file=sys.stderr)
    except OSError as e:
        print(f"      ERR iter {game_dir}: {e}", file=sys.stderr)
    return n


def reextract_cp932(zip_path: Path, *, dry_run: bool) -> bool:
    """Re-extract zip with cp932 codepage so Shift_JIS names land
    as proper UTF-8. Output dir is sibling stem-named (matches the
    convention migrate_games_to_d.py uses)."""
    out_dir = zip_path.parent / zip_path.stem
    if dry_run:
        print(f"      would unzip -O cp932 {zip_path.name} → "
              f"{out_dir.name}/")
        return True
    out_dir.mkdir(parents=True, exist_ok=True)
    cmd = ["unzip", "-q", "-O", "cp932", "-o", str(zip_path),
           "-d", str(out_dir)]
    print(f"      unzip -O cp932 {zip_path.name} → {out_dir.name}/")
    r = subprocess.run(cmd, stdin=subprocess.DEVNULL,
                       capture_output=True, text=True)
    if r.returncode != 0:
        print(f"      ERR unzip exit {r.returncode}: "
              f"{r.stderr.strip()[:200]}", file=sys.stderr)
        return False
    return True


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", action="append", type=Path,
                    default=None,
                    help=f"root dir to scan (repeat). default: "
                         f"{DEFAULT_ROOTS}")
    ap.add_argument("--apply", action="store_true",
                    help="actually wipe + re-extract; default is "
                         "dry run")
    args = ap.parse_args()

    if shutil.which("unzip") is None:
        print("error: unzip not found in PATH (apt install unzip)",
              file=sys.stderr)
        return 1

    roots = args.root or DEFAULT_ROOTS
    affected = discover_affected(roots)
    print(f"affected game-dirs: {len(affected)}")
    if not affected:
        return 0

    n_ok = n_fail = 0
    for gd in affected:
        zips = find_zips(gd)
        try:
            rel = gd.relative_to(gd.parents[2])
        except ValueError:
            rel = gd
        print(f"  {rel}")
        if not zips:
            print(f"      ! no .zip found — can't re-extract",
                  file=sys.stderr)
            n_fail += 1
            continue
        for z in zips:
            print(f"      zip: {z.name} "
                  f"({z.stat().st_size:,} B)")

        wipe_extract(gd, dry_run=not args.apply)

        all_ok = True
        for z in zips:
            if not reextract_cp932(z, dry_run=not args.apply):
                all_ok = False
        if all_ok:
            n_ok += 1
        else:
            n_fail += 1

    print()
    verb = "fixed" if args.apply else "would fix"
    print(f"{verb}: {n_ok}, failed: {n_fail}")
    if not args.apply:
        print("(dry run — pass --apply to actually wipe + re-extract)")
        print("after --apply, also re-run:")
        print("  python3 tools/flatten_nested_dirs.py /mnt/d/games/fm2k --apply")
        print("  python3 tools/flatten_nested_dirs.py /mnt/d/games/fm95 --apply")
        print("  python3 tools/apply_clean_engine.py /mnt/d/games/fm2k --apply")
    return 0 if n_fail == 0 else 2


if __name__ == "__main__":
    sys.exit(main())
