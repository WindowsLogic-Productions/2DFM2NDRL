#!/usr/bin/env python3
"""Aggregate FM2K game metadata from all known sources -> registry.json.

Inputs (any subset can be present):
  /mnt/c/games/2dfm/**/metadata.json  — MyAbandonware scrape
  tools/ia_scrape.json                — Internet Archive scrape (run
                                        scrape_archive_org.py first)
  games/registry_overrides.json       — manual hand-edits (highest
                                        priority; used to fix bad
                                        scrape titles, add banners,
                                        etc.)

Output:
  games/registry.json — one record per `game_id` (= exe stem the
  launcher sends in `join_room`). Schema documented below in
  EMPTY_REGISTRY_RECORD. Hub serves this via /api/v1/registry; the
  launcher caches it; the stats site renders friendly names + banners
  off it.

Run:    python3 tools/build_registry.py
Deps:   stdlib only.
"""

from __future__ import annotations

import json
import re
import sys
import zipfile
from pathlib import Path
from typing import Any


REPO_ROOT = Path(__file__).resolve().parent.parent
GAMES_DIR_DEFAULT = Path("/mnt/c/games/2dfm")
IA_SCRAPE_PATH = REPO_ROOT / "tools" / "ia_scrape.json"
OVERRIDES_PATH = REPO_ROOT / "games" / "registry_overrides.json"
OUT_PATH = REPO_ROOT / "games" / "registry.json"


# Canonical record shape. New scrapers add to `_raw[<source>]` rather
# than mutating top-level fields directly so the merge stays
# deterministic and we never lose source provenance.
EMPTY_REGISTRY_RECORD: dict[str, Any] = {
    "game_id": "",          # exe stem, lowercased — matches hub join_room
    "name": "",             # canonical display name
    "alt_names": [],        # alternate titles seen across sources
    "engine": "FM2K",       # FM2K | FM95
    "year": "",
    "developer": "",
    "publisher": "",
    "exe_stems": [],        # all known exe stems pointing at this game
    "kgt_filename": "",     # primary .kgt; needed for the hash check
    "homepage": "",
    "download_url": "",
    "banner_url": "",       # 16:9 banner for the stats site grid
    "thumb_url": "",        # square thumb for the launcher game-picker
    "sources": [],          # ["MyAbandonware", "archive.org", "manual"]
    "characters": [],       # [{ id, name, thumb_url? }, ...] (pending)
    "stages": [],           # [{ id, name }, ...] (pending)
    "_raw": {},             # per-source raw dump for forensics
}


def slugify(s: str) -> str:
    """Match-key for fuzzy dedup across sources.

    Lowercase, strip non-alphanum. "Kensei Shoujo" / "kensei-shoujo-hxx"
    / "Kensei_Shoujo" all collapse to "kenseishoujo". MyAbandonware
    appends a hash-suffix like "-hxx" or "-hy1" that we strip below.
    """
    s = s.lower()
    s = re.sub(r"[^a-z0-9]+", "", s)
    # Trim trailing 2-3 char alphanumeric MyAbandonware id suffix
    # (matches "hxx", "hy1", "fb6" — all 3 chars; not greedy enough
    # to eat real word endings since those land on word breaks above).
    s = re.sub(r"(?:hxx|hy\d|fb\d)$", "", s)
    return s


def derive_exe_stem(zip_path: Path | None, fallback_name: str) -> str | None:
    """Inspect a Win zip if present to pull the executable's stem.

    MyAbandonware downloads ship as `<Name>_Win_JA.zip` containing a
    `<game>.exe` at root. We don't unzip — just read the central
    directory to find candidate .exe entries and pick the first one
    that isn't an installer / unins / lilithport / antimicrox.
    """
    if zip_path is None or not zip_path.exists():
        return None
    try:
        with zipfile.ZipFile(zip_path) as z:
            exes = [n for n in z.namelist() if n.lower().endswith(".exe")]
    except zipfile.BadZipFile:
        return None
    if not exes:
        return None
    # Filter installer / launcher noise, prefer shortest plausible name.
    SKIP = ("unins", "setup", "install", "antimicrox", "lilithport",
            "update", "launcher")
    candidates = [e for e in exes if not any(s in e.lower() for s in SKIP)]
    if not candidates:
        candidates = exes
    candidates.sort(key=lambda n: (n.count("/"), len(n)))
    stem = Path(candidates[0]).stem.lower()
    return stem


def load_myabandonware_records(games_dir: Path) -> list[dict[str, Any]]:
    """Walk <games_dir>/**/metadata.json — produced by an earlier
    one-off MyAbandonware scrape. Each metadata.json sits alongside
    a single zip we can fingerprint for the exe stem."""
    out: list[dict[str, Any]] = []
    if not games_dir.exists():
        print(f"warn: {games_dir} not present, skipping MyAbandonware merge")
        return out
    for meta_path in games_dir.rglob("metadata.json"):
        try:
            raw = json.loads(meta_path.read_text(encoding="utf-8"))
        except Exception as e:
            print(f"  skip {meta_path}: {e}")
            continue
        # Find sibling zip for exe-stem inference.
        zips = list(meta_path.parent.glob("*.zip"))
        zip_path = zips[0] if zips else None
        exe_stem = derive_exe_stem(zip_path, raw.get("name", ""))
        rec = dict(EMPTY_REGISTRY_RECORD)
        rec["_raw"] = {"MyAbandonware": raw}
        rec["name"] = raw.get("name", "")
        rec["alt_names"] = raw.get("alternative names", [])
        rec["year"] = (raw.get("year") or "").strip()
        rec["developer"] = "" if raw.get("developer") == "empty" \
                              else (raw.get("developer") or "")
        rec["publisher"] = "" if raw.get("publisher") == "empty" \
                              else (raw.get("publisher") or "")
        rec["homepage"] = raw.get("url", "")
        rec["download_url"] = raw.get("download", "")
        rec["sources"] = ["MyAbandonware"]
        if exe_stem:
            rec["exe_stems"] = [exe_stem]
            rec["game_id"] = exe_stem
        else:
            # Fallback: slug as game_id placeholder. Override file
            # can fix this later when someone manually maps the right
            # exe stem.
            rec["game_id"] = slugify(rec["name"])
        out.append(rec)
    print(f"loaded {len(out)} MyAbandonware records from {games_dir}")
    return out


def load_ia_records(path: Path) -> list[dict[str, Any]]:
    """Convert tools/ia_scrape.json output into registry records."""
    if not path.exists():
        print(f"info: {path.name} not present "
              f"(run scrape_archive_org.py to generate)")
        return []
    raw_list = json.loads(path.read_text(encoding="utf-8"))
    out: list[dict[str, Any]] = []
    for raw in raw_list:
        rec = dict(EMPTY_REGISTRY_RECORD)
        rec["_raw"] = {"archive.org": raw}
        rec["name"] = raw.get("title", "")
        rec["year"] = (raw.get("year") or "")[:4]
        rec["developer"] = raw.get("creator", "")
        rec["homepage"] = raw.get("homepage", "")
        rec["download_url"] = raw.get("download", "")
        rec["sources"] = ["archive.org"]
        # Try to pull an exe stem from the file list. IA bundles often
        # contain the install root with the game's exe at top level.
        kgt = next((Path(f).stem for f in raw.get("files") or []
                    if f.lower().endswith(".kgt")), "")
        if kgt:
            rec["kgt_filename"] = kgt + ".kgt"
            # Convention: most FM2K games ship with exe and .kgt sharing
            # the same stem. Match-or-fallback to slugified title.
            rec["exe_stems"] = [kgt.lower()]
            rec["game_id"] = kgt.lower()
        else:
            rec["game_id"] = slugify(rec["name"])
        out.append(rec)
    print(f"loaded {len(out)} archive.org records from {path.name}")
    return out


def load_overrides(path: Path) -> list[dict[str, Any]]:
    """Hand-edited corrections / additions. Shape matches the registry
    schema directly so an entry can either patch existing fields by
    `game_id` or introduce a wholly new record."""
    if not path.exists():
        return []
    raw = json.loads(path.read_text(encoding="utf-8"))
    print(f"loaded {len(raw)} override records from {path.name}")
    return raw


def merge(records: list[dict[str, Any]],
          overrides: list[dict[str, Any]]) -> list[dict[str, Any]]:
    """Dedup by slug(name) ∪ exe_stems. First record seen wins for
    scalar fields; subsequent records add to alt_names / sources / _raw.
    Overrides apply last as field-level patches."""
    by_key: dict[str, dict[str, Any]] = {}

    # Index records by BOTH possible keys (exe_stem AND slugified name)
    # so a MyAbandonware record keyed under stem and an IA record keyed
    # under name still merge when their slugs collide. Dedup happens by
    # slug primarily; exe_stem only wins when the slugged name is empty.
    def slug_key(rec: dict[str, Any]) -> str:
        slug = slugify(rec.get("name", ""))
        if slug:
            return f"slug:{slug}"
        stems = rec.get("exe_stems") or []
        if stems:
            return f"stem:{stems[0]}"
        return f"id:{rec.get('_raw', {}).get('archive.org', {}).get('ia_identifier', '')}"

    def key_for(rec: dict[str, Any]) -> str:
        return slug_key(rec)

    for rec in records:
        k = key_for(rec)
        if k not in by_key:
            by_key[k] = dict(rec)
            by_key[k]["_raw"] = dict(rec["_raw"])
            continue
        existing = by_key[k]
        # Merge: keep first scalar; union list fields + _raw.
        for src in rec.get("sources", []):
            if src not in existing["sources"]:
                existing["sources"].append(src)
        for nm in rec.get("alt_names", []) + [rec.get("name")]:
            if nm and nm != existing["name"] and nm not in existing["alt_names"]:
                existing["alt_names"].append(nm)
        existing["_raw"].update(rec.get("_raw", {}))
        # Fill empty scalars from later sources rather than overwrite.
        for fld in ("year", "developer", "publisher", "homepage",
                    "download_url", "kgt_filename"):
            if not existing.get(fld) and rec.get(fld):
                existing[fld] = rec[fld]

    # Apply hand-edits last.
    for ov in overrides:
        gid = ov.get("game_id")
        if not gid:
            print(f"  override missing game_id, skipping: {ov}")
            continue
        # Find by exe_stem first, then by game_id direct match.
        target_key = next((k for k, v in by_key.items()
                           if gid in v.get("exe_stems", []) or
                           v.get("game_id") == gid), None)
        if target_key:
            by_key[target_key].update({k: v for k, v in ov.items()
                                       if k != "_raw"})
        else:
            # New record from scratch via override.
            rec = dict(EMPTY_REGISTRY_RECORD)
            rec.update(ov)
            rec["sources"] = list(set(rec.get("sources", []) + ["manual"]))
            by_key[f"stem:{gid}"] = rec

    # Deterministic output ordering.
    return sorted(by_key.values(),
                  key=lambda r: (r.get("game_id") or "").lower())


def main() -> int:
    games_dir = GAMES_DIR_DEFAULT
    if len(sys.argv) > 1:
        games_dir = Path(sys.argv[1])

    mw = load_myabandonware_records(games_dir)
    ia = load_ia_records(IA_SCRAPE_PATH)
    ov = load_overrides(OVERRIDES_PATH)

    merged = merge(mw + ia, ov)

    OUT_PATH.parent.mkdir(parents=True, exist_ok=True)
    OUT_PATH.write_text(json.dumps(merged, indent=2, ensure_ascii=False) + "\n",
                        encoding="utf-8")
    print(f"wrote {len(merged)} records to {OUT_PATH.relative_to(REPO_ROOT)}")
    print(f"  with exe_stem set: "
          f"{sum(1 for r in merged if r.get('exe_stems'))}")
    print(f"  needing manual game_id fix: "
          f"{sum(1 for r in merged if not r.get('exe_stems'))}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
