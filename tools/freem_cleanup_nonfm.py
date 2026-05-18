#!/usr/bin/env python3
"""Remove freem download bytes for games that classified as engine=None
after the strict re-crawl. Use after freem_crawl.py confirms the
listing-membership-based engine. Doesn't touch metadata JSON — just
clears the downloaded zip/exe + the `pulled` field on the record so a
future re-pull (when classification flips) starts clean.
"""
import argparse, json, shutil
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
GAMES_DIR = REPO / "data" / "freem" / "games"
DL_DIR    = REPO / "data" / "freem" / "downloads"


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    n_wiped = 0
    bytes_freed = 0
    for p in sorted(GAMES_DIR.glob("*.json")):
        rec = json.loads(p.read_text(encoding="utf-8"))
        gid = rec["freem_id"]
        if rec.get("engine") in ("FM2K", "FM95"):
            continue
        # engine is None — non-FM noise. Wipe pulled bytes if any.
        target_dir = DL_DIR / gid
        if target_dir.exists():
            sz = sum(f.stat().st_size for f in target_dir.iterdir() if f.is_file())
            print(f"  wipe {gid} (engine=None, {rec.get('title','')[:50]!r}) — "
                  f"{sz/1e6:.1f} MB")
            if not args.dry_run:
                shutil.rmtree(target_dir)
                rec.pop("pulled", None)
                p.write_text(json.dumps(rec, ensure_ascii=False, indent=2),
                             encoding="utf-8")
            n_wiped += 1
            bytes_freed += sz
    print(f"\nwiped {n_wiped} dirs · freed {bytes_freed/1e6:.1f} MB")
    if args.dry_run:
        print("(dry-run — nothing actually deleted)")


if __name__ == "__main__":
    main()
