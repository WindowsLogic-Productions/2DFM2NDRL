#!/usr/bin/env python3
"""Score per-game records by 'how likely are we to land a real ZIP'
and emit the top-N atwiki IDs.

Heuristics (positive points):
  +5  vector.co.jp link            — direct download page, mostly still live
  +4  ux.getuploader link          — direct file host
  +4  freem.ne.jp / dlsite link    — software distribution sites
  +3  *.zip / *.lzh / *.exe in body_text — author already linked the file
  +2  fc2 / freett / geocities .jp — homepage-hop, frequently archived
  +1  any non-empty outbound        — at least we have a starting URL
  +2  outbound has Wayback archive resolved

Negative:
  -3  no outbound at all
  -2  outbound is dead-ish (mediafire / megaupload / etc.) since
      file hosts that died don't archive their bytes well

Usage:
    python3 tools/atwiki_select_easy.py --top 30
"""

import argparse
import json
import re
import sys
from pathlib import Path


REPO = Path(__file__).resolve().parents[1]
GAMES_DIR = REPO / "data" / "fm2k_wiki" / "games"


SCORE_RULES = [
    (re.compile(r"vector\.co\.jp",       re.I),  5, "vector"),
    (re.compile(r"ux\.getuploader\.com", re.I),  4, "getuploader"),
    (re.compile(r"freem\.ne\.jp",        re.I),  4, "freem"),
    (re.compile(r"dlsite\.com",          re.I),  4, "dlsite"),
    (re.compile(r"fc2\.com|freett\.com|geocities\.(co\.|)jp", re.I), 2, "old-jp-host"),
    (re.compile(r"mediafire\.com|megaupload", re.I), -2, "dead-host"),
]
DIRECT_ARCHIVE_RE = re.compile(r"\.(zip|lzh|rar|7z|exe)\b", re.I)


def score_game(record: dict) -> tuple[int, list[str]]:
    score = 0
    reasons: list[str] = []
    outbound = record.get("outbound", [])
    if not outbound:
        return -3, ["no-outbound"]
    score += 1
    reasons.append("has-outbound")

    for link in outbound:
        url = link.get("url", "")
        for rule, pts, tag in SCORE_RULES:
            if rule.search(url):
                score += pts
                reasons.append(f"{'+' if pts >= 0 else ''}{pts}:{tag}")
        arc = link.get("archive") or {}
        if arc.get("wayback_url"):
            score += 2
            reasons.append("+2:archived")

    body = record.get("body_text", "")
    if DIRECT_ARCHIVE_RE.search(body):
        score += 3
        reasons.append("+3:direct-archive-in-body")

    return score, reasons


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--top", type=int, default=30)
    ap.add_argument("--engine", choices=("FM2K", "FM95", "any"), default="any")
    args = ap.parse_args()

    rows: list[tuple[int, str, str, list[str]]] = []
    for p in sorted(GAMES_DIR.glob("*.json")):
        rec = json.load(p.open(encoding="utf-8"))
        if args.engine != "any" and rec["engine"] != args.engine:
            continue
        score, reasons = score_game(rec)
        rows.append((score, rec["atwiki_id"], rec["title"], reasons))

    rows.sort(key=lambda r: -r[0])
    print(f"top {args.top} by score:")
    for score, aid, title, reasons in rows[: args.top]:
        print(f"  [{score:>3}] {aid:>4}  {title[:50]:<52}  {','.join(reasons[:6])}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
