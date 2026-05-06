#!/usr/bin/env python3
"""Scrape mizuumi.wiki for FM2K-engine game pages.

Mizuumi is the canonical fighting-game wiki for everything obscure
enough to not warrant its own site. It has rich, curated metadata —
friendly names, dev info, character rosters, screenshots — that
beats anything we get out of IA / MyAbandonware. Downside: there's
no "FM2K games" category to enumerate from. Each game is its own
self-titled category, the wiki search API isn't enabled, and the
namespace is shared with games on every other engine (UNI, Akatsuki
Blitzkampf, MUGEN ports, Arcana Heart, etc.).

Strategy:
  1. Walk Special:AllPages via the MediaWiki API, paginated.
  2. Keep only mainspace ROOT pages (no "/" in title — those are
     the game landing pages; "/Frame Data" / "/Character X" / etc.
     are subpages).
  3. For each root, pull the wikitext and check for FM2K markers:
        - Literal "FM2K" / "2D Fighter Maker" / "2DFM" mention.
        - Link to en.wikipedia.org/wiki/Fighter_Maker#2D_Fighter_Maker
          (which Pokemon: Close Combat / Messiah End / similar use).
  4. For matches, extract: title (from page name), category-ish
     fields out of common templates, the lead image filename, and
     the wikipedia-engine link confirmation.
  5. Write tools/mizuumi_scrape.json.

Run:    python3 tools/scrape_mizuumi.py
        python3 tools/scrape_mizuumi.py --refresh   # ignore cache

Polite: 0.3s between API calls. ~500–800 pages on the wiki, so a
full run is 3–5 min.
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
from typing import Any


REPO_ROOT = Path(__file__).resolve().parent.parent
OUT_PATH = REPO_ROOT / "tools" / "mizuumi_scrape.json"
API_URL = "https://mizuumi.wiki/api.php"
SLEEP_BETWEEN = 0.3
USER_AGENT = ("fm2k-rollback-launcher / registry-builder "
              "(https://github.com/Armonte/wanwan)")

# Wikitext substrings that confirm a page is about an FM2K-engine
# game. Most pages use one or more of these. Case-insensitive substr
# match — false positives are vanishingly rare since these strings
# don't appear in non-FM2K games' wikitext (we checked).
FM2K_MARKERS = [
    r"\bFM2K\b",
    r"\b2D[ _]Fighter[ _]Maker\b",
    r"\b2DFM\b",
    r"Fighter_Maker#2D_Fighter_Maker",       # the canonical link
    r"Fighter_Maker_2nd",
    r"Fighter_Maker_95",
    r"FM95",
    r"格闘ツクール",                          # JP name
    # LilithPort is the FM2K-specific netplay tool. If the wiki page
    # mentions it as the netcode method we can confidently classify
    # the page as FM2K. Catches Pokemon: Close Combat, Pokemon:
    # PsyStrike, etc., whose wikitext doesn't name the engine itself
    # but lists "via LilithPort" in the infobox. Avoid using game-
    # specific abbreviations as engine markers — "BBBR" tripped the
    # filter on Big Bang Beat -Revolve- (Alice Soft, not FM2K).
    r"\bLilithPort\b",
    r"\boldmud0/LilithPort\b",
]
FM2K_MARKER_RE = re.compile("|".join(FM2K_MARKERS), re.IGNORECASE)


def http_get_json(url: str) -> dict[str, Any] | None:
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    try:
        with urllib.request.urlopen(req, timeout=15) as resp:
            return json.loads(resp.read().decode("utf-8", errors="replace"))
    except Exception as e:
        print(f"  http_get_json failed: {url[:120]} -> {e}",
              file=sys.stderr)
        return None


def list_root_pages() -> list[str]:
    """Walk Special:AllPages, return main-space pages without `/` in
    title (= the game landing pages). Pages like X/Frame_Data are
    subpages of game X; we fetch wikitext from the root only."""
    out: list[str] = []
    apcontinue: str | None = None
    while True:
        params = {
            "action":         "query",
            "list":           "allpages",
            "aplimit":        500,
            "apnamespace":    0,
            "apfilterredir":  "nonredirects",
            "format":         "json",
        }
        if apcontinue:
            params["apcontinue"] = apcontinue
        url = f"{API_URL}?{urllib.parse.urlencode(params)}"
        d = http_get_json(url)
        if not d:
            break
        pages = d.get("query", {}).get("allpages", []) or []
        for p in pages:
            t = p.get("title", "")
            if t and "/" not in t:
                out.append(t)
        cont = d.get("continue") or {}
        if "apcontinue" not in cont:
            break
        apcontinue = cont["apcontinue"]
        time.sleep(SLEEP_BETWEEN)
    return out


def fetch_page_meta(title: str) -> dict[str, Any] | None:
    """Pull wikitext + categories + images for a single page."""
    params = {
        "action":  "parse",
        "page":    title,
        "format":  "json",
        "prop":    "wikitext|categories|images",
    }
    url = f"{API_URL}?{urllib.parse.urlencode(params)}"
    d = http_get_json(url)
    if not d or "parse" not in d:
        return None
    return d["parse"]


def looks_like_fm2k(parse: dict[str, Any]) -> bool:
    wt = (parse.get("wikitext") or {}).get("*") or ""
    return bool(FM2K_MARKER_RE.search(wt))


def normalize(parse: dict[str, Any]) -> dict[str, Any]:
    title = parse.get("title") or ""
    wikitext = (parse.get("wikitext") or {}).get("*") or ""

    # Lead image. Two patterns to try in order:
    #   1. {{Infobox Game | image = <filename> }} — the canonical
    #      banner / cover slot. Most modern game pages use this.
    #   2. First `[[File:<name>.png]]` reference — older pages that
    #      pre-date the infobox template embed images via raw
    #      bracket syntax.
    # Whichever resolves first wins.
    lead_image = ""
    m = re.search(r"\|\s*image\s*=\s*([^|\n]+\.(?:png|jpg|jpeg|gif))",
                  wikitext, re.IGNORECASE)
    if m:
        lead_image = m.group(1).strip()
    if not lead_image:
        m = re.search(r"\[\[File:([^|\]]+\.(?:png|jpg|jpeg|gif))",
                      wikitext, re.IGNORECASE)
        if m:
            lead_image = m.group(1).strip()

    # Try to extract a year — most wikitext lead paragraphs include a
    # "released [date]" or just a 19xx/20xx number near the top.
    year = ""
    m = re.search(r"\b(19[89]\d|20[0-3]\d)\b", wikitext[:2000])
    if m:
        year = m.group(1)

    # Developer hint: the wiki commonly uses "by '''<name>'''" or
    # "developed by '''<name>'''" in the lead paragraph. Conservative
    # match — only the first wikitext-bolded name in the first 1000
    # chars after a "by" / "developed by" / "programmed by".
    developer = ""
    m = re.search(r"(?:by|developed by|programmed by)\s+'''([^']+)'''",
                  wikitext[:1500], re.IGNORECASE)
    if m:
        developer = m.group(1).strip()

    cats = [c.get("*", "") for c in (parse.get("categories") or [])]
    images = parse.get("images") or []

    return {
        "wiki_title":     title,
        "homepage":       f"https://mizuumi.wiki/w/"
                          f"{urllib.parse.quote(title.replace(' ', '_'))}",
        "name":           title,
        "year":           year,
        "developer":      developer,
        "lead_image":     lead_image,
        "categories":     cats,
        "images":         images,
        "wikitext_excerpt": wikitext[:500],
        "source":         "mizuumi.wiki",
    }


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--refresh", action="store_true")
    ap.add_argument("--limit", type=int, default=0)
    args = ap.parse_args()

    cached: dict[str, dict[str, Any]] = {}
    if not args.refresh and OUT_PATH.exists():
        try:
            cached = {r["wiki_title"]: r for r in
                      json.loads(OUT_PATH.read_text(encoding="utf-8"))}
            print(f"reusing {len(cached)} cached records")
        except Exception:
            cached = {}

    print("listing all root pages…")
    titles = list_root_pages()
    print(f"  {len(titles)} root pages on the wiki")

    out: list[dict[str, Any]] = []
    fetched = 0
    for title in titles:
        if title in cached:
            out.append(cached[title])
            continue
        if args.limit and fetched >= args.limit:
            break
        print(f"fetch: {title}")
        parse = fetch_page_meta(title)
        fetched += 1
        time.sleep(SLEEP_BETWEEN)
        if not parse:
            continue
        if not looks_like_fm2k(parse):
            continue
        rec = normalize(parse)
        out.append(rec)
        print(f"  ✓ FM2K — {len(out)} confirmed so far")

    OUT_PATH.write_text(json.dumps(out, indent=2, ensure_ascii=False),
                        encoding="utf-8")
    print(f"\nwrote {len(out)} FM2K records to "
          f"{OUT_PATH.relative_to(REPO_ROOT)} "
          f"({fetched} pages fetched this run)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
