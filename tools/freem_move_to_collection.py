#!/usr/bin/env python3
"""Move freem-extracted game trees into the user's main collection at
/mnt/d/Games/fm2k/<game-folder>/.

For each pulled freem game, decide the target folder name:
  - If the extraction has a single top-level subfolder (common for
    zipped doujin distributions: 闘闘/闘闘.exe + assets), use that
    folder name verbatim.
  - If the extraction has files at the root (bare-exe pulls like
    CalendarRave.exe), create a folder named after the game's title
    from freem metadata, then move files in.
  - Flatten double-nested folders (some zips have Foo/Foo/...).

Skips when the target folder already exists in the collection (e.g.
the user manually extracted that game previously). Logs for review.

After move, the source directory under data/freem/extracted/<id>/ is
removed and the record's `extracted.collection_path` is set so we
remember where it ended up.
"""

import argparse
import json
import shutil
import sys
from pathlib import Path

REPO        = Path(__file__).resolve().parents[1]
GAMES_DIR   = REPO / "data" / "freem" / "games"
EXTRACTED   = REPO / "data" / "freem" / "extracted"
COLLECTION  = Path("/mnt/d/Games/fm2k")


def safe_target_name(rec: dict) -> str:
    """Pick a clean folder name for bare-exe pulls. Prefer title;
    strip characters illegal on Windows (\\/:*?\"<>|)."""
    name = (rec.get("title") or rec["freem_id"]).strip()
    bad = '\\/:*?"<>|'
    for c in bad:
        name = name.replace(c, "_")
    return name[:100]


def pick_source_folder(src_root: Path) -> tuple[Path, str]:
    """Decide what to move from. Returns (source_path, target_basename).

    - If src_root has exactly one subdir and no top-level files (or
      only a single ignorable file), descend into the subdir.
    - Handle double-nested case: Foo/Foo/<files> → use the inner Foo
      contents but name the target Foo.
    - Otherwise (bare-exe), src_root itself is the source.
    """
    children = list(src_root.iterdir())
    dirs = [c for c in children if c.is_dir()]
    files = [c for c in children if c.is_file()]

    # Bare files only (or files + no useful dir) → use src_root as is.
    if not dirs:
        return src_root, None  # caller picks a name from metadata

    # Single subdir at root, no important top-level files.
    if len(dirs) == 1 and len(files) == 0:
        sub = dirs[0]
        # Double-nested? e.g. Sirufuru_1_05/Sirufuru_1_05/...
        sub_children = list(sub.iterdir())
        sub_dirs = [c for c in sub_children if c.is_dir()]
        sub_files = [c for c in sub_children if c.is_file()]
        if (len(sub_dirs) == 1 and len(sub_files) == 0
                and sub_dirs[0].name == sub.name):
            # Use the deeper one but keep the outer name.
            return sub_dirs[0], sub.name
        return sub, sub.name

    # Multi-dir or mixed — use src_root as is; we'll keep its name
    # but that's unusual.
    return src_root, None


def merge_or_move(src: Path, dst: Path, dry: bool) -> dict:
    """Move src dir contents to dst. dst must already exist (we
    create it). Returns {moved: int, conflicts: list}."""
    moved = 0
    conflicts: list[str] = []
    if dry:
        return {"moved": "DRY", "conflicts": conflicts}
    dst.mkdir(parents=True, exist_ok=True)
    for child in src.iterdir():
        target = dst / child.name
        if target.exists():
            conflicts.append(child.name)
            continue
        shutil.move(str(child), str(target))
        moved += 1
    # If src is now empty, remove it.
    try:
        if not any(src.iterdir()):
            src.rmdir()
    except OSError:
        pass
    return {"moved": moved, "conflicts": conflicts}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--ids", default="",
                    help="comma-separated freem_ids (default: all pulled)")
    ap.add_argument("--apply", action="store_true",
                    help="actually move (default: dry-run)")
    args = ap.parse_args()

    if not COLLECTION.exists():
        print(f"collection root missing: {COLLECTION}", file=sys.stderr)
        return 1

    if args.ids:
        wanted = {s.strip() for s in args.ids.split(",") if s.strip()}
        paths = [GAMES_DIR / f"{i}.json" for i in wanted
                 if (GAMES_DIR / f"{i}.json").exists()]
    else:
        paths = sorted(GAMES_DIR.glob("*.json"))

    n_moved = n_skip = n_conflict = 0
    for rec_path in paths:
        rec = json.loads(rec_path.read_text(encoding="utf-8"))
        ex = rec.get("extracted")
        if not ex:
            continue
        if rec.get("collection_path"):
            n_skip += 1
            continue

        src_root = EXTRACTED / rec["freem_id"]
        if not src_root.exists():
            print(f"  SKIP   {rec['freem_id']:>6}  source missing: {src_root}")
            n_skip += 1
            continue

        src, target_name = pick_source_folder(src_root)
        if target_name is None:
            # Bare-exe pull — derive folder name from title.
            target_name = safe_target_name(rec)

        dst = COLLECTION / target_name
        if dst.exists():
            # Check if it's the same content (user manually extracted earlier).
            n_conflict += 1
            print(f"  CONFLICT  {rec['freem_id']:>6}  target exists: {dst}")
            print(f"            (source: {src})")
            continue

        action = "WOULD MOVE" if not args.apply else "MOVED"
        print(f"  {action}  {rec['freem_id']:>6}  → {dst}")
        print(f"           from {src.relative_to(EXTRACTED)}")

        if args.apply:
            result = merge_or_move(src, dst, dry=False)
            if result["conflicts"]:
                print(f"           file conflicts: {result['conflicts'][:5]}")
            rec["collection_path"] = str(dst)
            rec_path.write_text(json.dumps(rec, ensure_ascii=False, indent=2),
                                encoding="utf-8")
            # Clean up empty freem extracted dir if everything moved.
            try:
                if src_root.exists() and not any(src_root.iterdir()):
                    src_root.rmdir()
            except OSError:
                pass
            n_moved += 1
        else:
            n_moved += 1  # count as planned-to-move

    print(f"\n{'moved' if args.apply else 'would move'}: {n_moved}")
    print(f"skipped:  {n_skip}")
    print(f"conflicts: {n_conflict}")
    if not args.apply:
        print("\n(dry-run — pass --apply to actually move)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
