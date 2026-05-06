#!/usr/bin/env python3
"""Collapse the redundant extract-stem layer inside each game dir.

The 2dfm tree has a deliberate hierarchy:
  Studio/Game/[Variant/]<extract_stem>/<actual game files>
  + metadata.json at the Game level.

After running migrate_games_to_d.py, each archive expands into a
sibling dir named after the archive's stem (`Henichi_Win_JA`,
`Eight-Marbles_Fix_Win_JA_Cleaned-exe`, ...). That extract-stem
layer is information-free duplication of the archive's filename —
we want to lift its contents up one level into the parent so the
actual game files (`*.exe`, `*.kgt`, `*.player`) sit directly in
the Game-or-Variant dir.

We DO NOT touch:
  - The Studio level (e.g. `Cascom/Henichi/` — Cascom is a real
    studio, even if it only ships one game).
  - The Game level (the dir containing `metadata.json`).
  - Variant containers (e.g. `Eight Marbles/Cleaned exe/`,
    `TIME AND STOPPER/Version 02/`) — those carry meaningful
    "which build of the game" info.
  - Any subtree without a metadata.json sentinel (e.g. fm95/),
    since we don't know its convention.

We DO lift:
  - The single-subdir-and-nothing-else layer just below the Game
    or Variant dir, when its contents look like a real game folder.

Always dry-run by default. Pass --apply to actually move files.

Usage:
    python3 tools/flatten_nested_dirs.py /mnt/d/games/2dfm
    python3 tools/flatten_nested_dirs.py /mnt/d/games/2dfm --apply
"""

from __future__ import annotations

import argparse
import shutil
import sys
from pathlib import Path


GAME_SENTINEL = "metadata.json"

# If a candidate dir's children include any of these, we treat it
# as the actual game files and stop descending. Lifting happens at
# the level ABOVE this. (.exe is intentionally last so games that
# bundle bonus PDFs/manuals at top alongside their .kgt parents
# still trip the FM2K-asset signal first.)
GAME_FILE_EXTS = (".kgt", ".player", ".stage", ".stg", ".csi",
                  ".dat", ".ini", ".exe")


def is_game_dir(d: Path) -> bool:
    """True if `d` looks like the actual extracted game folder.
    Heuristic: any file with an FM2K-shaped extension at top level."""
    try:
        for child in d.iterdir():
            if child.is_file():
                if child.suffix.lower() in GAME_FILE_EXTS:
                    return True
    except OSError:
        return False
    return False


def lift_into_parent(redundant_dir: Path, *, apply: bool
                     ) -> tuple[bool, int]:
    """Move every entry of `redundant_dir` up to `redundant_dir.parent`,
    then rmdir `redundant_dir`. Returns (did_move, count)."""
    parent = redundant_dir.parent
    try:
        kids = list(redundant_dir.iterdir())
    except OSError:
        return False, 0
    if not kids:
        return False, 0
    # Refuse on collisions — we don't know how to merge safely.
    existing = {p.name for p in parent.iterdir() if p != redundant_dir}
    collisions = [k for k in kids if k.name in existing]
    if collisions:
        print(f"  SKIP {redundant_dir}: name collisions in parent "
              f"({', '.join(c.name for c in collisions[:3])})",
              file=sys.stderr)
        return False, 0
    if not apply:
        return True, len(kids)
    for k in kids:
        try:
            shutil.move(str(k), str(parent / k.name))
        except OSError as e:
            print(f"  ERR  moving {k} → {parent / k.name}: {e}",
                  file=sys.stderr)
            return False, 0
    try:
        redundant_dir.rmdir()
    except OSError:
        pass  # leave the empty dir behind; rare
    return True, len(kids)


def process_layer(d: Path, *, apply: bool, label: str
                  ) -> tuple[int, int, bool]:
    """Post-order flatten of the subtree rooted at `d`.

    Returns (changed, unchanged, would_be_game_dir_after_recursion).
    The third item lets the caller decide whether to lift `d` up
    into ITS parent during dry-run, since dry-run mode can't trust
    the live filesystem to reflect not-yet-applied moves.

    Walk depth-first. After recursing into each subdir, re-check
    whether the subdir has now become a flat game dir (either by
    actually being one on disk, or by virtue of an in-recursion
    "would-lift" claim); if so, lift its contents up into `d`.

    Handles arbitrarily deep nesting like
    `.unpacked/foo_jp/foo_jp/<game files>` because each level gets
    a chance to lift after the level below it has been collapsed.

    Sibling stray files (e.g. a `変更点.txt` changelog living next
    to the redundant nested dir) come along for the ride during a
    lift since lift_into_parent moves all of `d`'s entries.
    """
    if is_game_dir(d):
        return 0, 0, True  # already flat

    try:
        entries = [p for p in d.iterdir()
                   if p.name != GAME_SENTINEL]
    except OSError:
        return 0, 0, False

    files = [p for p in entries if p.is_file()]
    subdirs = [p for p in entries if p.is_dir()]

    if len(subdirs) == 1:
        only = subdirs[0]
        # Post-order: collapse anything inside `only` first, then
        # decide whether `only` itself can be lifted.
        c, u, child_is_game_dir = process_layer(only, apply=apply,
                                                label=label)
        # Lift if the subtree (collapsed or not) is or would-be a
        # game dir. dry-run uses `child_is_game_dir`; --apply uses
        # live filesystem (which now does reflect moves).
        if child_is_game_dir:
            moved, n = lift_into_parent(only, apply=apply)
            if moved:
                # In dry-run, a chained lift reports stale counts at
                # the upper levels (we can't see the would-be moved
                # items). Suppress intermediate prints — the deepest
                # lift's print represents the chain. In --apply each
                # step's filesystem state is real, so we always print.
                deeper_chain = (c > 0)
                if apply or not deeper_chain:
                    verb = "lifted" if apply else "would lift"
                    chain_note = f" (×{c + 1} chain)" if c > 0 else ""
                    print(f"  {verb:10s} {n:>3} item(s)  "
                          f"{label} ← {only.name}/{chain_note}")
                return c + 1, u, True
            return c, u + 1, child_is_game_dir
        return c, u, child_is_game_dir

    # Zero or multiple subdirs at this level — recurse into each.
    # (Multiple = variant container like file-1/Cleaned exe/...)
    changed = unchanged = 0
    for sd in subdirs:
        c, u, _ = process_layer(sd, apply=apply,
                                label=f"{label}/{sd.name}")
        changed += c
        unchanged += u
    # A multi-subdir container is never itself a game dir.
    return changed, unchanged, False


def find_game_dirs(root: Path) -> list[Path]:
    out: list[Path] = []
    for sentinel in root.rglob(GAME_SENTINEL):
        if sentinel.is_file():
            out.append(sentinel.parent)
    return sorted(out)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("root", type=Path,
                    help="dir to walk; usually /mnt/d/games/2dfm")
    ap.add_argument("--apply", action="store_true",
                    help="actually move files; default is dry run")
    args = ap.parse_args()

    if not args.root.exists():
        print(f"error: {args.root} not found", file=sys.stderr)
        return 1

    game_dirs = find_game_dirs(args.root)
    print(f"found {len(game_dirs)} game dir(s) "
          f"(via {GAME_SENTINEL} sentinel) under {args.root}")
    print()

    total_changed = total_unchanged = 0
    for gd in game_dirs:
        rel = gd
        try:
            rel = gd.relative_to(args.root)
        except ValueError:
            pass
        c, u, _ = process_layer(gd, apply=args.apply, label=str(rel))
        total_changed += c
        total_unchanged += u

    print()
    verb = "lifted" if args.apply else "would lift"
    print(f"{verb}: {total_changed} extract-stem layer(s); "
          f"unchanged: {total_unchanged}")
    if not args.apply and total_changed:
        print("(dry run — pass --apply to actually move files)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
