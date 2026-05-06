#!/usr/bin/env python3
"""Look INSIDE archive.org-hosted zips to find .kgt + .player files.

IA's /metadata/<id> API returns the OUTER file list — the zip is one
entry, its contents are not. But when a zip has been server-side
indexed (most uploads ≥ a few months old) IA serves a directory
listing of the zip's internals at:

    https://archive.org/download/<id>/<zipname>.zip/

Plain HTML, parseable with the stdlib html parser.

For each record in tools/ia_scrape.json this script:
  1. Fingerprints the *outermost* archive (.zip / .7z / .rar) in the
     record's `files` list.
  2. Fetches its directory-listing HTML.
  3. Scrapes <a href> filenames out and pushes them into a new
     `inner_files` field on the record.
  4. If `inner_files` includes any *.kgt, sets:
       - record["verified"] = True
       - record["kgt_filename"] = "<basename>.kgt"
       - record["exe_stem"] = stem of the kgt (FM2K convention is
                              <stem>.kgt + <stem>.exe sharing a stem)
  5. Auto-tags engine-only uploads (title or contents look like the
     bare engine without a game) by setting `engine_bundle: true`.
     The merger drops these from the registry.

Run:    python3 tools/inspect_ia_zips.py
        python3 tools/inspect_ia_zips.py --refresh   # ignore cache

Output: rewrites tools/ia_scrape.json in place, adding inner_files /
verified / kgt_filename / exe_stem / engine_bundle fields.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
import time
import urllib.parse
import urllib.request
from html.parser import HTMLParser
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parent.parent
SCRAPE_PATH = REPO_ROOT / "tools" / "ia_scrape.json"

DOWNLOAD_BASE = "https://archive.org/download/{ident}/{name}/"
ARCHIVE_EXTS = (".zip", ".7z", ".rar")
SLEEP_BETWEEN = 0.3
USER_AGENT = ("fm2k-rollback-launcher / registry-builder "
              "(https://github.com/Armonte/wanwan)")

# Title patterns that scream "this is the FM2K engine itself, not a
# specific game." Drop these from the registry — they'd never have a
# game_id worth surfacing in the lobby. Conservative — only matches
# when the title is _dominated_ by these tokens, not just contains
# them. (Real games like "Pokemon Close Combat" might mention
# "FM2K" in description but won't title themselves that.)
ENGINE_BUNDLE_TITLE_PATTERNS = [
    re.compile(r"^\s*2[\s-]*d?\s*fighter\s*maker(\s*\d*)?\s*$",   re.I),
    re.compile(r"engine\s*bundle",                                re.I),
    re.compile(r"^\s*fm2k\s*(engine|editor)?\s*$",                re.I),
    re.compile(r"^\s*2dfm(95)?\s*(engine|editor)?\s*$",           re.I),
    re.compile(r"^\s*fighter\s*maker\s*(2nd|95|2002|2016)?\s*$",  re.I),
    re.compile(r"格闘ツクール(\s*\d*)?\s*$",                       re.I),
    # Common engine-only filename patterns inside the zip
]
ENGINE_BUNDLE_FILE_PATTERNS = [
    re.compile(r"^2dkfm(95)?\.exe$", re.I),     # editor exe
    re.compile(r"^2dfm(95)?\.exe$",  re.I),     # ditto
]


def list_zip_contents_via_regex(html: str, zip_name: str) -> list[str]:
    """Pull inner filenames out of an IA zip-contents directory listing.

    IA renders zip contents as <a href="//archive.org/download/<id>/
    <zipname>.zip/<inner_path>"> entries — absolute URLs, %-encoded.
    We don't try to parse the HTML structure (<tr> / <td> shape varies
    by IA template version); just extract the path that follows the
    zip name and decode it.
    """
    # Match the slice of the href URL that lives inside the zip.
    # Example: "WonderfulWorld_ver_0945.zip/WonderfulWorld_ver_0945%2Fboss.player"
    # We want everything after the .zip/ delimiter.
    quoted = re.escape(zip_name)
    pat = re.compile(rf'{quoted}/([^"\s<>]+)', re.IGNORECASE)
    raw = pat.findall(html)
    out: set[str] = set()
    for r in raw:
        # Decode %2F -> / and any other %xx encodings.
        try:
            decoded = urllib.parse.unquote(r)
        except Exception:
            decoded = r
        # Drop the View-Contents toplink (just the zip itself) and
        # any IA query strings.
        if not decoded or decoded.endswith("/"):
            continue
        if decoded.startswith("?"):
            continue
        out.add(decoded)
    return sorted(out)


def http_get_text(url: str) -> str | None:
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    try:
        with urllib.request.urlopen(req, timeout=15) as resp:
            return resp.read().decode("utf-8", errors="replace")
    except Exception as e:
        print(f"  http_get_text failed: {url} -> {e}", file=sys.stderr)
        return None


def list_zip_contents(ident: str, archive_filename: str) -> list[str]:
    """Fetch IA's directory-listing HTML for a server-indexed archive
    and return the inner filenames. Empty list on failure / not-yet-
    indexed zips."""
    url = DOWNLOAD_BASE.format(
        ident=urllib.parse.quote(ident),
        name=urllib.parse.quote(archive_filename),
    )
    html = http_get_text(url)
    if not html:
        return []
    return list_zip_contents_via_regex(html, archive_filename)


def first_archive(files: list[str]) -> str | None:
    """Pick the most-game-shaped archive from the outer file list.
    Prefers .zip > .7z > .rar; tie-broken by name length (shorter ==
    "the main download" rather than a derivative)."""
    cands: list[tuple[int, int, str]] = []
    for f in files:
        low = f.lower()
        for rank, ext in enumerate(ARCHIVE_EXTS):
            if low.endswith(ext):
                cands.append((rank, len(f), f))
                break
    if not cands:
        return None
    cands.sort()
    return cands[0][2]


def looks_like_engine_bundle(rec: dict, inner: list[str]) -> bool:
    title = (rec.get("title") or "").strip()
    for p in ENGINE_BUNDLE_TITLE_PATTERNS:
        if p.search(title):
            return True
    # Inner-file fingerprint: editor exe present and NO game-content
    # files (no .kgt + no .player + no .stage).
    has_editor_exe = any(any(p.search(f) for p in ENGINE_BUNDLE_FILE_PATTERNS)
                        for f in inner)
    has_content = any(f.lower().endswith((".kgt", ".player", ".stage"))
                      for f in inner)
    return has_editor_exe and not has_content


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--refresh", action="store_true",
                    help="re-fetch zip listings even when cached")
    ap.add_argument("--limit", type=int, default=0,
                    help="stop after N records (debug)")
    args = ap.parse_args()

    if not SCRAPE_PATH.exists():
        print(f"error: {SCRAPE_PATH.relative_to(REPO_ROOT)} not present "
              f"(run scrape_archive_org.py first)", file=sys.stderr)
        return 1

    records = json.loads(SCRAPE_PATH.read_text(encoding="utf-8"))
    n = len(records)
    print(f"inspecting {n} IA records for inner zip contents")

    fetched = 0
    upgraded = 0
    flagged_bundles = 0
    for i, rec in enumerate(records):
        if args.limit and fetched >= args.limit:
            break
        ident = rec.get("ia_identifier")
        files = rec.get("files") or []
        archive = first_archive(files)
        if not archive:
            continue
        # Cache: if already inspected and not --refresh, skip.
        if "inner_files" in rec and not args.refresh:
            inner = rec["inner_files"]
        else:
            print(f"[{i+1}/{n}] {ident} -> {archive}")
            inner = list_zip_contents(ident, archive)
            rec["inner_files"] = inner
            time.sleep(SLEEP_BETWEEN)
            fetched += 1
        # Verification + game_id derivation
        kgt = next((Path(f).stem for f in inner
                    if f.lower().endswith(".kgt")), "")
        if kgt:
            rec["verified"] = True
            rec["kgt_filename"] = kgt + ".kgt"
            # Most FM2K games ship the .exe and .kgt under the same stem.
            # We'll pick the lowercased kgt stem as the canonical exe stem
            # and let registry_overrides.json correct outliers.
            rec["exe_stem"] = kgt.lower()
            upgraded += 1
        # Engine-bundle flag
        if looks_like_engine_bundle(rec, inner):
            rec["engine_bundle"] = True
            flagged_bundles += 1

    SCRAPE_PATH.write_text(json.dumps(records, indent=2, ensure_ascii=False))
    print(f"\nupgraded {upgraded} records with verified .kgt match")
    print(f"flagged {flagged_bundles} as engine bundles")
    print(f"wrote {SCRAPE_PATH.relative_to(REPO_ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
