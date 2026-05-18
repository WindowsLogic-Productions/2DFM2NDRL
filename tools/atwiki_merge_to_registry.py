#!/usr/bin/env python3
"""Merge atwiki + audit + local-inventory data into the launcher's
canonical games/registry.json so the stats hub renders archive info
directly on every per-game page (/g/{game_id}).

For each registry entry:
  1. Match against the atwiki index by name (fuzzy).
  2. If matched, attach the atwiki record:
       sources           += "atwiki"
       atwiki_id          = "86"
       atwiki_url         = "https://w.atwiki.jp/arunau32167/pages/86.html"
       atwiki_match_score = 0.83
       atwiki_match_method = "exact|substring|..."
       homepage           = pulled from outbound[0] iff registry blank
  3. Attach the audit verdict:
       archive_status   = "HAVE_LOCAL" | "KNOWN_RECOVERABLE" | etc.
       wayback_homepage = first archived outbound URL (if any)
  4. Attach archive links — one entry per atwiki outbound + each
     discovered download URL — with both live + Wayback variants:
       archive_links = [
         {"kind": "homepage", "url": "...", "wayback": "..."},
         {"kind": "download", "url": "...", "wayback": "...", "filename": "..."},
       ]
  5. Attach version list (distinct xxh64 across local + crossref):
       versions = [{"xxh64": "0x...", "size": N, "source": "local",
                    "exe_path": "..."}, ...]

Existing fields are never overwritten — `homepage`, `download_url`,
`year`, `engine`, etc. stay as-is unless previously empty.

Idempotent: re-running merges incremental updates without losing
prior entries (audit_status, versions, archive_links are recomputed
each time; everything else preserved).
"""

import argparse
import json
import re
import sys
from pathlib import Path
from typing import Optional


REPO = Path(__file__).resolve().parents[1]
INDEX_PATH    = REPO / "data" / "fm2k_wiki" / "index.json"
GAMES_DIR     = REPO / "data" / "fm2k_wiki" / "games"
INV_PATH      = REPO / "data" / "fm2k_wiki" / "local_inventory.json"
AUDIT_PATH    = REPO / "data" / "fm2k_wiki" / "audit_report.json"
REGISTRY_PATH = REPO / "games" / "registry.json"

ATWIKI_URL_FMT = "https://w.atwiki.jp/arunau32167/pages/{id}.html"

# Storefronts that sell the game — never auto-pull these. Resources
# pointing at them are reclassified to kind="store" so the hub /
# launcher render them as a purchase link rather than a free
# download. Keep in sync with PAYWALLED_HOSTS in
# tools/atwiki_playwright_pull.py.
PAYWALLED_HOSTS = (
    "dlsite.com",
    "dmm.com",
    "dmm.co.jp",
    "melonbooks.com",
    "melonbooks.co.jp",
    "toranoana.jp",
    "toranoana.shop",
    "booth.pm",
    "steampowered.com",
    "steamcommunity.com",
)


def is_paywalled(url: str) -> bool:
    u = (url or "").lower()
    return any(h in u for h in PAYWALLED_HOSTS)


# Old free-host hostnames that are guaranteed dead in 2026. We
# synthesize a Wayback calendar URL for any of these so editors can
# manually browse snapshots even when our auto-resolver came back empty
# (CDX outage, missing year-hint, etc.). The list is conservative —
# only hosts that have been completely shut down. Hosts that still
# have a live HTTP endpoint (vector.co.jp, w.atwiki.jp) are NOT here
# because we trust their live response.
DEAD_HOSTS = (
    "geocities.co.jp", "geocities.com", "geocities.jp",
    "freett.com", "freett.ne.jp", "page.freett.com", "big.freett.com",
    "tok2.com", "www56.tok2.com",
    "hp.infoseek.co.jp", "members.tripod.com",
    "homepage1.nifty.com", "homepage2.nifty.com", "homepage3.nifty.com",
    "ww1.tiki.ne.jp", "www.geocities.ws",
    "fc2web.com", "members.at.infoseek.co.jp",
    "members.jcom.home.ne.jp",
)


def is_dead_host(url: str) -> bool:
    u = (url or "").lower()
    return any(h in u for h in DEAD_HOSTS)


def wayback_closest(url: str) -> str:
    """Return a Wayback URL using the `/web/0/` "closest available
    snapshot" semantic. Behavior:
      - If Wayback has any snapshot of `url`: redirects to the closest
        one, with the toolbar+timeline visible at the top so editors
        can scrub to a different snapshot.
      - If Wayback has nothing: serves the "URL has not been captured"
        page cleanly, instead of a 404 or stuck calendar widget.

    The `/web/*/<url>` calendar form we used before requires Wayback's
    JS sparkline widget to render and isn't reliable in all browsers
    (also doesn't always parse without a numeric prefix like `20*`).
    `/web/0/` is plain HTTP redirect semantics and always resolves to
    something useful.
    """
    return f"https://web.archive.org/web/0/{url}"


# Backwards-compat alias — older runs of this script wrote rows
# tagged `preferred=wayback_calendar`. The migration step rewrites
# those archive_urls in place; this stub is here so external callers
# (if any) don't break on the rename.
def wayback_calendar(url: str) -> str:
    return wayback_closest(url)


_WAYBACK_ID_RE = re.compile(r"(/web/\d{8,14})id_/", re.IGNORECASE)


def strip_id_marker(wb_url: str) -> str:
    """Convert raw-bytes Wayback URLs (`/web/<ts>id_/<url>`) into the
    page-with-toolbar form (`/web/<ts>/<url>`).

    The `id_` suffix tells Wayback "give me the archived bytes
    untouched" — useful for embedding, but it strips the navigation
    toolbar that lets editors scrub to other snapshots in the
    timeline. Our wayback_probe fallback returns the `id_` form
    because it asks Wayback for a HEAD redirect; we normalize here so
    every snapshot URL we surface to editors has the timeline visible
    at the top of the page.
    """
    if not wb_url:
        return wb_url
    return _WAYBACK_ID_RE.sub(r"\1/", wb_url)


def normalize(s: str) -> str:
    s = (s or "").lower()
    s = re.sub(r"_?ver[_\-\s]*\d[\d\._]*", "", s)
    s = re.sub(r"\s*v?\d+(?:\.\d+)+\s*$", "", s)
    s = re.sub(r"[\s_\-　　]+", "", s)
    s = re.sub(r"[【】（）()「」『』:：・/\\|]+", "", s)
    return s


def fuzzy_score(a: str, b: str) -> tuple[float, str]:
    """Return (score, method) for two name candidates."""
    na, nb = normalize(a), normalize(b)
    if not na or not nb:
        return 0.0, ""
    if na == nb:
        return 1.0, "exact"
    if na in nb or nb in na:
        shorter, longer = sorted([na, nb], key=len)
        return len(shorter) / len(longer), "substring"
    return 0.0, ""


def best_match_for(reg: dict, atwiki: list[dict],
                   threshold: float = 0.55) -> Optional[dict]:
    """Pick the best atwiki entry for a registry record, considering
    its `name` and any `alt_names`."""
    candidates = [reg.get("name", "")] + list(reg.get("alt_names") or [])
    best, best_score, best_method = None, 0.0, ""
    for cand in candidates:
        if not cand:
            continue
        for entry in atwiki:
            score, method = fuzzy_score(cand, entry["title"])
            if score > best_score:
                best, best_score, best_method = entry, score, method
    if best and best_score >= threshold:
        return {"entry": best, "score": best_score, "method": best_method}
    return None


def build_resources(reg: dict, atwiki_entry: dict, rec: dict) -> list[dict]:
    """Build the unified `resources` list the hub renders.

    Schema (intentionally open — `kind` enumerates the ones we
    auto-populate, but manual additions can use any string):
      kind:        "atwiki" | "homepage" | "wayback" | "download"
                   | "wiki" | "discord" | "gamefaqs" | "store" | ...
      name:        short display label
      desc:        optional one-line description
      url:         live URL (may 404 — see archive_url)
      archive_url: optional Wayback / mirror URL
      source:      where this entry came from (e.g. "atwiki-crawler",
                   "manual", "mizuumi-importer")
    Plus, for kind=="download":
      filename, size_bytes, sha256, kind_real ("zip"/"exe"/...)
      pulled, error

    This script touches ONLY entries with source=="atwiki-crawler".
    Resources from other sources (manual / mizuumi / etc.) are
    preserved verbatim across re-runs.
    """
    # Preserve any manually-added resources or those from other importers.
    out = [r for r in (reg.get("resources") or [])
           if r.get("source") != "atwiki-crawler"]

    # 1. atwiki entry itself
    out.append({
        "kind":   "atwiki",
        "name":   f"atwiki #{atwiki_entry['atwiki_id']}",
        "desc":   "Japanese 2DFM doujin wiki entry (arunau32167)",
        "url":    ATWIKI_URL_FMT.format(id=atwiki_entry["atwiki_id"]),
        "source": "atwiki-crawler",
    })

    # 2. Homepage(s) + their Wayback snapshots. Paywalled storefronts
    # (dlsite, dmm, booth, etc.) are reclassified to kind="store" with
    # a purchase_url so the hub renders them as "buy here" rather
    # than a free download. Future hook: when the launcher embeds a
    # purchase button, point it at purchase_url.
    for link in rec.get("outbound", []):
        url = link["url"]
        arc = link.get("archive") or {}
        if is_paywalled(url):
            out.append({
                "kind":         "store",
                "name":         link.get("label") or url,
                "url":          url,
                "purchase_url": url,
                "source":       "atwiki-crawler",
            })
            continue
        # Always provide an archive_url for known-dead hosts. If our
        # auto-resolver found a real snapshot, use it; otherwise fall
        # back to the Wayback calendar URL so editors can hunt
        # manually. The kind stays "homepage" — it's the same link,
        # just with a guaranteed-clickable archive button.
        archive_url = strip_id_marker(arc.get("wayback_url"))
        preferred = arc.get("preferred")
        if not archive_url and is_dead_host(url):
            archive_url = wayback_closest(url)
            preferred = "wayback_closest"
        out.append({
            "kind":        "homepage",
            "name":        link.get("label") or url,
            "url":         url,
            "archive_url": archive_url,
            "preferred":   preferred,
            "source":      "atwiki-crawler",
        })

    # 3. Discovered downloads
    for d in rec.get("downloads", []):
        arc = d.get("archive") or {}
        entry = {
            "kind":        "download",
            "name":        d.get("filename") or d.get("original_url", ""),
            "filename":    d.get("filename"),
            "url":         d.get("original_url"),
            "archive_url": strip_id_marker(arc.get("wayback_url")),
            "found_via":   d.get("found_via"),
            "source":      "atwiki-crawler",
        }
        if "sha256" in d:
            entry["pulled"]     = True
            entry["size_bytes"] = d.get("size_bytes")
            entry["sha256"]     = d.get("sha256")
            entry["kind_real"]  = d.get("kind")
        elif "error" in d:
            entry["pulled"] = False
            entry["error"]  = d["error"]
        out.append(entry)
    return out


def collect_versions(rec: dict, locals_here: list[dict]) -> list[dict]:
    """Distinct xxh64s known for this game (local + crossref)."""
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
            "project":    g.get("project_name"),
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


def slugify(title: str, atwiki_id: str) -> str:
    """ASCII slug for the registry's `game_id` primary key.

    CJK-only / mostly-CJK titles (e.g. "8つの秘石リメイク" or "2Dたど") strip
    to junk like "8" or "2d", which is useless as a primary key. We
    require ≥4 letters of meaningful ASCII before accepting the slug;
    otherwise fall back to `atwiki-<id>`. Owners can rename via the
    admin editor once they have a romanization.
    """
    s = (title or "").lower()
    s = re.sub(r"[^a-z0-9]+", "-", s).strip("-")
    # Count actual letters (digits-only "2003" or "8" should not pass).
    letters = sum(1 for c in s if c.isalpha())
    if letters < 4:
        return f"atwiki-{atwiki_id}"
    if len(s) > 60:
        s = s[:60].rstrip("-")
    return s


def import_unmatched_atwiki(registry: list[dict], atwiki: list[dict],
                            audit_by_aid: dict, inv_by_aid: dict
                            ) -> tuple[int, int]:
    """Add fresh registry rows for atwiki entries that didn't fuzzy-
    match anything in the existing registry.

    Returns (n_imported, n_skipped_dupe). Skipped happens when a new
    atwiki entry's normalized title collides with an existing
    registry entry that didn't link via fuzzy_score (threshold miss
    but underlying same game) — we link the existing entry instead of
    creating a duplicate row.
    """
    linked_ids = {r.get("atwiki_id") for r in registry if r.get("atwiki_id")}
    existing_ids = {r.get("game_id") for r in registry}
    existing_norm: dict[str, dict] = {}
    for r in registry:
        for cand in [r.get("name", "")] + list(r.get("alt_names") or []):
            n = normalize(cand)
            if n:
                existing_norm.setdefault(n, r)

    n_imported = 0
    n_skipped_dupe = 0

    for entry in atwiki:
        aid = entry["atwiki_id"]
        if aid in linked_ids:
            continue

        # Last-ditch dedupe: did the title normalize to the same key
        # as an existing entry? (Catches near-misses below the fuzzy
        # threshold — e.g. "Axel City SCU" vs "Axel City".)
        n = normalize(entry["title"])
        if n and n in existing_norm:
            tgt = existing_norm[n]
            if not tgt.get("atwiki_id"):
                tgt["atwiki_id"]           = aid
                tgt["atwiki_match_score"]  = 1.0
                tgt["atwiki_match_method"] = "post_normalize"
                linked_ids.add(aid)
                n_skipped_dupe += 1
            continue

        # Generate unique game_id slug.
        base = slugify(entry["title"], aid)
        gid = base
        suffix = 2
        while gid in existing_ids:
            gid = f"{base}-{suffix}"
            suffix += 1
        existing_ids.add(gid)

        # Build the per-game JSON-derived bits.
        rec_path = GAMES_DIR / f"{aid}.json"
        rec = (json.load(rec_path.open(encoding="utf-8"))
               if rec_path.exists() else {})

        homepage = ""
        wayback_homepage = None
        for link in rec.get("outbound", []):
            if link.get("url"):
                homepage = link["url"]
                arc = link.get("archive") or {}
                wayback_homepage = strip_id_marker(arc.get("wayback_url"))
                break

        new_row = {
            "game_id":             gid,
            "name":                entry["title"],
            "alt_names":           [],
            "engine":              entry.get("engine", "FM2K"),
            "year":                "",
            "developer":           "",
            "publisher":           "",
            "exe_stems":           [gid],
            "kgt_filename":        "",
            "homepage":            homepage,
            "download_url":        "",
            "banner_url":          "",
            "thumb_url":           "",
            "sources":             ["atwiki"],
            "characters":          [],
            "stages":              [],
            "_raw":                {},
            "atwiki_id":           aid,
            "atwiki_match_score":  1.0,
            "atwiki_match_method": "imported",
            "atwiki_url":          ATWIKI_URL_FMT.format(id=aid),
            "archive_status":      audit_by_aid.get(aid, {}).get(
                                       "status", "NO_OUTBOUND"),
            "wayback_homepage":    wayback_homepage,
            "imported_from":       "atwiki-merge",
        }

        # Attach resources + versions same way as the matched path.
        new_row["resources"] = build_resources(new_row, entry, rec)
        new_row["versions"]  = collect_versions(rec, inv_by_aid.get(aid, []))

        registry.append(new_row)
        linked_ids.add(aid)
        n_imported += 1

    return n_imported, n_skipped_dupe


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--threshold", type=float, default=0.55,
                    help="min fuzzy score for an atwiki match (default 0.55)")
    ap.add_argument("--dry-run", action="store_true",
                    help="print what would change, don't write registry.json")
    ap.add_argument("--no-import-unmatched", action="store_true",
                    help="don't add atwiki games missing from the registry "
                         "as new rows (default: import them)")
    args = ap.parse_args()

    for p in (INDEX_PATH, INV_PATH, AUDIT_PATH, REGISTRY_PATH):
        if not p.exists():
            print(f"missing: {p}", file=sys.stderr)
            return 1

    atwiki = json.load(INDEX_PATH.open(encoding="utf-8"))["games"]
    inv    = json.load(INV_PATH.open(encoding="utf-8"))
    audit  = json.load(AUDIT_PATH.open(encoding="utf-8"))
    registry = json.load(REGISTRY_PATH.open(encoding="utf-8"))

    # Pre-index audit + inventory by atwiki_id for O(1) lookup.
    audit_by_aid = {r["atwiki_id"]: r for r in audit["rows"]}
    inv_by_aid: dict[str, list[dict]] = {}
    for g in inv["local_games"]:
        m = g.get("atwiki_match")
        if m and m.get("atwiki_id"):
            inv_by_aid.setdefault(m["atwiki_id"], []).append(g)

    n_matched = n_already = n_changed = 0
    for reg in registry:
        # Already linked? Refresh the dynamic fields but keep the link.
        atwiki_id = reg.get("atwiki_id")
        if not atwiki_id:
            picked = best_match_for(reg, atwiki, threshold=args.threshold)
            if not picked:
                continue
            atwiki_id = picked["entry"]["atwiki_id"]
            reg["atwiki_id"]            = atwiki_id
            reg["atwiki_match_score"]   = round(picked["score"], 3)
            reg["atwiki_match_method"]  = picked["method"]
            n_matched += 1
        else:
            n_already += 1

        reg["atwiki_url"] = ATWIKI_URL_FMT.format(id=atwiki_id)
        sources = reg.get("sources") or []
        if isinstance(sources, list) and "atwiki" not in sources:
            sources.append("atwiki")
            reg["sources"] = sources

        # Pull the per-game atwiki record (HTML-parsed).
        rec_path = GAMES_DIR / f"{atwiki_id}.json"
        rec = (json.load(rec_path.open(encoding="utf-8"))
               if rec_path.exists() else {})

        # Backfill homepage when the registry didn't already have one.
        if not reg.get("homepage"):
            for link in rec.get("outbound", []):
                if link.get("url"):
                    reg["homepage"] = link["url"]
                    break

        # Audit verdict (archive_status) + first wayback URL.
        audit_row = audit_by_aid.get(atwiki_id, {})
        reg["archive_status"] = audit_row.get("status", "NO_OUTBOUND")
        for link in rec.get("outbound", []):
            arc = link.get("archive") or {}
            if arc.get("wayback_url"):
                reg["wayback_homepage"] = strip_id_marker(arc["wayback_url"])
                break

        # Generalized resources list — atwiki, homepage, wayback,
        # downloads, all in one shape so the template can render
        # them uniformly and other importers (mizuumi, gamefaqs,
        # discord, manual) can co-exist.
        atwiki_entry = next((e for e in atwiki
                             if e["atwiki_id"] == atwiki_id), None)
        if atwiki_entry:
            reg["resources"] = build_resources(reg, atwiki_entry, rec)
        reg["versions"] = collect_versions(rec, inv_by_aid.get(atwiki_id, []))
        n_changed += 1

    n_imported = 0
    n_dedup = 0
    if not args.no_import_unmatched:
        n_imported, n_dedup = import_unmatched_atwiki(
            registry, atwiki, audit_by_aid, inv_by_aid)

    print(f"  matched (newly linked): {n_matched}")
    print(f"  matched (pre-existing): {n_already}")
    print(f"  rows updated:           {n_changed}")
    if not args.no_import_unmatched:
        print(f"  imported (new rows):    {n_imported}")
        print(f"  deduped (post-norm):    {n_dedup}")
    print(f"  registry size:          {len(registry)}")

    if args.dry_run:
        print("  (dry-run — registry.json not written)")
        return 0

    REGISTRY_PATH.write_text(
        json.dumps(registry, ensure_ascii=False, indent=2),
        encoding="utf-8")
    print(f"  wrote -> {REGISTRY_PATH.relative_to(REPO)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
