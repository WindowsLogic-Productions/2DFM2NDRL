#!/usr/bin/env python3
"""Dedupe registry.json by game_id, merging duplicates.

The registry was built by stitching MyAbandonware + Internet Archive
scrapes. 7 game_ids ended up with 2-3 duplicate records each (`reshuffle`
×3 is the worst). The hub's `load_registry` keys by game_id, so the
later duplicates clobber the earlier ones — 8 records lost in a 141-row
registry, surfaced as `133 cataloged` in the games grid.

This pass groups by game_id and merges all records with the same id
into one canonical record:
  - For scalar fields (name, year, developer, ...): prefer non-empty
    values, breaking ties by record source priority (archive.org's
    metadata tends to be richer than MyAbandonware's).
  - For list fields (sources, exe_stems, alt_names, characters,
    stages): union deduplicated.
  - For dict fields (downloads, _raw): union (later overwrites
    earlier on key collision; doesn't matter much since these are
    just provenance).

Result: 133 distinct records, no data lost. Hub will show all of
them with no clobbering.

Always dry-run by default. Pass --apply to rewrite registry.json.

Usage:
    python3 tools/dedupe_registry.py
    python3 tools/dedupe_registry.py --apply
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from collections import defaultdict
from pathlib import Path


REGISTRY_PATH = (Path(__file__).resolve().parent.parent
                 / "games" / "registry.json")

SCALAR_FIELDS = ("name", "year", "developer", "publisher",
                 "homepage", "download_url", "kgt_filename",
                 "banner_url", "thumb_url", "engine")
LIST_FIELDS = ("alt_names", "sources", "exe_stems",
               "characters", "stages")
DICT_FIELDS = ("downloads", "_raw")


def merge_records(group: list[dict]) -> dict:
    """Merge all records in `group` (same game_id) into one."""
    out: dict = {}
    # game_id is shared by definition
    out["game_id"] = group[0].get("game_id", "")

    # Source-priority sort: archive.org > MyAbandonware > others.
    # When multiple records have a non-empty value for the same
    # scalar field, we want the higher-priority source's value.
    def src_priority(r: dict) -> int:
        srcs = r.get("sources") or []
        if "archive.org" in srcs:
            return 0
        if "MyAbandonware" in srcs:
            return 1
        return 2
    ordered = sorted(group, key=src_priority)

    for f in SCALAR_FIELDS:
        for r in ordered:
            v = r.get(f)
            if v not in (None, "", [], {}):
                out[f] = v
                break
        else:
            # All empty — keep the first record's value (often "")
            out[f] = ordered[0].get(f, "")

    for f in LIST_FIELDS:
        seen = []
        seen_set = set()
        for r in ordered:
            for item in (r.get(f) or []):
                # Dedupe primitives + dicts (rare but possible)
                key = json.dumps(item, sort_keys=True) \
                      if isinstance(item, (dict, list)) else item
                if key not in seen_set:
                    seen.append(item)
                    seen_set.add(key)
        out[f] = seen

    for f in DICT_FIELDS:
        merged: dict = {}
        for r in ordered:
            d = r.get(f) or {}
            if isinstance(d, dict):
                merged.update(d)
        out[f] = merged

    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--apply", action="store_true")
    args = ap.parse_args()

    if not REGISTRY_PATH.exists():
        print(f"error: {REGISTRY_PATH} missing", file=sys.stderr)
        return 1
    recs = json.loads(REGISTRY_PATH.read_text(encoding="utf-8"))

    by_id: dict[str, list[dict]] = defaultdict(list)
    no_id: list[dict] = []
    for r in recs:
        gid = r.get("game_id", "")
        if not gid:
            no_id.append(r)
            continue
        by_id[gid].append(r)

    n_before = len(recs)
    n_distinct = len(by_id)
    n_dupes_groups = sum(1 for g in by_id.values() if len(g) > 1)
    n_dupe_records = sum(len(g) - 1 for g in by_id.values()
                          if len(g) > 1)

    merged: list[dict] = []
    for gid in sorted(by_id.keys()):
        group = by_id[gid]
        if len(group) == 1:
            merged.append(group[0])
        else:
            merged.append(merge_records(group))
    # Preserve no-id records as-is at the end (shouldn't happen for
    # current registry but be safe).
    merged.extend(no_id)

    print(f"input records:        {n_before}")
    print(f"distinct game_ids:    {n_distinct}")
    print(f"duplicate-id groups:  {n_dupes_groups}")
    print(f"records dropped:      {n_dupe_records}")
    print(f"output records:       {len(merged)}")
    print()

    if n_dupes_groups > 0:
        print("merged dupes:")
        for gid, group in sorted(by_id.items()):
            if len(group) > 1:
                # Show what we kept
                m = merge_records(group)
                print(f"  {gid}  ×{len(group)}  → "
                      f"name='{m.get('name', '')[:40]}'  "
                      f"sources={m.get('sources', [])}")
        print()

    # Engine distribution
    engines: dict[str, int] = {}
    for r in merged:
        e = r.get("engine") or "<none>"
        engines[e] = engines.get(e, 0) + 1
    print(f"engine distribution after dedupe: {engines}")
    print()

    if not args.apply:
        print(f"(dry run — pass --apply to rewrite "
              f"{REGISTRY_PATH.name})")
        return 0

    tmp = REGISTRY_PATH.with_suffix(".json.tmp")
    tmp.write_text(
        json.dumps(merged, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8")
    os.replace(str(tmp), str(REGISTRY_PATH))
    print(f"wrote {REGISTRY_PATH}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
