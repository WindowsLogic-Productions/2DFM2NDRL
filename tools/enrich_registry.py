#!/usr/bin/env python3
"""Second-pass enricher for tools/ia_scrape.json.

Run AFTER inspect_ia_zips.py. Improvements over the first pass:

  - Broader engine / tools / docs filter. Catches:
      * "2D Fighter Maker 2016" (engine, not a game)
      * "fm2k-re-translation"   (translation patch)
      * "2D Fighter Maker Fixes" (community fixpack)
      * "2D Fighter Maker Documentation" (wiki dump)
      * "Manual" / "Bundle for Windows" / etc.
  - Identifier→game_id heuristic for records where IA hasn't indexed
    the zip yet. Strips date suffixes, version suffixes, kebab-case
    glue, and bundles trailing "-7z" / "-rar". `axel-city-complete`
    keeps the slug as-is, but `8-marbles_202212` becomes `8-marbles`
    and `vanguard.-princess.v-1.8.7` becomes `vanguard-princess`.
  - Retries IA zip-listing fetch for records where inner_files is
    empty AND the outer file list contains an archive — IA may have
    finished post-upload processing since the first pass. Capped via
    --retry-limit to avoid pounding the API.
  - Mirrors the same .7z / .rar URL pattern through IA's directory-
    listing endpoint. IA serves equivalent listings for those formats
    on items where it's been able to extract them.

Run:    python3 tools/enrich_registry.py
        python3 tools/enrich_registry.py --retry-limit 30
"""

from __future__ import annotations

import argparse
import json
import re
import sys
import time
import urllib.parse
import urllib.request
from pathlib import Path

# Reuse the listing parser from inspect_ia_zips so we have one source
# of truth for IA's URL shape and HTML decoding.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from inspect_ia_zips import list_zip_contents, first_archive  # noqa: E402


REPO_ROOT = Path(__file__).resolve().parent.parent
SCRAPE_PATH = REPO_ROOT / "tools" / "ia_scrape.json"


# Title patterns that mean "this isn't a playable game upload."
# Conservative — anchors at start/end where possible so a real game
# named e.g. "Engine of Destruction" doesn't get false-flagged.
NON_GAME_TITLE_PATTERNS = [
    # Engine itself
    re.compile(r"^\s*2[\s-]*d?\s*fighter\s*maker[\s\d()]*$",       re.I),
    re.compile(r"^\s*fighter\s*maker(\s*\d{2,4})?\s*$",            re.I),
    re.compile(r"^\s*fm2k\s*(engine|editor)?\s*$",                 re.I),
    re.compile(r"^\s*2dfm(95)?\s*(engine|editor)?\s*$",            re.I),
    re.compile(r"^\s*2[\s-]?d?\s*格闘ツクール[\s\d]*$",              re.I),
    re.compile(r"engine\s*bundle",                                 re.I),
    re.compile(r"\bbundle\s+for\s+Windows\b",                      re.I),
    # Other engines that subject-tag queries pull in
    re.compile(r"\bMUGEN(ation)?\b",                               re.I),
    re.compile(r"\bIKEMEN\b",                                      re.I),
    re.compile(r"\bFighter\s+Factory\b",                           re.I),
    # Commercial PC ports / arcade collections
    re.compile(r"\(GOG\.com\)",                                    re.I),
    re.compile(r"\bClayFighter\b",                                 re.I),
    re.compile(r"\bGuilty\s*Gear\b",                               re.I),
    re.compile(r"\bUN[I_]B\b",                                     re.I),  # Under Night In-Birth
    re.compile(r"\bCyberbots\b",                                   re.I),
    re.compile(r"\bGroove\s+on\s+Fight\b",                         re.I),
    re.compile(r"Far\s+East\s+of\s+Eden",                          re.I),
    re.compile(r"\bDragon\s+Ball\b.*\b(arcade|rev)\b",              re.I),
    re.compile(r"\bVirtua\s+Fighter\b",                            re.I),
    # Talks / tutorials / collections
    re.compile(r"\bGDC\s*\d{4}",                                    re.I),
    re.compile(r"\bprimer\b",                                       re.I),
    re.compile(r"PC-98\s+Collection",                              re.I),
    re.compile(r"\bcollection\W*archive\b",                        re.I),
    # Patches / translations / fixpacks
    re.compile(r"\bre\W*translation\b",                            re.I),
    re.compile(r"\btranslation\s*(patch|pack)?\b",                 re.I),
    re.compile(r"\bfix(es|pack)?\b",                               re.I),
    re.compile(r"\bpatch\b",                                       re.I),
    # Docs / manuals / wikis
    re.compile(r"\bdocumentation\b",                               re.I),
    re.compile(r"\bmanual\b",                                      re.I),
    re.compile(r"\bwiki\b",                                        re.I),
]

# Identifier patterns that obviously aren't games. Mirrors / dumps /
# scrapes that slipped past the first scrape filter.
NON_GAME_IDENT_PATTERNS = [
    re.compile(r"^miraheze-",   re.I),     # wiki dumps
    re.compile(r"^.*-wiki-",    re.I),
    re.compile(r"-translation", re.I),
    re.compile(r"-fixes",       re.I),
    re.compile(r"^arcade_",     re.I),     # MAME-style arcade ROMs
    re.compile(r"^pc-98",       re.I),     # PC-98 collections
    re.compile(r"^GDC\d{4}",    re.I),     # GDC talk videos
    re.compile(r"-mugen",       re.I),
    re.compile(r"^ikemen",      re.I),
    re.compile(r"^ffactory",    re.I),     # Fighter Factory (MUGEN tool)
    re.compile(r"\bsnmugen\b",  re.I),     # *snmugen* fangame uploads
]


def looks_like_non_game(rec: dict) -> bool:
    """Aggressive non-game classifier. Anything matching becomes
    engine_bundle:true so build_registry.py drops it."""
    title = (rec.get("title") or "").strip()
    ident = rec.get("ia_identifier", "")
    for p in NON_GAME_TITLE_PATTERNS:
        if p.search(title):
            return True
    for p in NON_GAME_IDENT_PATTERNS:
        if p.search(ident):
            return True
    # Hard signal: IA indexed enough inner files to be confident, but
    # NONE end in .kgt. Real FM2K games always include at least one
    # .kgt (the compiled gamesystem). 5+ inner files without one is
    # almost certainly a different engine — MUGEN ships .def, Ikemen
    # ships .ssz, MAME arcade ROMs ship .rom/.bin, GDC talks ship
    # .mp4. Conservative threshold so we don't drop a small upload
    # that's mid-indexing (those typically show 0–3 files).
    inner = rec.get("inner_files") or []
    if len(inner) >= 5 and not any(f.lower().endswith(".kgt")
                                   for f in inner):
        return True
    return False


# Identifier-cleanup heuristics. Run in order, each fed the output
# of the previous. Intent: turn IA upload-style identifiers
# (lower-kebab + occasional date/version trailers) into the
# launcher-canonical short stem.
IDENT_NORMALIZE_RULES: list[tuple[re.Pattern, str]] = [
    # Trailing date stamp like "_20260501" or "-202212"
    (re.compile(r"[-_]?\d{6,8}$"),                ""),
    # Trailing version stamp like ".v-1.8.7", "-v1-2", "-v2"
    (re.compile(r"[-_.]+v[-_.]?\d+([-_.]\d+)*$"), ""),
    # Trailing format hint like "-7z", "-rar"
    (re.compile(r"[-_]?(7z|rar|zip)$"),           ""),
    # Trailing variant tag like "-complete", "-final", "-jp"
    (re.compile(r"[-_]?(complete|final|jp|en)$"), ""),
    # Extra dots ("vanguard.-princess.v-1.8.7" etc) -> hyphens
    (re.compile(r"\.+"),                          "-"),
    # Collapse runs of separators
    (re.compile(r"[-_]+"),                        "-"),
    # Trim leading/trailing separators
    (re.compile(r"^-|-$"),                        ""),
]


def normalize_ident_to_stem(ident: str) -> str:
    s = ident.lower()
    for pat, repl in IDENT_NORMALIZE_RULES:
        s = pat.sub(repl, s)
    return s


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--retry-limit", type=int, default=20,
                    help="max records to re-fetch inner_files for")
    args = ap.parse_args()

    if not SCRAPE_PATH.exists():
        print(f"error: {SCRAPE_PATH.relative_to(REPO_ROOT)} missing",
              file=sys.stderr)
        return 1

    records = json.loads(SCRAPE_PATH.read_text(encoding="utf-8"))

    flagged_non_game = 0
    cleaned_idents = 0
    retried = 0
    new_kgt_finds = 0

    for rec in records:
        if rec.get("engine_bundle"):
            continue
        if looks_like_non_game(rec):
            rec["engine_bundle"] = True   # reuse the same drop flag
            rec["non_game_reason"] = "title/ident pattern"
            flagged_non_game += 1
            continue

        # Normalize stem from identifier as a fallback when we have no
        # zip-internal kgt to derive from.
        ident = rec.get("ia_identifier", "")
        stem = normalize_ident_to_stem(ident)
        if stem and stem != rec.get("exe_stem"):
            # Don't clobber a verified-from-zip exe_stem; only fill
            # when nothing else is set.
            if not rec.get("exe_stem"):
                rec["exe_stem_from_ident"] = stem
                cleaned_idents += 1

    # Retry zip-listing fetch for records that came up empty last time
    # AND haven't been flagged as non-game. IA's auto-indexing runs in
    # batches — uploads from after our first pass may have inner files
    # available now.
    for rec in records:
        if rec.get("engine_bundle"):
            continue
        if rec.get("inner_files"):
            continue
        if retried >= args.retry_limit:
            break
        files = rec.get("files") or []
        archive = first_archive(files)
        if not archive:
            continue
        ident = rec.get("ia_identifier", "")
        print(f"retry inner_files for {ident}/{archive}")
        inner = list_zip_contents(ident, archive)
        retried += 1
        time.sleep(0.3)
        if inner:
            rec["inner_files"] = inner
            kgt = next((Path(f).name for f in inner
                        if f.lower().endswith(".kgt")), "")
            if kgt:
                rec["verified"] = True
                rec["kgt_filename"] = kgt
                rec["exe_stem"] = Path(kgt).stem.lower()
                new_kgt_finds += 1

    SCRAPE_PATH.write_text(
        json.dumps(records, indent=2, ensure_ascii=False),
        encoding="utf-8")

    print()
    print(f"newly flagged as non-game:   {flagged_non_game}")
    print(f"identifier→stem fallbacks:   {cleaned_idents}")
    print(f"inner_files retries:         {retried}  "
          f"(of which yielded a .kgt: {new_kgt_finds})")
    print(f"wrote {SCRAPE_PATH.relative_to(REPO_ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
