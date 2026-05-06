#!/usr/bin/env python3
"""Merge /mnt/d/games/2dfm + 2dfm_ia into engine-bucketed layout.

Final tree:
  /mnt/d/games/
    ├── fm2k/    # FM2K_2nd games (1,204,224-byte engine — what
    │            # our rollback launcher targets)
    │   ├── _NODEV/<game>/         IA records w/ no developer +
    │   │                          naked drops at top-level
    │   ├── <Studio>/<Game>/       curated tree's studio layout
    │   │                          preserved as-is
    │   └── <Studio>/<Game>/<Variant>/
    │                              (file-1, Cleaned exe,
    │                              Version 02, etc.)
    └── fm95/    # FM95 games (~384 KB engine — different binary,
                 # not handled by current rollback hook)
        ├── _NODEV/<game>/
        ├── CPW/                   was 2dfm/fm95/CPW
        └── urobura/{ko,og}/       was 2dfm/urobura/{ko,og}

Engine detection: find the canonical exe in each game-dir (matches
its sibling .kgt by stem). Size at exactly 1,204,224 bytes → FM2K_2nd
(after we ran apply_clean_engine.py everything legit FM2K landed
here). Anything smaller → FM95 / older. Anything larger → still
packed (warn loudly — pipeline missed it).

Game-dir discovery:
  - Every dir containing metadata.json is a game-dir (47 + 55 = 102).
  - Top-level dirs lacking metadata.json but containing a .kgt+.exe
    pair at root are "naked" game-dirs (15 in curated 2dfm/).
  - urobura/ + fm95/ are multi-game containers — descend one level
    and treat each child as a naked game-dir.

Always dry-run by default. Pass --apply to actually move.

Usage:
    python3 tools/sort_by_engine.py
    python3 tools/sort_by_engine.py --apply
    python3 tools/sort_by_engine.py --dest /mnt/d/games --apply
"""

from __future__ import annotations

import argparse
import json
import re
import shutil
import sys
from dataclasses import dataclass, field
from pathlib import Path


SOURCES_DEFAULT = [
    Path("/mnt/d/games/2dfm"),
    Path("/mnt/d/games/2dfm_ia"),
]
DEST_DEFAULT = Path("/mnt/d/games")

ENGINE_BASELINE = 1_204_224

# Naked dirs whose name shouldn't become a studio (engine buckets,
# the source roots themselves).
GENERIC_CONTAINERS = {"fm95", "fm2k", "2dfm", "2dfm_ia",
                      "_NODEV", "games"}

# Filesystem-unsafe characters in game/studio names.
FS_BAD = re.compile(r'[<>:"/\\|?*\x00-\x1f]')


@dataclass
class GameMove:
    src: Path
    engine: str
    studio: str
    name: str
    via_metadata: bool       # had a metadata.json
    canonical_exe_size: int  # for the engine choice
    notes: list[str] = field(default_factory=list)

    @property
    def dest_rel(self) -> Path:
        return Path(self.engine) / self.studio / self.name


def find_first_kgt(d: Path) -> Path | None:
    """Find the first .kgt at d's top level (case-insensitive)."""
    try:
        for p in d.iterdir():
            if p.is_file() and p.suffix.lower() == ".kgt":
                return p
    except OSError:
        return None
    return None


def find_canonical_exe(game_dir: Path,
                       max_depth: int = 6) -> tuple[Path | None, int]:
    """Find the canonical kgt+exe pair anywhere in game_dir's
    subtree, up to max_depth levels. Returns (exe_path or None,
    size or 0).

    Some game-dirs have deeply-nested layouts even after flatten
    (Emblem of Red Shuffle keeps a 5-deep chain because of
    collisions; Otepuri has 4-deep nesting from a zip-of-zip)."""
    queue: list[tuple[Path, int]] = [(game_dir, 0)]
    while queue:
        cur, depth = queue.pop(0)  # BFS — take the shallowest hit
        if depth > max_depth:
            continue
        kgt = find_first_kgt(cur)
        if kgt is not None:
            target = cur / f"{kgt.stem}.exe"
            if target.exists():
                return target, target.stat().st_size
            # Same-dir fallback: any .exe at all.
            try:
                for p in cur.iterdir():
                    if p.is_file() and p.suffix.lower() == ".exe":
                        return p, p.stat().st_size
            except OSError:
                pass
        try:
            for p in sorted(cur.iterdir()):
                if p.is_dir():
                    queue.append((p, depth + 1))
        except OSError:
            continue
    return None, 0


def detect_engine(exe_size: int) -> str:
    if exe_size == 0:
        return "unknown"
    if exe_size < ENGINE_BASELINE:
        return "fm95"
    if exe_size == ENGINE_BASELINE:
        return "fm2k"
    return "fm2k"  # > baseline — still packed; treat as fm2k


def sanitize(name: str) -> str:
    name = FS_BAD.sub("_", name).strip()
    # Collapse repeated separator chars + trim trailing dots/spaces
    # (NTFS doesn't allow dirs ending with those).
    name = re.sub(r"\s+", " ", name).strip(" .")
    return name or "untitled"


def read_metadata(game_dir: Path) -> dict | None:
    p = game_dir / "metadata.json"
    if not p.is_file():
        return None
    try:
        return json.loads(p.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return None


def resolve_studio_and_name(src: Path,
                            game_dir: Path,
                            meta: dict | None,
                            ) -> tuple[str, str, list[str]]:
    """Return (studio, name, notes)."""
    notes: list[str] = []
    if meta is not None:
        # IA-generated metadata has the developer field but it's
        # often empty; curated has a "developer" we trust. The
        # MyAbandonware-sourced curated metadata uses the literal
        # string "empty" for unknown publishers/developers — treat
        # those as no-developer too.
        dev = (meta.get("developer") or "").strip()
        if dev.lower() in ("", "empty", "unknown", "n/a"):
            dev = ""
        name_raw = (meta.get("name") or "").strip()
        # Curated tree puts the game-dir under a studio dir already.
        # If src looks like .../<studio>/<game>/, prefer that
        # structural studio over the metadata field (the dirs were
        # painstakingly hand-organized).
        parent = game_dir.parent
        if parent != src and parent.name not in GENERIC_CONTAINERS:
            studio = parent.name
            notes.append(f"studio from parent dir ({parent.name})")
        elif dev:
            studio = dev
        else:
            studio = "_NODEV"
        name = name_raw or game_dir.name
    else:
        # Naked dir — no metadata. Use parent.name as studio if it's
        # a multi-game container (urobura), else _NODEV.
        parent = game_dir.parent
        if parent == src or parent.name in GENERIC_CONTAINERS:
            studio = "_NODEV"
        else:
            studio = parent.name
            notes.append(f"naked, studio from parent ({parent.name})")
        name = game_dir.name
    return sanitize(studio), sanitize(name), notes


def has_kgt_anywhere(d: Path, max_depth: int = 5) -> bool:
    """Cheap check: does this subtree contain any .kgt within
    max_depth?"""
    queue = [(d, 0)]
    while queue:
        cur, depth = queue.pop()
        if depth > max_depth:
            continue
        try:
            for p in cur.iterdir():
                if p.is_file() and p.suffix.lower() == ".kgt":
                    return True
                if p.is_dir():
                    queue.append((p, depth + 1))
        except OSError:
            continue
    return False


def _kgt_bearing_children(d: Path) -> list[Path]:
    """Children of d that contain a .kgt anywhere in their subtree."""
    out: list[Path] = []
    try:
        for c in sorted(d.iterdir()):
            if c.is_dir() and has_kgt_anywhere(c):
                out.append(c)
    except OSError:
        pass
    return out


def _classify_naked(child: Path,
                    seen: set[Path],
                    known_studios: set[Path]) -> list[Path]:
    """Recursively decide whether `child` is a single game-dir or a
    multi-game container, returning the naked game-dir(s) inside.

    - kgt at top of child → child is the game-dir.
    - if child is a known studio (had sentinel-bearing children):
      always drill in — its remaining unsentinel'd children are
      orphan games (e.g. Dream Illusory Valley/Colors after its
      sibling Colors Party - Lost in Eater was sentinel-found and
      moved). Even with only one orphan, don't lift the whole
      studio — keep the studio dimension.
    - else: count kgt-bearing children.
        - 0 → no game here, return [].
        - 1 → child is a single deeply-nested game-dir (e.g.
          `DDND V2-1-2/V2-1-1/GAME/GAME.kgt`).
        - 2+ → multi-game container (urobura, fm95)."""
    if child.resolve() in seen:
        return []
    if find_first_kgt(child) is not None:
        return [child]
    kbcs = [c for c in _kgt_bearing_children(child)
            if c.resolve() not in seen]
    fresh = []
    for c in kbcs:
        if any(s.is_relative_to(c.resolve()) for s in seen):
            continue
        fresh.append(c)
    if len(fresh) == 0:
        return []
    is_studio = child.resolve() in known_studios
    # Studio detection by structure: if ANY immediate kgt-bearing
    # child has its OWN .kgt at top (i.e. that child looks like a
    # game-dir on its own), `child` is a studio — drill in. Catches
    # post-move state where sentinel-bearing siblings have already
    # been relocated, leaving an "orphan" Retsuzan/SCWU Infinity
    # or Dream Illusory Valley/Colors situation that we'd otherwise
    # mis-lift.
    has_topkgt_child = any(find_first_kgt(c) is not None
                           for c in fresh)
    if len(fresh) == 1 and not is_studio and not has_topkgt_child:
        # Single deeply-nested game with the outer dir as its
        # canonical name (DDND V2-1-2/V2-1-1/GAME/GAME.kgt).
        return [child]
    # Multi-game container, known studio, or single-orphan inside
    # a studio — recurse.
    out: list[Path] = []
    for c in fresh:
        out.extend(_classify_naked(c, seen, known_studios))
    return out


def discover_game_dirs(root: Path) -> list[tuple[Path, bool]]:
    """Return (game_dir, has_metadata). Two-pass:
      1. metadata.json sentinels — canonical game-dirs.
      2. naked-dir classifier (handles single-deep, multi-game
         container, urobura/cadenza-style mixed cases)."""
    out: list[tuple[Path, bool]] = []
    seen: set[Path] = set()
    if not root.exists():
        return out

    # Pass 1: metadata.json sentinels.
    for sentinel in root.rglob("metadata.json"):
        if sentinel.is_file():
            gd = sentinel.parent
            out.append((gd, True))
            seen.add(gd.resolve())

    # A "known studio" is any dir that had sentinel-bearing
    # children. We treat it differently in classification: orphans
    # left behind get drilled in, not lifted to the studio level.
    known_studios: set[Path] = set()
    for sentinel_gd, _ in out:
        if sentinel_gd.parent != root:
            known_studios.add(sentinel_gd.parent.resolve())

    # Pass 2: walk root's immediate children. Classifier handles
    # the studio / nested / multi-game-container distinction.
    for child in sorted(root.iterdir()):
        if not child.is_dir():
            continue
        if child.resolve() in seen:
            continue
        for gd in _classify_naked(child, seen, known_studios):
            if gd.resolve() in seen:
                continue
            out.append((gd, False))
            seen.add(gd.resolve())
    return sorted(out, key=lambda t: str(t[0]))


def plan(sources: list[Path]) -> list[GameMove]:
    moves: list[GameMove] = []
    for src in sources:
        for game_dir, via_metadata in discover_game_dirs(src):
            meta = read_metadata(game_dir) if via_metadata else None
            exe, size = find_canonical_exe(game_dir)
            engine = detect_engine(size)
            studio, name, notes = resolve_studio_and_name(
                src, game_dir, meta)
            mv = GameMove(src=game_dir,
                          engine=engine,
                          studio=studio,
                          name=name,
                          via_metadata=via_metadata,
                          canonical_exe_size=size,
                          notes=notes)
            if exe is None:
                mv.notes.append("no canonical .exe found")
            elif size > ENGINE_BASELINE:
                mv.notes.append(
                    f"exe still packed ({size:,} B > baseline)")
            moves.append(mv)
    return moves


def apply_moves(moves: list[GameMove], dest_root: Path,
                *, dry_run: bool) -> tuple[int, int, int]:
    n_ok = n_skip = n_fail = 0
    for mv in moves:
        target = dest_root / mv.dest_rel
        if mv.engine == "unknown":
            print(f"  [skip] {mv.src}  → engine unknown "
                  f"(exe size {mv.canonical_exe_size})",
                  file=sys.stderr)
            n_skip += 1
            continue
        if target.exists():
            try:
                # Allow re-targeting to a path that's actually the
                # same dir (no-op idempotent check after a partial
                # apply).
                if target.resolve() == mv.src.resolve():
                    print(f"  [done] {mv.dest_rel}  (already in "
                          f"place)")
                    n_skip += 1
                    continue
            except OSError:
                pass
            print(f"  [skip] {mv.dest_rel}  (target exists, won't "
                  f"clobber)", file=sys.stderr)
            n_skip += 1
            continue
        verb = "would mv" if dry_run else "mv     "
        note_str = (f"  ({'; '.join(mv.notes)})"
                    if mv.notes else "")
        print(f"  {verb}  {mv.src.relative_to(mv.src.parents[1])} "
              f"→ {mv.dest_rel}{note_str}")
        if dry_run:
            n_ok += 1
            continue
        try:
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.move(str(mv.src), str(target))
            n_ok += 1
        except OSError as e:
            print(f"     ERR: {e}", file=sys.stderr)
            n_fail += 1
    return n_ok, n_skip, n_fail


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", action="append", type=Path,
                    default=None,
                    help="source root (repeat for multiple). "
                         f"default: {SOURCES_DEFAULT}")
    ap.add_argument("--dest", type=Path, default=DEST_DEFAULT,
                    help=f"engine-bucketed destination root. "
                         f"default: {DEST_DEFAULT}")
    ap.add_argument("--apply", action="store_true",
                    help="actually move; default is dry run")
    args = ap.parse_args()

    sources = args.src or SOURCES_DEFAULT
    moves = plan(sources)
    print(f"discovered {len(moves)} game-dir(s) across "
          f"{len(sources)} source root(s)")
    print()

    by_engine: dict[str, int] = {}
    for mv in moves:
        by_engine[mv.engine] = by_engine.get(mv.engine, 0) + 1
    for eng in sorted(by_engine):
        print(f"  {eng:10s} {by_engine[eng]}")
    print()

    n_ok, n_skip, n_fail = apply_moves(moves, args.dest,
                                       dry_run=not args.apply)
    print()
    verb = "moved" if args.apply else "would move"
    print(f"{verb}: {n_ok}, skipped: {n_skip}, "
          f"failed: {n_fail}")
    if not args.apply:
        print("(dry run — pass --apply to actually move)")
    return 0 if n_fail == 0 else 2


if __name__ == "__main__":
    sys.exit(main())
