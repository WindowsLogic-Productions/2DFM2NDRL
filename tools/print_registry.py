#!/usr/bin/env python3
"""Render games/registry.json as a human-reviewable markdown report.

Produces games/REGISTRY.md with three tiers, ordered most→least
confident. The .kgt presence is the central signal — every real FM2K
game ships a compiled gamesystem with that extension, so confirming
its presence inside the upload's zip is the strongest possible
"this is FM2K" check.

Tiers:
  1. CONFIRMED  — at least one .kgt file seen inside the IA zip OR
                  a sibling MyAbandonware install with a known game.
                  These are publishable as-is.
  2. PROBABLE   — keyword + archive match but IA hadn't indexed the
                  zip's contents at scrape time. Likely FM2K but
                  needs a re-run of inspect_ia_zips.py later, or a
                  manual download to verify.
  3. NEEDS_REVIEW — no archive listed at all, or no usable exe_stem.
                  Hand-edit games/registry_overrides.json to
                  finish.

Also writes a 4th section listing AUTO-DROPPED candidates (engine
bundles, MUGEN, commercial ports, etc.) from tools/ia_scrape.json
so you can spot-check the filter didn't over-prune.

Run:    python3 tools/print_registry.py
Output: games/REGISTRY.md
"""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any


REPO_ROOT = Path(__file__).resolve().parent.parent
REGISTRY_PATH = REPO_ROOT / "games" / "registry.json"
SCRAPE_PATH = REPO_ROOT / "tools" / "ia_scrape.json"
OUT_PATH = REPO_ROOT / "games" / "REGISTRY.md"


def kgt_for(rec: dict[str, Any]) -> str:
    """Return the kgt_filename or '' if none. Look at top-level field
    first, fall back to scanning inner_files of the IA raw."""
    if rec.get("kgt_filename"):
        return rec["kgt_filename"]
    inner = (rec.get("_raw", {}).get("archive.org", {})
             .get("inner_files") or [])
    for f in inner:
        if f.lower().endswith(".kgt"):
            return Path(f).name
    return ""


def has_archive(rec: dict[str, Any]) -> bool:
    raw = rec.get("_raw", {})
    if "MyAbandonware" in raw:
        return True
    files = (raw.get("archive.org", {}).get("files") or [])
    return any(f.lower().endswith((".zip", ".7z", ".rar")) for f in files)


def homepage(rec: dict[str, Any]) -> str:
    return rec.get("homepage") or ""


def md_escape(s: str) -> str:
    """Minimal escape for markdown table cells: pipes break columns,
    angle-brackets get stripped of literal HTML, newlines flatten."""
    if not s:
        return ""
    return (s.replace("|", "\\|")
             .replace("\n", " ")
             .replace("\r", " "))


def short(s: str, n: int) -> str:
    if not s:
        return ""
    return s if len(s) <= n else s[:n - 1] + "…"


def render_table(rows: list[dict[str, Any]],
                 columns: list[tuple[str, callable]]) -> str:
    out = []
    headers = [name for name, _ in columns]
    out.append("| " + " | ".join(headers) + " |")
    out.append("|" + "|".join(["---"] * len(headers)) + "|")
    for r in rows:
        cells = [md_escape(str(fn(r) or "")) for _, fn in columns]
        out.append("| " + " | ".join(cells) + " |")
    return "\n".join(out)


def main() -> int:
    if not REGISTRY_PATH.exists():
        print(f"error: {REGISTRY_PATH} not found")
        return 1
    recs = json.loads(REGISTRY_PATH.read_text(encoding="utf-8"))
    scrape = (json.loads(SCRAPE_PATH.read_text(encoding="utf-8"))
              if SCRAPE_PATH.exists() else [])

    # Sort each tier by name (case-insensitive) for readability.
    def name_key(r: dict[str, Any]) -> str:
        return (r.get("name") or "").lower()

    confirmed = sorted([r for r in recs if kgt_for(r)], key=name_key)
    probable = sorted([r for r in recs
                       if not kgt_for(r) and r.get("exe_stems")
                       and has_archive(r)], key=name_key)
    needs_review = sorted([r for r in recs
                           if r not in confirmed and r not in probable],
                          key=name_key)
    dropped = sorted([r for r in scrape if r.get("engine_bundle")],
                     key=lambda r: (r.get("title") or "").lower())

    cols_full = [
        ("game_id",  lambda r: short(r.get("game_id", ""), 28)),
        ("name",     lambda r: short(r.get("name", ""), 50)),
        ("year",     lambda r: r.get("year") or ""),
        ("exe_stem", lambda r: short((r.get("exe_stems") or [""])[0], 24)),
        ("kgt",      lambda r: short(kgt_for(r), 32)),
        ("sources",  lambda r: ",".join(r.get("sources") or [])),
        ("link",     lambda r: f"[link]({homepage(r)})" if homepage(r) else ""),
    ]
    cols_review = [
        ("name",      lambda r: short(r.get("name", ""), 50)),
        ("game_id",   lambda r: short(r.get("game_id", ""), 28)),
        ("source",    lambda r: ",".join(r.get("sources") or [])),
        ("notes",     lambda r: ("no archive in IA item"
                                 if not has_archive(r)
                                 else "no exe_stem set")),
        ("link",      lambda r: f"[link]({homepage(r)})" if homepage(r) else ""),
    ]
    cols_dropped = [
        ("title",   lambda r: short(r.get("title", ""), 60)),
        ("ident",   lambda r: short(r.get("ia_identifier", ""), 36)),
        ("reason",  lambda r: r.get("non_game_reason") or
                              "engine_bundle title/ident match"),
    ]

    md = []
    md.append(f"# FM2K Games Registry\n")
    md.append(f"_Generated by `tools/print_registry.py` from "
              f"`games/registry.json` and `tools/ia_scrape.json`._\n")
    md.append("")
    md.append(f"**Counts**:")
    md.append(f"- Confirmed (.kgt seen): **{len(confirmed)}**")
    md.append(f"- Probable (archive + exe_stem, kgt not yet seen): "
              f"**{len(probable)}**")
    md.append(f"- Needs manual review: **{len(needs_review)}**")
    md.append(f"- Auto-dropped as non-FM2K: **{len(dropped)}**")
    md.append("")

    md.append("## Confirmed games\n")
    md.append("These have a `.kgt` file confirmed inside the upload "
              "(or a sibling MyAbandonware install).")
    md.append("")
    md.append(render_table(confirmed, cols_full))
    md.append("")

    md.append("## Probable games (need re-scan or manual verify)\n")
    md.append("Keyword + archive match but IA hadn't extracted the "
              "zip's contents at scrape time. Re-run "
              "`tools/inspect_ia_zips.py --refresh` later, or "
              "download the zip and check for `.kgt` manually.")
    md.append("")
    md.append(render_table(probable, cols_full))
    md.append("")

    md.append("## Needs manual review\n")
    md.append("No archive in the IA item, or the scrape didn't yield "
              "a usable exe_stem. Hand-edit "
              "`games/registry_overrides.json` to fix.")
    md.append("")
    md.append(render_table(needs_review, cols_review))
    md.append("")

    md.append("## Auto-dropped (filtered as non-FM2K)\n")
    md.append("Items the scrape pulled in but were classified as "
              "engine bundles / MUGEN / arcade ROMs / GDC talks / "
              "commercial PC ports / wikis / fixpacks. Spot-check "
              "this list — if a real FM2K game got dropped, add an "
              "entry to `games/registry_overrides.json` to override.")
    md.append("")
    md.append(render_table(dropped, cols_dropped))
    md.append("")

    OUT_PATH.write_text("\n".join(md), encoding="utf-8")
    print(f"wrote {OUT_PATH.relative_to(REPO_ROOT)} "
          f"({len(confirmed)} confirmed, "
          f"{len(probable)} probable, "
          f"{len(needs_review)} review, "
          f"{len(dropped)} dropped)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
