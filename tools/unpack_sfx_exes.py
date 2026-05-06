#!/usr/bin/env python3
"""Detect + unpack self-extracting-archive .exe files in place.

A lot of FM2K distributions ship the game as a single .exe that's
actually a 7z / Zip SFX wrapper bundling the real game folder
(executable + .kgt + .player files + assets). They look bigger than
a normal FM2K build (25-700 MB instead of ~1.1 MB) and `7z l` on
them lists actual game files instead of just PE resources.

This pass walks a tree, tries `7z l` on every .exe, and for those
that turn out to be archives extracts them via `7z x` into a sibling
`<exe-stem>.unpacked/` directory. Idempotent — already-extracted
sfxes skip on re-run.

Usage:
    python3 tools/unpack_sfx_exes.py /mnt/d/games/2dfm
    python3 tools/unpack_sfx_exes.py /mnt/d/games/2dfm --dry-run
    python3 tools/unpack_sfx_exes.py /mnt/d/games/2dfm --refresh

Heuristic for "this is an SFX, not a normal exe":
  - `7z l` exits zero AND
  - the listing contains at least one file with a game-shaped
    extension (.kgt / .player / .stage / .exe inside) OR contains
    5+ files of any kind
A regular compiled exe gets refused by 7z (non-zero exit + "Cannot
open the file as archive") and is skipped.
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path


ARCHIVE_GAME_EXTS = (".kgt", ".player", ".stage", ".stg", ".csi")

# Lots of FM2K games include unrelated bundled tools that we DON'T
# want to try-extract. They're real exes, not archives.
SKIP_NAME_RE = re.compile(
    r"(?:^|[/\\])(antimicro|antimicrox|lilithport|unins\d*|setup-?\d*|"
    r"buttonbinds|cnc-?ddraw[^/\\]*config|fm2k_rollback|update|kgt_inject"
    r")",
    re.IGNORECASE,
)


def have_7z() -> bool:
    return shutil.which("7z") is not None


def list_archive_contents(exe: Path,
                          timeout_s: float = 15.0
                          ) -> list[str] | None:
    """Run `7z l -ba` on the exe. Returns parsed file paths or None
    if 7z refused (= it's a normal exe).

    `-ba` strips the table header so we get one row per file. The
    last column is the path. Some packers / mode strings push it
    around; we take everything after the last whitespace gap, which
    matches every 7z output template seen in the wild."""
    cmd = ["7z", "l", "-ba", "-slt", str(exe)]
    try:
        r = subprocess.run(cmd, capture_output=True, text=True,
                           timeout=timeout_s, errors="replace")
    except subprocess.TimeoutExpired:
        return None
    if r.returncode != 0:
        return None
    # -slt format: blocks separated by blank lines, each block has
    # "Path = <name>" lines. Pick those out.
    out: list[str] = []
    for line in r.stdout.splitlines():
        if line.startswith("Path = "):
            out.append(line[7:].strip())
    return out


def looks_like_game_sfx(files: list[str]) -> bool:
    """Strict: only treat the exe as an SFX if it lists at least one
    real FM2K game asset (.kgt / .player / .stage / .stg / .csi).

    `7z l -slt` returns PE resources (`.text`, `.rdata`, `.rsrc/...
    /BITMAP/...`) for every Windows exe — even normal compiled
    binaries — so a generic file-count threshold trips on every PE
    file. Only the FM2K extensions are unique enough to be a
    bullet-proof signal."""
    if not files:
        return False
    lower = [f.lower() for f in files]
    return any(f.endswith(ARCHIVE_GAME_EXTS) for f in lower)


def already_unpacked(extract_dir: Path) -> bool:
    if not extract_dir.exists():
        return False
    try:
        return any(extract_dir.iterdir())
    except OSError:
        return False


def unpack_sfx(exe: Path, *, refresh: bool, dry_run: bool) -> bool:
    out_dir = exe.parent / f"{exe.stem}.unpacked"
    if already_unpacked(out_dir) and not refresh:
        print(f"  ⏭  {exe.name} → {out_dir.name}/  (already unpacked)")
        return True
    if dry_run:
        print(f"  ▶  {exe.name} → {out_dir.name}/  (dry run)")
        return True
    out_dir.mkdir(parents=True, exist_ok=True)
    cmd = ["7z", "x", "-y", "-bso0", "-bsp1",
           f"-o{out_dir}", str(exe)]
    print(f"  ▶  {exe.name} → {out_dir.name}/")
    r = subprocess.run(cmd)
    if r.returncode != 0:
        print(f"     7z x failed (exit {r.returncode})", file=sys.stderr)
        # Tear down the empty output dir so a retry can write to a
        # clean state.
        try:
            shutil.rmtree(out_dir, ignore_errors=True)
        except OSError:
            pass
        return False
    return True


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("root", type=Path,
                    help="dir to walk; usually /mnt/d/games/2dfm")
    ap.add_argument("--refresh", action="store_true",
                    help="re-extract even when target dir is "
                         "already populated")
    ap.add_argument("--dry-run", action="store_true",
                    help="report which exes would be extracted; "
                         "no writes / no actual unpacking")
    ap.add_argument("--include-launchers", action="store_true",
                    help="also probe exes that look like installers /"
                         "tools (antimicrox, lilithport, etc.). "
                         "Default skips them.")
    args = ap.parse_args()

    if not have_7z():
        print("error: 7z not in PATH (apt install p7zip-full)",
              file=sys.stderr)
        return 1
    if not args.root.exists():
        print(f"error: {args.root} not found", file=sys.stderr)
        return 1

    started = time.monotonic()
    candidates: list[Path] = []
    for p in args.root.rglob("*"):
        if not p.is_file():
            continue
        if p.suffix.lower() != ".exe":
            continue
        if (not args.include_launchers and
                SKIP_NAME_RE.search(str(p))):
            continue
        candidates.append(p)
    print(f"scanning {len(candidates)} exe(s) under {args.root}")

    n_sfx = 0
    n_unpacked = 0
    n_failed = 0
    n_normal = 0

    for exe in candidates:
        files = list_archive_contents(exe)
        if files is None or not looks_like_game_sfx(files):
            n_normal += 1
            continue
        n_sfx += 1
        ok = unpack_sfx(exe, refresh=args.refresh, dry_run=args.dry_run)
        if ok:
            n_unpacked += 1
        else:
            n_failed += 1

    elapsed = time.monotonic() - started
    print()
    print(f"results in {elapsed:.0f}s:")
    print(f"  exes scanned:               {len(candidates)}")
    print(f"  not an archive (skipped):   {n_normal}")
    print(f"  detected as SFX:            {n_sfx}")
    print(f"    successfully unpacked:    {n_unpacked}")
    print(f"    extraction failed:        {n_failed}")
    if args.dry_run:
        print()
        print("(dry run — no files written)")
    return 0 if n_failed == 0 else 2


if __name__ == "__main__":
    sys.exit(main())
