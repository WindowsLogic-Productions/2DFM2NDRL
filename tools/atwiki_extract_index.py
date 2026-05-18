#!/usr/bin/env python3
"""Extract atwiki FM2K/FM95 game indexes into a unified JSON.

Inputs are local copies of the two index pages from
https://w.atwiki.jp/arunau32167/ (fetched via Wayback to dodge
Cloudflare). Defaults expect the snapshots already on disk in /tmp.

Output: data/fm2k_wiki/index.json — one row per game with the atwiki
page id, the JP title text from the index, and the engine flag derived
from which index the entry came from (740 -> FM2K, 743 -> FM95).
"""

import argparse
import json
import re
import sys
from pathlib import Path


REPO = Path(__file__).resolve().parents[1]
DEFAULT_OUT = REPO / "data" / "fm2k_wiki" / "index.json"

# Source index pages on atwiki and what engine they enumerate.
INDEXES = (
    # (atwiki_id, engine, default_local_html_for_offline_run)
    ("740", "FM2K", "/tmp/wb_740.html"),
    ("743", "FM95", "/tmp/wb_743.html"),
)

PAGE_LINK_RE = re.compile(
    r'<a[^>]+href="(?P<href>[^"]*?/arunau32167/pages/(?P<id>\d+)\.html)"[^>]*>(?P<title>[^<]+)</a>',
    re.IGNORECASE,
)


def slice_content(html: str) -> str:
    """Restrict to the page's main content area, excluding the shared
    sidebar (`<div class="menu">`) whose link set is identical across
    both index pages."""
    # `<div id="contents">` opens the article body; `<div id="atwiki-
    # page-tags">` closes it. Fall back to whole-page if markers missing.
    start = html.find('<div id="contents"')
    end = html.find('<div id="atwiki-page-tags"', start if start != -1 else 0)
    if start == -1 or end == -1:
        return html
    return html[start:end]


def parse_index(html: str, engine: str) -> list[dict]:
    """Walk an atwiki index page and return one record per linked game."""
    body = slice_content(html)
    rows: list[dict] = []
    seen: set[str] = set()
    for m in PAGE_LINK_RE.finditer(body):
        page_id = m.group("id")
        if page_id in {"740", "743"} or page_id in seen:
            continue
        seen.add(page_id)
        rows.append({
            "atwiki_id": page_id,
            "engine": engine,
            "title": m.group("title").strip(),
        })
    return rows


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "--out",
        type=Path,
        default=DEFAULT_OUT,
        help=f"output JSON path (default: {DEFAULT_OUT.relative_to(REPO)})",
    )
    args = ap.parse_args()

    all_rows: list[dict] = []
    for atwiki_id, engine, local_path in INDEXES:
        p = Path(local_path)
        if not p.exists():
            print(f"missing index snapshot: {p}", file=sys.stderr)
            return 1
        html = p.read_text(encoding="utf-8")
        rows = parse_index(html, engine)
        print(f"  {engine} index ({atwiki_id}): {len(rows)} games")
        all_rows.extend(rows)

    args.out.parent.mkdir(parents=True, exist_ok=True)
    with args.out.open("w", encoding="utf-8") as f:
        json.dump({"games": all_rows}, f, ensure_ascii=False, indent=2)
    print(f"wrote {len(all_rows)} entries -> {args.out.relative_to(REPO)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
