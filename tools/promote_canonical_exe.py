#!/usr/bin/env python3
"""Promote `file-1/` to game level + apply `Cleaned exe/` over base.

Maiga858 distros (Eight Marbles family) use a particular layout:

    Eight Marbles/
      ├── file-1/                ← full game install (heavy-packed)
      ├── Cleaned exe/           ← single .exe, lighter wrapper
      ├── Full Japanese Version/ ← full Japanese install
      ├── Updated Version/       ← rare, only on EM2X
      └── metadata.json

For our launcher we want:
  - The actual game files at the game-dir level (no `file-1/`
    wrapper — the name is just an artifact of the upstream
    download flow).
  - The `Cleaned exe/<exe>` to overwrite the base game's .exe so
    the game we boot is the lighter wrapper (URO instead of
    MoleBox-style heavy null-named-section pack).
  - The `Cleaned exe/` directory removed once consumed (it's
    just a tiny .exe staging area, no other content).
  - Other variant dirs (Full Japanese Version / Updated Version /
    Version 02 / Version 05) preserved — those carry meaningful
    distinct content, not just a single replacement exe.

This tool does both passes. Pass 1: lift `file-1/` contents up
into the game dir. Pass 2: for each remaining `Cleaned exe/`
subdir, copy each .exe inside over a same-named base .exe at the
game-dir level, then rmdir the `Cleaned exe/`.

Refuses on filename collisions during pass 1 — if the game dir
already has a file that file-1 would overwrite, we bail with an
error so the operator can investigate. Pass 2 INTENTIONALLY
overwrites (that's the whole point — applying the cleaned exe).

Always dry-run by default. Pass --apply to actually move files.

Usage:
    python3 tools/promote_canonical_exe.py /mnt/d/games/2dfm
    python3 tools/promote_canonical_exe.py /mnt/d/games/2dfm --apply
"""

from __future__ import annotations

import argparse
import shutil
import sys
from pathlib import Path


GAME_SENTINEL = "metadata.json"
FILE1_DIR_NAME = "file-1"
CLEAN_DIR_NAMES = ("Cleaned exe", "Cleaned-exe", "cleaned exe")


def find_game_dirs(root: Path) -> list[Path]:
    out: list[Path] = []
    for sentinel in root.rglob(GAME_SENTINEL):
        if sentinel.is_file():
            out.append(sentinel.parent)
    return sorted(out)


def lift_file1(game_dir: Path, *, apply: bool) -> tuple[bool, str]:
    """Pass 1: lift the contents of `<game_dir>/file-1/` up to
    `game_dir`. Returns (changed, reason)."""
    f1 = game_dir / FILE1_DIR_NAME
    if not f1.is_dir():
        return False, "no file-1/"
    try:
        kids = list(f1.iterdir())
    except OSError as e:
        return False, f"read error: {e}"
    if not kids:
        return False, "file-1/ is empty"

    # Refuse on collisions — that's a sign the game dir already had
    # files at game level, and we don't want to overwrite them with
    # file-1's content silently.
    existing = {p.name for p in game_dir.iterdir()
                if p != f1 and p.name != GAME_SENTINEL}
    collisions = [k for k in kids if k.name in existing]
    if collisions:
        return False, ("collision with existing entries: "
                       + ", ".join(c.name for c in collisions[:3]))

    if not apply:
        return True, f"would lift {len(kids)} item(s) from file-1/"
    for k in kids:
        try:
            shutil.move(str(k), str(game_dir / k.name))
        except OSError as e:
            return False, f"move failed: {e}"
    try:
        f1.rmdir()
    except OSError:
        pass  # leftover empty dir is fine
    return True, f"lifted {len(kids)} item(s) from file-1/"


def find_clean_dir(game_dir: Path) -> Path | None:
    for name in CLEAN_DIR_NAMES:
        c = game_dir / name
        if c.is_dir():
            return c
    return None


def apply_cleaned(game_dir: Path, *, apply: bool
                  ) -> tuple[bool, str]:
    """Pass 2: for each .exe inside `<game_dir>/Cleaned exe/`, copy
    it over the same-named .exe at game_dir level (intentionally
    overwriting). Then rm the `Cleaned exe/` dir entirely."""
    cd = find_clean_dir(game_dir)
    if cd is None:
        return False, "no Cleaned exe/"

    try:
        clean_exes = sorted(p for p in cd.iterdir()
                            if p.is_file() and p.suffix.lower() == ".exe")
    except OSError as e:
        return False, f"read error: {e}"
    if not clean_exes:
        return False, "Cleaned exe/ has no .exe inside"

    actions: list[str] = []
    for ce in clean_exes:
        target = game_dir / ce.name
        if target.exists():
            actions.append(
                f"would overwrite {ce.name} ({ce.stat().st_size} B) "
                f"← Cleaned exe/{ce.name}"
                if not apply else
                f"overwrote {ce.name} ← Cleaned exe/{ce.name}")
        else:
            actions.append(
                f"would copy {ce.name} (no base to overwrite)"
                if not apply else
                f"copied {ce.name} (no base to overwrite)")
        if apply:
            try:
                shutil.copy2(str(ce), str(target))
            except OSError as e:
                return False, f"copy failed: {e}"

    if apply:
        # Remove the Cleaned exe/ dir entirely (operator policy:
        # once applied, the staging dir is dead weight).
        try:
            shutil.rmtree(str(cd))
        except OSError as e:
            return True, ("applied " + "; ".join(actions)
                          + f" — but couldn't rmdir Cleaned: {e}")
    return True, "; ".join(actions)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("root", type=Path)
    ap.add_argument("--apply", action="store_true",
                    help="actually move/copy files; default is dry "
                         "run")
    ap.add_argument("--only", default="",
                    help="comma-separated list of substrings; only "
                         "process game dirs whose path contains one")
    args = ap.parse_args()

    if not args.root.exists():
        print(f"error: {args.root} not found", file=sys.stderr)
        return 1

    only = [s.strip() for s in args.only.split(",") if s.strip()]
    game_dirs = find_game_dirs(args.root)
    if only:
        game_dirs = [g for g in game_dirs
                     if any(s in str(g) for s in only)]

    print(f"processing {len(game_dirs)} game dir(s)")
    print()

    f1_changed = f1_skipped = 0
    cl_changed = cl_skipped = 0
    for gd in game_dirs:
        try:
            rel = gd.relative_to(args.root)
        except ValueError:
            rel = gd

        ok1, msg1 = lift_file1(gd, apply=args.apply)
        if ok1:
            f1_changed += 1
            verb = "lifted" if args.apply else "would lift"
            print(f"  [{verb}] file-1   {rel}: {msg1}")
        elif msg1 != "no file-1/":
            print(f"  [SKIP ] file-1   {rel}: {msg1}", file=sys.stderr)

        ok2, msg2 = apply_cleaned(gd, apply=args.apply)
        if ok2:
            cl_changed += 1
            verb = "applied" if args.apply else "would apply"
            print(f"  [{verb}] cleaned  {rel}: {msg2}")
        elif msg2 != "no Cleaned exe/":
            print(f"  [SKIP ] cleaned  {rel}: {msg2}", file=sys.stderr)

    print()
    verb1 = "lifted" if args.apply else "would lift"
    verb2 = "applied" if args.apply else "would apply"
    print(f"{verb1} file-1 in   {f1_changed} game(s)")
    print(f"{verb2} Cleaned in  {cl_changed} game(s)")
    if not args.apply:
        print("(dry run — pass --apply to actually move/copy)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
