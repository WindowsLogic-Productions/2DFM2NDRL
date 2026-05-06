#!/usr/bin/env python3
"""Copy /mnt/c/games/2dfm/ → /mnt/d/games/2dfm/ and unpack archives.

C: drive is full (97% used, 34GB free); D: has 160GB free. Mass-
copy the whole 2dfm tree there, then extract every .zip / .7z / .rar
encountered side-by-side so we have one canonical "everything
unpacked" tree to analyze for packed exes / cleanup / banner
captures / etc.

Pipeline:
  1. rsync-style copy with `cp -ru` (resume / skip existing-and-
     newer). Preserves directory layout exactly.
  2. For each archive in the destination, extract to a sibling
     directory named after the archive's stem. Idempotent: if the
     extraction dir already has files, skip unless --refresh-extract.
  3. Optional --remove-archives strips the original .zip / .7z /
     .rar AFTER successful extraction so the disk usage doesn't
     double. Off by default — keep originals until you've confirmed
     the extracted tree works.

Usage:
    python3 tools/migrate_games_to_d.py
    python3 tools/migrate_games_to_d.py --src /mnt/c/games/2dfm \\
                                        --dst /mnt/d/games/2dfm
    python3 tools/migrate_games_to_d.py --refresh-extract
    python3 tools/migrate_games_to_d.py --remove-archives  # destructive

Polite about disk: refuses to start if dst is on the same drive as
src and would balloon usage in place.
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path


SRC_DEFAULT = Path("/mnt/c/games/2dfm")
DST_DEFAULT = Path("/mnt/d/games/2dfm")

ARCHIVE_EXTS = (".zip", ".7z", ".rar")


def has_7z() -> bool:
    return shutil.which("7z") is not None


def have_required_tools() -> bool:
    if not has_7z():
        print("error: 7z not found in PATH. install p7zip-full "
              "(`apt install p7zip-full`).", file=sys.stderr)
        return False
    return True


def run_copy(src: Path, dst: Path) -> bool:
    """Mirror src into dst. Use rsync if available (fastest, robust
    incremental); fall back to `cp -r` skip-newer."""
    dst.mkdir(parents=True, exist_ok=True)
    if shutil.which("rsync"):
        # -a archive (perms, times, recursive); --info=progress2
        # gives a single rolling progress line; --no-i-r so the rate
        # estimate is accurate from the start.
        cmd = ["rsync", "-a", "--info=progress2", "--no-i-r",
               str(src) + "/", str(dst) + "/"]
    else:
        # cp -r --update preserves directory + only overwrites
        # newer files. Slower than rsync but no extra deps.
        cmd = ["cp", "-rv", "--update", str(src) + "/.", str(dst) + "/"]
    print(f"copy: {' '.join(cmd)}")
    result = subprocess.run(cmd)
    return result.returncode == 0


def extract_dir_for(archive: Path) -> Path:
    """Where to extract the archive. Sibling directory named after
    the archive's stem. `Foo.zip` → `Foo/`."""
    return archive.parent / archive.stem


def already_extracted(extract_dir: Path) -> bool:
    """Has this archive's extract dir already been populated?"""
    if not extract_dir.exists():
        return False
    try:
        return any(extract_dir.iterdir())
    except OSError:
        return False


def extract_archive(archive: Path, *, refresh: bool) -> bool:
    """Use 7z to expand archive into its sibling stem directory.
    Handles .zip / .7z / .rar uniformly via `7z x`."""
    out_dir = extract_dir_for(archive)
    if already_extracted(out_dir) and not refresh:
        return True
    out_dir.mkdir(parents=True, exist_ok=True)
    cmd = ["7z", "x", "-y", "-bso0", "-bsp1",
           f"-o{out_dir}", str(archive)]
    print(f"  extract: {archive.name} → {out_dir.relative_to(out_dir.parent.parent)}/")
    r = subprocess.run(cmd)
    return r.returncode == 0


def find_archives(root: Path) -> list[Path]:
    out: list[Path] = []
    for p in root.rglob("*"):
        if p.is_file() and p.suffix.lower() in ARCHIVE_EXTS:
            out.append(p)
    return sorted(out)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", type=Path, default=SRC_DEFAULT)
    ap.add_argument("--dst", type=Path, default=DST_DEFAULT)
    ap.add_argument("--skip-copy",   action="store_true",
                    help="skip the C→D copy step; only extract "
                         "archives that already exist under --dst")
    ap.add_argument("--skip-extract", action="store_true",
                    help="copy only; don't expand any archives")
    ap.add_argument("--refresh-extract", action="store_true",
                    help="re-extract archives even when target dir "
                         "already has files")
    ap.add_argument("--remove-archives", action="store_true",
                    help="DESTRUCTIVE: delete each .zip/.7z/.rar "
                         "after a successful extraction. Off by "
                         "default — verify first, then re-run with "
                         "this flag once you trust the extracted "
                         "tree.")
    args = ap.parse_args()

    if not have_required_tools():
        return 1

    if not args.src.exists():
        print(f"error: src {args.src} does not exist", file=sys.stderr)
        return 1

    # Sanity: refuse to migrate in-place onto the same drive — that
    # would just double C:'s usage instead of moving to D:.
    if args.src.resolve() == args.dst.resolve():
        print(f"error: src and dst are the same path", file=sys.stderr)
        return 1
    if str(args.dst).startswith(str(args.src.parent.resolve())):
        # dst is under src's parent — likely C:. Bail loudly.
        print(f"warn: dst {args.dst} appears to be on the same drive "
              f"as src. Continuing — but the disk-pressure goal "
              f"isn't met.", file=sys.stderr)

    started = time.monotonic()

    if not args.skip_copy:
        ok = run_copy(args.src, args.dst)
        if not ok:
            print("error: copy failed", file=sys.stderr)
            return 1
        copy_elapsed = time.monotonic() - started
        print(f"copy done in {copy_elapsed:.0f}s")
    else:
        print("skip-copy: assuming dst already mirrors src")

    if args.skip_extract:
        print("skip-extract: done")
        return 0

    archives = find_archives(args.dst)
    print(f"found {len(archives)} archive(s) under {args.dst}")
    extracted = 0
    failed: list[Path] = []
    for a in archives:
        if extract_archive(a, refresh=args.refresh_extract):
            extracted += 1
            if args.remove_archives:
                try:
                    a.unlink()
                    print(f"    removed {a.name}")
                except OSError as e:
                    print(f"    warn: couldn't remove {a}: {e}",
                          file=sys.stderr)
        else:
            failed.append(a)

    print()
    print(f"extracted: {extracted} / {len(archives)}")
    if failed:
        print(f"failed:    {len(failed)}")
        for f in failed[:10]:
            print(f"  {f}")
        if len(failed) > 10:
            print(f"  ... and {len(failed)-10} more")
    print(f"total time: {time.monotonic() - started:.0f}s")
    return 0 if not failed else 2


if __name__ == "__main__":
    sys.exit(main())
