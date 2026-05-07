#!/usr/bin/env python3
"""Sync the on-disk engine bucket (fm2k/fm95) into registry.json.

The registry was built before tools/sort_by_engine.py existed, so
every record has `engine: "FM2K"` regardless of whether the game
is actually FM2K_2nd or FM95. The hub at /games reads that field
and renders an engine badge — currently mis-labeling 26 actually-
FM95 games as FM2K.

This pass uses games/catalog.json (which knows the truth — it was
built from the on-disk fm2k/ vs fm95/ bucket layout) and overlays
the engine field onto the matching registry records. Match keys
checked, in order:
  1. kgt_filename (case-insensitive, exact match)
  2. exe_stems intersection with normalized catalog stems
  3. game_id  ↔ catalog `name` normalized

Engine values mapped:
  catalog "fm2k_2nd" → registry "FM2K"
  catalog "fm95"     → registry "FM95"

Always dry-run by default. Pass --apply to actually rewrite the
registry. Atomic: writes to a tmp file then renames over the
original.

Usage:
    python3 tools/sync_engine_to_registry.py
    python3 tools/sync_engine_to_registry.py --apply
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parent.parent
CATALOG_PATH = REPO_ROOT / "games" / "catalog.json"
REGISTRY_PATH = REPO_ROOT / "games" / "registry.json"

ENGINE_MAP = {
    "fm2k_2nd": "FM2K",
    "fm95":     "FM95",
    "unknown":  None,  # leave registry alone if catalog can't decide
}


def normalize(s: str) -> str:
    return ((s or "").lower()
            .replace(" ", "").replace("-", "")
            .replace("_", "").replace("~", "")
            .replace(".", ""))


def build_catalog_index(catalog: list[dict]) -> dict:
    """Index catalog by every key we might match a registry record on.

    Returns {key: engine_label_or_None}. Missing keys = no engine
    info in catalog for that record."""
    out: dict[str, str | None] = {}
    for c in catalog:
        engine = ENGINE_MAP.get(c.get("engine"))
        if engine is None:
            continue
        # kgt filename (just the basename, original case kept for
        # one variant + lowercase for another)
        kgt = c.get("kgt") or ""
        if kgt:
            stem = kgt.rsplit("/", 1)[-1].rsplit(".", 1)[0]
            out[normalize(stem)] = engine
            out[stem.lower()] = engine
        # exe (basename stem)
        exe = c.get("exe") or ""
        if exe:
            stem = exe.rsplit("/", 1)[-1].rsplit(".", 1)[0]
            out[normalize(stem)] = engine
        # display name + IA ident
        out[normalize(c.get("name") or "")] = engine
        if c.get("ia_id"):
            out[normalize(c["ia_id"])] = engine
    out.pop("", None)
    return out


def lookup_engine(rec: dict, idx: dict) -> str | None:
    """Try every signal we have on the registry record against
    the catalog index. First hit wins."""
    keys: list[str] = []
    if rec.get("kgt_filename"):
        kf = rec["kgt_filename"]
        keys.append(normalize(kf.rsplit(".", 1)[0]))
        keys.append(kf.lower())
    for s in rec.get("exe_stems") or []:
        if s:
            keys.append(normalize(s))
    if rec.get("game_id"):
        keys.append(normalize(rec["game_id"]))
    if rec.get("name"):
        keys.append(normalize(rec["name"]))
    for k in keys:
        if k and k in idx:
            return idx[k]
    return None


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--apply", action="store_true",
                    help="rewrite registry.json in place; default "
                         "is dry run")
    args = ap.parse_args()

    if not CATALOG_PATH.exists():
        print(f"error: {CATALOG_PATH} missing — run "
              f"tools/build_game_catalog.py first", file=sys.stderr)
        return 1
    if not REGISTRY_PATH.exists():
        print(f"error: {REGISTRY_PATH} missing", file=sys.stderr)
        return 1

    catalog = json.loads(CATALOG_PATH.read_text(encoding="utf-8"))
    registry = json.loads(REGISTRY_PATH.read_text(encoding="utf-8"))
    idx = build_catalog_index(catalog)

    n_changed = 0
    n_unchanged = 0
    n_no_match = 0
    n_kept = 0
    by_engine: dict[str, int] = {}
    examples_changed: list[tuple[str, str, str]] = []

    for rec in registry:
        new_engine = lookup_engine(rec, idx)
        if new_engine is None:
            n_no_match += 1
            continue
        old_engine = rec.get("engine") or ""
        if old_engine == new_engine:
            n_unchanged += 1
            by_engine[new_engine] = by_engine.get(new_engine, 0) + 1
            continue
        # Change the field
        n_changed += 1
        by_engine[new_engine] = by_engine.get(new_engine, 0) + 1
        if len(examples_changed) < 10:
            examples_changed.append((rec.get("game_id", "?"),
                                     old_engine or "<none>",
                                     new_engine))
        rec["engine"] = new_engine

    print(f"registry records:       {len(registry)}")
    print(f"  matched to catalog:   {n_changed + n_unchanged}")
    print(f"  no catalog match:     {n_no_match}  (engine left as-is)")
    print()
    print(f"  engine changed:       {n_changed}")
    print(f"  engine unchanged:     {n_unchanged}")
    print()
    print(f"engine distribution after sync:")
    for eng, n in sorted(by_engine.items()):
        print(f"  {eng:8s}  {n}")
    print()
    if examples_changed:
        print(f"example changes:")
        for gid, old, new in examples_changed:
            print(f"  {gid:30s}  {old}  →  {new}")
        print()

    if not args.apply:
        print(f"(dry run — pass --apply to rewrite "
              f"{REGISTRY_PATH.name})")
        return 0

    # Atomic write
    tmp = REGISTRY_PATH.with_suffix(".json.tmp")
    tmp.write_text(
        json.dumps(registry, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8")
    os.replace(str(tmp), str(REGISTRY_PATH))
    print(f"wrote {REGISTRY_PATH}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
