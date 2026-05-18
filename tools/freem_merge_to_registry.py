#!/usr/bin/env python3
"""Merge freem.ne.jp crawl results into games/registry.json.

For each freem record:
  1. Try to fuzzy-match against an existing registry entry by
     normalized title (same logic atwiki_merge_to_registry uses).
  2. If matched: attach freem_id + author + a new resource row
     pointing at the freem detail page. Status bumps to at least
     KNOWN_RECOVERABLE (we know where to get it).
  3. If unmatched: emit it on stdout under NEW so a human / editor
     can decide whether to import it as a new registry row.

What gets attached to a matched row:
  - freem_id, freem_author (top-level fields for quick reference)
  - sources += "freem"
  - resources += {kind: "homepage", source: "freem-crawler",
                  url: detail_url, label: "freem"}
  - resources += {kind: "download", source: "freem-crawler",
                  url: download_url, label: "freem download",
                  desc: "version + size"}  iff a version is known
  - archive_status bumps from NEEDS_HUNT / NO_OUTBOUND →
                   KNOWN_RECOVERABLE (never downgrades HAVE_LOCAL / etc.)

Idempotent on re-run. Touching only rows that match a freem record.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from typing import Optional


REPO = Path(__file__).resolve().parents[1]
FREEM_INDEX  = REPO / "data" / "freem" / "index.json"
FREEM_GAMES  = REPO / "data" / "freem" / "games"
REGISTRY     = REPO / "games" / "registry.json"


def normalize(s: str) -> str:
    """Same normalization as atwiki_merge — strip versions / spaces /
    punctuation / brackets so 'Foo v1.0' and 'foo' collapse."""
    s = (s or "").lower()
    s = re.sub(r"_?ver[_\-\s]*\d[\d\._]*", "", s)
    s = re.sub(r"\s*v?\d+(?:\.\d+)+\s*$", "", s)
    s = re.sub(r"[\s_\-　　]+", "", s)
    s = re.sub(r"[【】（）()「」『』:：・/\\|]+", "", s)
    return s


def fuzzy_score(a: str, b: str) -> tuple[float, str]:
    na, nb = normalize(a), normalize(b)
    if not na or not nb:
        return 0.0, ""
    if na == nb:
        return 1.0, "exact"
    if na in nb or nb in na:
        shorter, longer = sorted([na, nb], key=len)
        return len(shorter) / len(longer), "substring"
    return 0.0, ""


def best_match_for(freem_rec: dict, registry: list[dict],
                   threshold: float = 0.6
                   ) -> Optional[tuple[int, float, str]]:
    """Return (registry_idx, score, method) for the best registry
    match, or None below threshold. Searches against name + alt_names."""
    best = (None, 0.0, "")
    for idx, reg in enumerate(registry):
        candidates = [reg.get("name", "")] + list(reg.get("alt_names") or [])
        for cand in candidates:
            score, method = fuzzy_score(freem_rec["title"], cand)
            if score > best[1]:
                best = (idx, score, method)
    if best[0] is not None and best[1] >= threshold:
        return best  # type: ignore[return-value]
    return None


# Status precedence — never demote a row already in a higher tier.
_STATUS_RANK = {
    "HAVE_LOCAL":          5,
    "HAVE_ARCHIVE_BYTES":  4,
    "KNOWN_RECOVERABLE":   3,
    "NEEDS_HUNT":          2,
    "NO_OUTBOUND":         1,
    "":                    0,
    None:                  0,
}


def bump_status(current: str, new: str) -> str:
    return new if _STATUS_RANK.get(new, 0) > _STATUS_RANK.get(current, 0) else current


def fold_freem_into_row(reg: dict, freem: dict) -> bool:
    """Mutate `reg` to absorb the freem data. Returns True if anything
    actually changed (idempotent on re-run)."""
    changed = False

    if reg.get("freem_id") != freem["freem_id"]:
        reg["freem_id"] = freem["freem_id"]
        changed = True
    if freem.get("author") and not reg.get("developer"):
        reg["developer"] = freem["author"]
        changed = True

    sources = list(reg.get("sources") or [])
    if "freem" not in sources:
        sources.append("freem")
        reg["sources"] = sources
        changed = True

    # Drop any previous freem-crawler resources so a re-run picks up
    # version / size changes. Other source rows are preserved.
    existing = [r for r in (reg.get("resources") or [])
                if r.get("source") != "freem-crawler"]

    homepage = {
        "kind":   "homepage",
        "name":   "freem.ne.jp",
        "label":  "freem detail page",
        "url":    freem["detail_url"],
        "source": "freem-crawler",
    }
    download = {
        "kind":   "download",
        "name":   "freem download",
        "label":  (f"freem · {freem['version']}"
                   if freem.get("version") else "freem · current"),
        "url":    freem["download_url"],
        "source": "freem-crawler",
    }
    if freem.get("size_bytes"):
        download["size_bytes"] = freem["size_bytes"]
    if freem.get("version"):
        download["desc"] = (f"{freem['version']} ({freem['updated']})"
                            if freem.get("updated") else freem["version"])

    new_resources = existing + [homepage, download]
    if reg.get("resources") != new_resources:
        reg["resources"] = new_resources
        changed = True

    # Bump archive_status. Never demote.
    new_st = bump_status(reg.get("archive_status", ""), "KNOWN_RECOVERABLE")
    if new_st != reg.get("archive_status"):
        reg["archive_status"] = new_st
        changed = True

    return changed


def slugify(s: str, fallback: str) -> str:
    """ASCII slug for registry game_id. Same shape atwiki_merge uses
    (lowercase letters/digits/hyphens). CJK-only titles fall back to
    freem-<id> since the slug requires at least 4 letters."""
    s2 = (s or "").lower()
    s2 = re.sub(r"[^a-z0-9]+", "-", s2).strip("-")
    letters = sum(1 for c in s2 if c.isalpha())
    if letters < 4:
        return fallback
    return s2[:60].rstrip("-")


def import_unmatched(freem_records: list[dict], registry: list[dict]) -> int:
    """Add registry rows for freem games that didn't match anything.
    Returns number imported.

    Each new row keeps the freem download as a resource so the admin
    editor can spot the source. Editors can rename game_id later via
    the rename endpoint if the auto-slug is ugly."""
    existing_ids = {r.get("game_id") for r in registry}
    n_added = 0
    for f in freem_records:
        gid = slugify(f["title"], f"freem-{f['freem_id']}")
        # Suffix on collision (e.g. "shadow-arts-2" if "shadow-arts" exists).
        if gid in existing_ids:
            suffix = 2
            while f"{gid}-{suffix}" in existing_ids:
                suffix += 1
            gid = f"{gid}-{suffix}"
        existing_ids.add(gid)

        new_row = {
            "game_id":      gid,
            "name":         f["title"],
            "alt_names":    [],
            "engine":       f.get("engine") or "FM2K",
            "year":         f.get("updated", "")[:4] if f.get("updated") else "",
            "developer":    f.get("author", ""),
            "publisher":    "",
            "exe_stems":    [gid],
            "kgt_filename": "",
            "homepage":     f.get("detail_url", ""),
            "download_url": "",
            "banner_url":   f.get("screenshots", [""])[0] if f.get("screenshots") else "",
            "thumb_url":    "",
            "sources":      ["freem"],
            "characters":   [],
            "stages":       [],
            "_raw":         {},
            "freem_id":     f["freem_id"],
            "imported_from": "freem-crawler",
            "archive_status": "KNOWN_RECOVERABLE",
            "description":  f.get("description", "")[:500],
            "resources":    [],
            "versions":     [],
        }
        # Use the same fold function so resources land identically to
        # the matched path. fold_freem_into_row mutates in place.
        fold_freem_into_row(new_row, f)
        registry.append(new_row)
        n_added += 1
        print(f"  IMPORT  [{f['engine']:>4}] {f['freem_id']:>5}  -> {gid}  "
              f"by {f['author']:<20s} {f['title']}")
    return n_added


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--threshold", type=float, default=0.6,
                    help="min fuzzy score (0..1) for an automatic match")
    ap.add_argument("--dry-run", action="store_true",
                    help="print what would change, don't write registry.json")
    ap.add_argument("--no-import-unmatched", action="store_true",
                    help="don't auto-add unmatched freem games as new registry rows")
    args = ap.parse_args()

    if not FREEM_INDEX.exists():
        print(f"missing {FREEM_INDEX} — run freem_crawl.py first",
              file=sys.stderr)
        return 1
    if not REGISTRY.exists():
        print(f"missing {REGISTRY}", file=sys.stderr)
        return 1

    idx_data = json.loads(FREEM_INDEX.read_text(encoding="utf-8"))
    registry = json.loads(REGISTRY.read_text(encoding="utf-8"))

    # Only consider freem entries we're confident are FM games.
    fm_records = []
    for slim in idx_data["games"]:
        if slim.get("engine") not in ("FM2K", "FM95"):
            continue
        # Load the full detail record.
        p = FREEM_GAMES / f"{slim['freem_id']}.json"
        if p.exists():
            fm_records.append(json.loads(p.read_text(encoding="utf-8")))

    print(f"freem FM2K/FM95 records: {len(fm_records)}")
    print(f"registry size:           {len(registry)}\n")

    n_matched = 0
    n_changed = 0
    new_candidates: list[dict] = []
    for f in fm_records:
        m = best_match_for(f, registry, threshold=args.threshold)
        if m is None:
            new_candidates.append(f)
            print(f"  NEW    [{f['engine']:>4}] {f['freem_id']:>5}  "
                  f"by {f['author']:<20s} {f['title']}")
            continue
        idx, score, method = m
        target = registry[idx]
        if fold_freem_into_row(target, f):
            n_changed += 1
        n_matched += 1
        print(f"  match  [{f['engine']:>4}] {f['freem_id']:>5}  "
              f"({score:.2f}/{method:>10s}) -> {target['game_id']}")

    print(f"\nmatched (folded into existing):   {n_matched}")
    print(f"  rows changed by this run:       {n_changed}")
    print(f"new candidates (no registry row): {len(new_candidates)}")

    if not args.no_import_unmatched and new_candidates:
        print(f"\nimporting {len(new_candidates)} new games...")
        n_imported = import_unmatched(new_candidates, registry)
        print(f"  imported: {n_imported}")

    if args.dry_run:
        print("\n(dry-run — registry.json not written)")
        return 0

    REGISTRY.write_text(
        json.dumps(registry, ensure_ascii=False, indent=2),
        encoding="utf-8")
    print(f"\nwrote -> {REGISTRY.relative_to(REPO)}  ({len(registry)} games)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
