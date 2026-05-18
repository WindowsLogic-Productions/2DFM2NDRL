#!/usr/bin/env python3
"""Produce the 'what do I have / what's missing / where to recover'
audit. Joins three data sources:

  1. atwiki index            (data/fm2k_wiki/index.json)
  2. atwiki per-game records (data/fm2k_wiki/games/<id>.json)
  3. local inventory         (data/fm2k_wiki/local_inventory.json)

For each atwiki entry, classifies into one of:
  HAVE_LOCAL          — at least one local install matched
  HAVE_ARCHIVE_BYTES  — we successfully pulled a real ZIP/EXE (magic-byte verified)
  KNOWN_RECOVERABLE   — Wayback has a snapshot of the homepage but we
                        haven't (or can't) fetch the binary yet
  NEEDS_HUNT          — outbound URLs are dead AND our auto-crawl found
                        no Wayback snapshot. The bytes might still exist
                        on a private archive, Twitter post, or older
                        Wayback snapshot we missed — this label means
                        "needs a human to hunt", not "lost media".
                        (Renamed from KNOWN_UNRECOVERABLE → NO_AUTO_ARCHIVE
                        → NEEDS_HUNT as we kept dropping the certainty.)
  NO_OUTBOUND         — atwiki entry has no homepage/dl link to chase

Also produces a per-version (xxh64) view: for atwiki games we have
locally, list every distinct exe hash seen (across multiple installs
+ any future archive pulls). Lets us answer "I have v0.5 of X but the
atwiki page says v1.2 was the final — am I missing the latest?"

Output:
  data/fm2k_wiki/audit_report.json   (machine-readable)
  stdout                             (human-readable summary)
"""

import argparse
import json
import sys
from collections import defaultdict
from pathlib import Path


REPO = Path(__file__).resolve().parents[1]
INDEX_PATH = REPO / "data" / "fm2k_wiki" / "index.json"
GAMES_DIR  = REPO / "data" / "fm2k_wiki" / "games"
INV_PATH   = REPO / "data" / "fm2k_wiki" / "local_inventory.json"
OUT_PATH   = REPO / "data" / "fm2k_wiki" / "audit_report.json"


STATUS_HAVE_LOCAL          = "HAVE_LOCAL"
STATUS_HAVE_ARCHIVE_BYTES  = "HAVE_ARCHIVE_BYTES"
STATUS_KNOWN_RECOVERABLE   = "KNOWN_RECOVERABLE"
STATUS_NEEDS_HUNT = "NEEDS_HUNT"
STATUS_NO_OUTBOUND         = "NO_OUTBOUND"


def load_atwiki_record(atwiki_id: str) -> dict | None:
    p = GAMES_DIR / f"{atwiki_id}.json"
    if not p.exists():
        return None
    return json.loads(p.read_text(encoding="utf-8"))


def classify(rec: dict, locals_by_aid: dict[str, list[dict]]) -> tuple[str, dict]:
    """Decide which bucket this atwiki entry belongs to plus a small
    detail dict the report can render."""
    aid = rec["atwiki_id"]
    locals_here = locals_by_aid.get(aid, [])
    if locals_here:
        return STATUS_HAVE_LOCAL, {"local_count": len(locals_here)}

    # Any successfully pulled archive (magic-byte verified)?
    has_real_bytes = any(
        d.get("kind") in {"zip", "lzh", "rar", "7z", "exe"}
        and "sha256" in d
        for d in rec.get("downloads", [])
    )
    if has_real_bytes:
        return STATUS_HAVE_ARCHIVE_BYTES, {
            "archive_count": sum(1 for d in rec.get("downloads", [])
                                 if "sha256" in d)
        }

    outbound = rec.get("outbound", [])
    if not outbound:
        return STATUS_NO_OUTBOUND, {}

    archived = [l for l in outbound
                if (l.get("archive") or {}).get("wayback_url")]
    if archived:
        return STATUS_KNOWN_RECOVERABLE, {
            "archived_outbound": len(archived),
            "first_snapshot":    (archived[0]["archive"] or {}).get("wayback_url"),
        }

    return STATUS_NEEDS_HUNT, {"outbound_count": len(outbound)}


def collect_versions(rec: dict, locals_here: list[dict]) -> list[dict]:
    """Distinct exe xxh64s known for this game (local + any archive
    pulls that produced exes via crossref)."""
    seen: dict[str, dict] = {}
    for g in locals_here:
        h = g.get("exe_xxh64")
        if not h:
            continue
        seen.setdefault(h, {
            "xxh64":      h,
            "size_bytes": g.get("exe_size"),
            "source":     "local",
            "exe_path":   g.get("exe_path"),
        })
    for m in rec.get("exe_matches", []):
        h = m.get("xxh64")
        if not h:
            continue
        if h in seen:
            seen[h]["source"] = "local+archive"
            seen[h]["from_archive"] = m.get("from_archive")
        else:
            seen[h] = {
                "xxh64":        h,
                "size_bytes":   m.get("size_bytes"),
                "source":       "archive",
                "from_archive": m.get("from_archive"),
                "match_known":  m.get("match_known"),
            }
    return list(seen.values())


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", type=Path, default=OUT_PATH)
    args = ap.parse_args()

    if not INDEX_PATH.exists():
        print(f"missing {INDEX_PATH}", file=sys.stderr)
        return 1
    if not INV_PATH.exists():
        print(f"missing {INV_PATH} — run atwiki_local_inventory.py first",
              file=sys.stderr)
        return 1

    index = json.load(INDEX_PATH.open(encoding="utf-8"))["games"]
    inv   = json.load(INV_PATH.open(encoding="utf-8"))

    # Group locals by their matched atwiki_id (None bucket for unmatched).
    locals_by_aid: dict[str, list[dict]] = defaultdict(list)
    locals_unmatched: list[dict] = []
    for g in inv["local_games"]:
        m = g.get("atwiki_match")
        if m and m.get("atwiki_id"):
            locals_by_aid[m["atwiki_id"]].append(g)
        else:
            locals_unmatched.append(g)

    by_status: dict[str, list[dict]] = defaultdict(list)
    audit_rows: list[dict] = []

    for entry in index:
        rec = load_atwiki_record(entry["atwiki_id"])
        if rec is None:
            # Indexed but not yet crawled — treat as no_outbound.
            status, detail = STATUS_NO_OUTBOUND, {}
            outbound_count = 0
        else:
            status, detail = classify(rec, locals_by_aid)
            outbound_count = len(rec.get("outbound", []))

        locals_here = locals_by_aid.get(entry["atwiki_id"], [])
        versions = collect_versions(rec or {}, locals_here)

        row = {
            "atwiki_id":    entry["atwiki_id"],
            "title":        entry["title"],
            "engine":       entry["engine"],
            "status":       status,
            "detail":       detail,
            "local_count":  len(locals_here),
            "versions":     versions,
        }
        audit_rows.append(row)
        by_status[status].append(row)

    report = {
        "generated_at":      inv.get("scan_time"),
        "atwiki_total":      len(index),
        "local_total":       len(inv["local_games"]),
        "local_matched":     sum(len(v) for v in locals_by_aid.values()),
        "local_unmatched":   len(locals_unmatched),
        "by_status":         {k: len(v) for k, v in by_status.items()},
        "rows":              audit_rows,
        "unmatched_locals":  locals_unmatched,
    }
    args.out.write_text(json.dumps(report, ensure_ascii=False, indent=2),
                        encoding="utf-8")

    # Human-readable summary
    print()
    print("=" * 70)
    print(f"  FM2K / FM95 archive audit")
    print("=" * 70)
    print(f"  atwiki entries:          {report['atwiki_total']}")
    print(f"  local installs found:    {report['local_total']}")
    print(f"  local matched to atwiki: {report['local_matched']}")
    print(f"  local with no atwiki:    {report['local_unmatched']}")
    print()
    print("  status breakdown (per atwiki entry):")
    width = max(len(s) for s in by_status) if by_status else 0
    for status in (STATUS_HAVE_LOCAL,
                   STATUS_HAVE_ARCHIVE_BYTES,
                   STATUS_KNOWN_RECOVERABLE,
                   STATUS_NEEDS_HUNT,
                   STATUS_NO_OUTBOUND):
        count = len(by_status.get(status, []))
        bar = "█" * int(50 * count / report['atwiki_total'])
        print(f"    {status:<{width}}  {count:>4}  {bar}")
    print()
    print(f"  versions tracked (distinct xxh64): "
          f"{sum(len(r['versions']) for r in audit_rows)}")
    print()
    print(f"  wrote -> {args.out.relative_to(REPO)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
