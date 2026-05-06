#!/usr/bin/env python3
"""Scrape Internet Archive for FM2K / 2D Fighter Maker games.

Wak floated the idea of feeding us a curated archive.org list and never
followed up, so we run our own search. Strategy:

  1. Hit archive.org's advancedsearch JSON API with several queries
     covering different ways FM2K games get titled / tagged on IA.
  2. For each unique identifier returned, fetch /metadata/<id> for the
     full file listing.
  3. Keep only items that look like real FM2K games: at least one
     `.kgt` (FM2K compiled gamesystem) or `.player` (character) file.
     Filters out random anime DVDs that match "fighter maker" by
     keyword.
  4. Write tools/ia_scrape.json — one record per identifier with the
     fields build_registry.py needs.

Run:    python3 tools/scrape_archive_org.py
Deps:   stdlib only (urllib + json)
Notes:  Polite — sleeps 250ms between metadata fetches. ~150 games
        means ~40s wall time. Re-runs are idempotent (overwrite the
        same output file). Pass --refresh to ignore the cached
        ia_scrape.json and re-fetch from scratch.
"""

from __future__ import annotations

import argparse
import json
import sys
import time
import urllib.parse
import urllib.request
from pathlib import Path
from typing import Any


REPO_ROOT = Path(__file__).resolve().parent.parent
OUT_PATH = REPO_ROOT / "tools" / "ia_scrape.json"

# Queries to broaden coverage. IA's full-text index is fuzzy, so a
# game tagged only "fighter maker" without "2D" still surfaces under
# the broader query, while a romhack tagged "FM2K" but missing the
# words "fighter maker" surfaces only under the engine-name query.
# Union the result sets and dedup by identifier.
QUERIES = [
    # Tightly-scoped phrases that don't false-match news / radio /
    # photo-archive items. Dropping the bare "fighter maker" query
    # entirely — it pulled in hundreds of unrelated AlJazeera /
    # automated-uploader items.
    '"2D Fighter Maker"',
    '"2D Fighter Maker 2nd"',
    '"2D Fighter Maker 95"',
    '"FM2K"',
    '"FM95"',
    '"2dfm"',
    '"2dfm95"',
    '"2dk2nd"',
    '"fighter maker 2nd"',
    '"fighter maker 95"',
    '"fighter maker 2002"',
    '"2D fighting"',            # broad — IA tokenizer matches games
                                # tagged "2D fighting" without other
                                # FM2K markers
    # Japanese variants
    '"2D 格闘ツクール"',
    '"格闘ツクール"',           # any "fighting-game maker"
    '"2D格闘ツクール"',         # no-space variant
    '"格ツク"',                 # short fan-fic abbreviation
    # Engine internals — high-precision FM2K-only matches
    'KGT2KGAME',                # FM2K window class
    'KGT95GAME',                # FM95 window class
    # Subject-tag queries. IA's `subject:` field is taxonomy / curator-
    # set; doujin-fighting-game uploads typically get tagged with one
    # or more of these. Catches games whose freetext doesn't include
    # "FM2K" / "fighter maker" but whose curator labeled them right.
    'subject:"fighting game maker"',
    'subject:"2D Fighter Maker"',
    'subject:"2D Fighter Maker 2nd"',
    'subject:"2D Fighter Maker 95"',
    'subject:"格闘ツクール"',
    'subject:"FM2K"',
    'subject:"doujin fighting"',
    'subject:"anime doujin fighter"',
]

# Identifier patterns that look obviously NOT-a-game-upload — we
# skip them before paying the metadata-fetch round-trip. Mostly
# automated photo / news uploaders that collide with "fighter
# maker" via OCR'd captions or "2dfm" via random tokens.
import re as _re
SKIP_IDENT_PATTERNS = [
    _re.compile(r"^A63_\d{14}", _re.IGNORECASE),          # automated photo uploader
    _re.compile(r"^ALJAZ", _re.IGNORECASE),               # Al Jazeera America transcripts
    _re.compile(r"^[a-z0-9]{40,}$", _re.IGNORECASE),      # 40+ char hashes (random uploads)
    _re.compile(r"^MSNBCW_\d{8}", _re.IGNORECASE),        # MSNBC West archive
    _re.compile(r"^CSPAN[A-Z]?_\d", _re.IGNORECASE),      # C-SPAN archive
    _re.compile(r"^FOXNEWSW_\d", _re.IGNORECASE),
    _re.compile(r"^CNNW_\d", _re.IGNORECASE),
]


def is_obvious_non_game(ident: str) -> bool:
    return any(p.search(ident) for p in SKIP_IDENT_PATTERNS)

# IA advancedsearch endpoint. We page in 50s so a single broad query
# (which can return hundreds of indie items) doesn't ask the server
# for 2000 rows at once.
SEARCH_URL = "https://archive.org/advancedsearch.php"
METADATA_URL = "https://archive.org/metadata/{}"
PAGE_SIZE = 50

# File-extension fingerprints. Two tiers:
#   STRONG_HINTS — guaranteed FM2K asset; instant accept.
#   ARCHIVE_EXTS — the upload is a packaged zip/rar/7z. IA doesn't
#                  auto-expand those in the file list, so we accept
#                  if the keyword search already matched and the
#                  item ships at least one archive (the typical
#                  shape of a playable upload).
# Strict mode (--strict) requires STRONG_HINTS. Default mode (loose)
# accepts archives + keyword match — broader coverage with the
# tradeoff that some engine-tool uploads slip through and need
# manual pruning via games/registry_overrides.json.
STRONG_HINTS = (".kgt", ".player", "kgt2kgame", "kgt95game")
ARCHIVE_EXTS = (".zip", ".7z", ".rar", ".tar.gz", ".tgz")

# Per-request pause to avoid hammering the IA backend. They publish
# rate guidelines around "be reasonable, don't open 100 concurrent
# connections"; sequential + 250ms is well within that.
SLEEP_BETWEEN = 0.25


def http_get_json(url: str) -> dict[str, Any] | None:
    """Single-shot GET → parsed JSON, or None on any error.

    Doesn't retry — IA's API is stable; if it transient-fails we'd
    rather log + skip and let the caller iterate again later than
    silently retry and eat the rate budget.
    """
    req = urllib.request.Request(url, headers={
        "User-Agent": "fm2k-rollback-launcher / registry-builder "
                      "(https://github.com/Armonte/wanwan)",
    })
    try:
        with urllib.request.urlopen(req, timeout=15) as resp:
            return json.loads(resp.read().decode("utf-8", errors="replace"))
    except Exception as e:
        print(f"  http_get_json failed: {url} -> {e}", file=sys.stderr)
        return None


def search(query: str) -> list[str]:
    """Return all IA identifiers matching `query`. Pages internally."""
    out: list[str] = []
    page = 1
    while True:
        params = {
            "q": query,
            "fl[]": ["identifier"],
            "rows": PAGE_SIZE,
            "page": page,
            "output": "json",
        }
        url = f"{SEARCH_URL}?{urllib.parse.urlencode(params, doseq=True)}"
        data = http_get_json(url)
        if not data or "response" not in data:
            break
        docs = data["response"].get("docs") or []
        if not docs:
            break
        out.extend(d["identifier"] for d in docs if "identifier" in d)
        if len(docs) < PAGE_SIZE:
            break
        page += 1
        time.sleep(SLEEP_BETWEEN)
    return out


def fetch_metadata(identifier: str) -> dict[str, Any] | None:
    """Pull /metadata/<id> for the full record. Returns None on error."""
    return http_get_json(METADATA_URL.format(urllib.parse.quote(identifier)))


def looks_like_fm2k(meta: dict[str, Any], strict: bool) -> tuple[bool, bool]:
    """File-list sniff. Returns (accept, verified).

    `verified` is True when STRONG_HINTS matched directly (.kgt /
    .player / engine-class string), False when we accepted via the
    archive-only path. The merger forwards `verified` so registry
    consumers / manual reviewers can sort.
    """
    files = meta.get("files") or []
    has_strong = False
    has_archive = False
    for f in files:
        name = (f.get("name") or "").lower()
        for hint in STRONG_HINTS:
            if hint in name:
                has_strong = True
                break
        for ext in ARCHIVE_EXTS:
            if name.endswith(ext):
                has_archive = True
    if has_strong:
        return True, True
    if strict:
        return False, False
    return has_archive, False


def normalize(identifier: str, meta: dict[str, Any]) -> dict[str, Any]:
    """Pull the registry-shaped fields out of an IA metadata record.

    IA's `metadata` block has many optional / list-or-string fields;
    we coerce to scalar where the registry expects scalar. Anything
    missing stays empty so the merge step has predictable shapes to
    diff against MyAbandonware records.
    """
    md = meta.get("metadata") or {}

    def scalar(key: str) -> str:
        v = md.get(key)
        if isinstance(v, list):
            return v[0] if v else ""
        return v or ""

    title = scalar("title") or identifier
    creator = scalar("creator")
    year = scalar("year") or scalar("date")[:4]
    files = meta.get("files") or []
    file_names = [f.get("name", "") for f in files if isinstance(f, dict)]

    return {
        "ia_identifier": identifier,
        "title": title,
        "creator": creator,
        "year": year,
        "homepage": f"https://archive.org/details/{identifier}",
        "download": f"https://archive.org/download/{identifier}",
        "files": file_names,
        "subjects": md.get("subject") if isinstance(md.get("subject"), list)
                    else [md["subject"]] if md.get("subject") else [],
        "description": scalar("description"),
        "source": "archive.org",
    }


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--refresh", action="store_true",
                    help="ignore any cached ia_scrape.json and re-search")
    ap.add_argument("--limit", type=int, default=0,
                    help="stop after fetching this many items (debug)")
    ap.add_argument("--strict", action="store_true",
                    help="require .kgt/.player in file list "
                         "(skips archive-bundled uploads)")
    args = ap.parse_args()

    # Step 1: union of all search results.
    all_ids: set[str] = set()
    for q in QUERIES:
        print(f"search: {q}")
        ids = search(q)
        print(f"  +{len(ids)} hits")
        all_ids.update(ids)
        time.sleep(SLEEP_BETWEEN)
    print(f"unique identifiers across all queries: {len(all_ids)}")

    # Optional cache reuse: skip metadata fetch for ids already in the
    # output file unless --refresh.
    cached: dict[str, dict[str, Any]] = {}
    if not args.refresh and OUT_PATH.exists():
        try:
            cached = {r["ia_identifier"]: r for r in
                      json.loads(OUT_PATH.read_text())}
            print(f"reusing {len(cached)} cached records "
                  f"(pass --refresh to re-fetch)")
        except Exception:
            cached = {}

    # Step 2: pull metadata + filter.
    out: list[dict[str, Any]] = []
    fetched = 0
    skipped_pattern = 0
    for ident in sorted(all_ids):
        if ident in cached:
            out.append(cached[ident])
            continue
        if is_obvious_non_game(ident):
            skipped_pattern += 1
            continue
        if args.limit and fetched >= args.limit:
            break
        print(f"fetch: {ident}")
        meta = fetch_metadata(ident)
        fetched += 1
        time.sleep(SLEEP_BETWEEN)
        if not meta:
            continue
        accept, verified = looks_like_fm2k(meta, args.strict)
        if not accept:
            print(f"  skip (no FM2K assets, no archive)")
            continue
        rec = normalize(ident, meta)
        rec["verified"] = verified  # True=strong .kgt/.player hit
        out.append(rec)

    # Step 3: write.
    OUT_PATH.parent.mkdir(parents=True, exist_ok=True)
    OUT_PATH.write_text(json.dumps(out, indent=2, ensure_ascii=False))
    print(f"wrote {len(out)} records to {OUT_PATH.relative_to(REPO_ROOT)} "
          f"({skipped_pattern} pre-filtered by ident pattern, "
          f"{fetched} fetched from API)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
