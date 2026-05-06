#!/usr/bin/env python3
"""Download verified-FM2K zips from Internet Archive.

Reads the scrape produced by tools/inspect_ia_zips.py
(tools/ia_scrape.json — contains 208 IA items with their inner-zip
file listings already inspected) and pulls the actual archive bytes
to disk so we can extract + register them like any other 2dfm game
distribution.

Filters applied:
  - verified FM2K only — record's `inner_files` contains a .kgt
  - skip engine SDK records — idents matching "fighter-maker" /
    explicit known SDK list — those go to a separate dest bucket
  - skip local duplicates — record's `exe_stem` matches an
    exe_stem in games/registry.json (we already have it locally)

Output layout:
  <dest>/<ident>/
    metadata.json   (generated from scrape data)
    <orig_zip>.zip  (one per .zip listed in record.files)

The metadata.json shape matches what the existing curation tree
uses (name / year / developer / url / download / source) so the
flatten + apply-clean-engine tools chain on cleanly.

Resume support:
  - Idempotent: skip if local zip's size matches archive.org's
    Content-Length on HEAD.
  - Partial: resume via HTTP Range when local size < expected.
  - Retries 3× with exponential backoff on transient errors.

Usage:
    python3 tools/download_ia_zips.py
    python3 tools/download_ia_zips.py --dest /mnt/d/games/2dfm_ia
    python3 tools/download_ia_zips.py --include-sdk     # also pull engine
    python3 tools/download_ia_zips.py --include-dupes   # also re-pull
                                                          existing local
                                                          stems
    python3 tools/download_ia_zips.py --only foo,bar    # ident substrings
    python3 tools/download_ia_zips.py --dry-run         # report only
"""

from __future__ import annotations

import argparse
import json
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path
from typing import Any


REPO_ROOT = Path(__file__).resolve().parent.parent
SCRAPE_PATH = REPO_ROOT / "tools" / "ia_scrape.json"
REGISTRY_PATH = REPO_ROOT / "games" / "registry.json"

DEST_GAMES_DEFAULT = Path("/mnt/d/games/2dfm_ia")
DEST_SDK_DEFAULT = Path("/mnt/d/games/fm2k_sdk")

DOWNLOAD_BASE = "https://archive.org/download/{ident}/{name}"

# IA records that are the FM2K engine itself rather than a playable
# game built on it. Bulk-download into a separate bucket.
SDK_IDENT_PATTERNS = (
    "fighter-maker", "2dfightermaker", "2dfighter-maker",
    "fightermaker", "fighter_maker",
)

# Polite to identify ourselves; archive.org rate-limits anonymous UAs.
USER_AGENT = ("FM2K-Rollback/0.1 (https://github.com/Armonte; "
              "downloading per-game scenes for offline play)")

CHUNK_BYTES = 256 * 1024
PROGRESS_INTERVAL_S = 1.0
RETRY_BACKOFF_S = (2.0, 5.0, 12.0)


def is_verified_fm2k(rec: dict[str, Any]) -> bool:
    return any(f.lower().endswith(".kgt")
               for f in rec.get("inner_files", []))


def is_sdk(ident: str) -> bool:
    low = ident.lower()
    return any(p in low for p in SDK_IDENT_PATTERNS)


def normalize_stem(s: str) -> str:
    """Strip case, spaces, dashes, underscores, tildes — anything
    naming-convention-ish — so cross-distribution names of the same
    game collapse to the same key. `Eight Marbles 2X`,
    `8-marbles-2-x`, `8marbles2x`, `8_Marbles_2X` all → `8marbles2x`."""
    return ((s or "").lower()
            .replace(" ", "").replace("-", "")
            .replace("_", "").replace("~", "")
            .replace(".", ""))


def load_local_dedup_keys(local_root: Path) -> set[str]:
    """Build a normalized key set we can match IA records against.
    Pulls in:
      - every .kgt stem on disk (the canonical match for FM2K games)
      - every ancestor dir name on the path from .kgt back up to
        local_root (catches deeply-nested + cross-naming cases:
        `KEROPYON/Crimson Alive_ Extreme Encounter/.../encounter.kgt`
        vs IA `crimson-alive-extreme-encounter` — only the
        great-grandparent dir name matches; the kgt stem doesn't)

    Also auto-skip useless root-level container names that would
    cause false-positive collisions across unrelated games."""
    if not local_root.exists():
        return set()
    # These are buckets / categories, not game names. If we keyed
    # on them every IA record matching would get falsely deduped.
    GENERIC_BUCKETS = {"_nodev", "fm95", "fm2k", "2dfm", "games"}
    norm: set[str] = set()
    for kgt in local_root.rglob("*.[Kk][Gg][Tt]"):
        if not kgt.is_file():
            continue
        norm.add(normalize_stem(kgt.stem))
        # Walk every ancestor dir between the kgt and local_root.
        for anc in kgt.parents:
            try:
                if anc == local_root or anc.parent == anc:
                    break
                anc.relative_to(local_root)
            except ValueError:
                break
            n = normalize_stem(anc.name)
            if n and n not in GENERIC_BUCKETS:
                norm.add(n)
    norm.discard("")
    return norm


def filter_records(scrape: list[dict[str, Any]],
                   *,
                   include_sdk: bool,
                   include_dupes: bool,
                   only: list[str],
                   local_root: Path,
                   ) -> tuple[list[dict[str, Any]],
                              list[dict[str, Any]],
                              dict[str, int]]:
    """Returns (game_records, sdk_records, stats)."""
    local_stems = load_local_dedup_keys(local_root)
    games: list[dict[str, Any]] = []
    sdks: list[dict[str, Any]] = []
    n_skipped_unverified = 0
    n_skipped_dupe = 0
    n_skipped_filter = 0
    for rec in scrape:
        ident = rec.get("ia_identifier") or ""
        if only and not any(s in ident for s in only):
            n_skipped_filter += 1
            continue
        if not is_verified_fm2k(rec):
            n_skipped_unverified += 1
            continue
        if is_sdk(ident):
            sdks.append(rec)
            continue
        if not include_dupes:
            # Normalize multiple identifying strings — IA's exe_stem,
            # ident slug, and inner-zip kgt filename — and skip if
            # any of them matches a kgt we already have on disk.
            keys: set[str] = set()
            keys.add(normalize_stem(rec.get("exe_stem", "")))
            keys.add(normalize_stem(ident))
            kgt = rec.get("kgt_filename", "")
            if kgt:
                # kgt_filename may be "ReSHUFFLE.kgt" — drop the ext.
                stem = kgt.rsplit(".", 1)[0]
                keys.add(normalize_stem(stem))
            keys.discard("")
            if keys & local_stems:
                n_skipped_dupe += 1
                continue
        games.append(rec)
    return games, sdks, {
        "unverified": n_skipped_unverified,
        "dupe": n_skipped_dupe,
        "filtered_out": n_skipped_filter,
    }


def http_open(url: str, *, headers: dict[str, str] | None = None,
              method: str = "GET",
              timeout_s: float = 60.0):
    req = urllib.request.Request(
        url, method=method,
        headers={"User-Agent": USER_AGENT, **(headers or {})})
    return urllib.request.urlopen(req, timeout=timeout_s)


def head_size(url: str) -> int | None:
    """Issue HEAD; return Content-Length (after redirects) or None.
    archive.org sometimes returns 405 on HEAD — fall back to a
    Range:0-0 GET to extract Content-Range / total."""
    try:
        with http_open(url, method="HEAD", timeout_s=30.0) as r:
            cl = r.headers.get("Content-Length")
            if cl is not None:
                return int(cl)
    except (urllib.error.URLError, ValueError, TimeoutError):
        pass
    try:
        with http_open(url, headers={"Range": "bytes=0-0"},
                       timeout_s=30.0) as r:
            cr = r.headers.get("Content-Range") or ""
            # "bytes 0-0/123456"
            if "/" in cr:
                tail = cr.split("/")[-1].strip()
                if tail.isdigit():
                    return int(tail)
            cl = r.headers.get("Content-Length")
            # If the server ignored Range we get the whole file's
            # size in Content-Length, but we don't want to actually
            # download that much for a probe — caller closes the
            # body without reading. Returning the size is OK either
            # way.
            if cl is not None:
                return int(cl)
    except (urllib.error.URLError, ValueError, TimeoutError):
        pass
    return None


def fmt_size(n: int) -> str:
    units = ["B", "KB", "MB", "GB"]
    s = float(n)
    for u in units:
        if s < 1024.0:
            return f"{s:7.1f} {u}"
        s /= 1024.0
    return f"{s:7.1f} TB"


def download_with_resume(url: str, target: Path, *,
                         expected_size: int | None,
                         label: str) -> bool:
    """Download `url` to `target`, resuming if local file is partial.
    Returns True on success."""
    target.parent.mkdir(parents=True, exist_ok=True)
    have = target.stat().st_size if target.exists() else 0
    if expected_size is not None and have == expected_size:
        print(f"  ✓ {label}: already complete "
              f"({fmt_size(have)})", flush=True)
        return True
    if expected_size is not None and have > expected_size:
        # Local overshot — server changed file or we got corrupted.
        # Restart from scratch.
        target.unlink()
        have = 0

    headers: dict[str, str] = {}
    open_mode = "wb"
    if have > 0:
        headers["Range"] = f"bytes={have}-"
        open_mode = "ab"
        print(f"  ⏵ {label}: resuming at {fmt_size(have)}",
              flush=True)
    else:
        print(f"  ⏵ {label}: starting "
              f"({fmt_size(expected_size) if expected_size else '?'} "
              f"total)", flush=True)

    last_tick = time.monotonic()
    last_progress_bytes = have
    try:
        with http_open(url, headers=headers, timeout_s=60.0) as resp:
            with target.open(open_mode) as f:
                while True:
                    chunk = resp.read(CHUNK_BYTES)
                    if not chunk:
                        break
                    f.write(chunk)
                    have += len(chunk)
                    now = time.monotonic()
                    if now - last_tick >= PROGRESS_INTERVAL_S:
                        delta = have - last_progress_bytes
                        rate = delta / (now - last_tick)
                        if expected_size:
                            pct = 100.0 * have / expected_size
                            print(f"    {fmt_size(have)} / "
                                  f"{fmt_size(expected_size)}  "
                                  f"({pct:5.1f}%)  "
                                  f"{fmt_size(int(rate))}/s",
                                  flush=True)
                        else:
                            print(f"    {fmt_size(have)}  "
                                  f"{fmt_size(int(rate))}/s",
                                  flush=True)
                        last_tick = now
                        last_progress_bytes = have
    except (urllib.error.URLError, TimeoutError, OSError) as e:
        print(f"  ✗ {label}: error {e}", file=sys.stderr)
        return False

    if expected_size is not None and have != expected_size:
        print(f"  ⚠ {label}: stopped at {fmt_size(have)} of "
              f"{fmt_size(expected_size)} (will resume on next run)",
              file=sys.stderr)
        return False
    print(f"  ✓ {label}: {fmt_size(have)}", flush=True)
    return True


def download_with_retry(url: str, target: Path, *,
                        expected_size: int | None,
                        label: str) -> bool:
    for attempt, backoff in enumerate(RETRY_BACKOFF_S, 1):
        if download_with_resume(url, target,
                                expected_size=expected_size,
                                label=label):
            return True
        print(f"  ↻ {label}: retry {attempt}/{len(RETRY_BACKOFF_S)} "
              f"in {backoff:.0f}s", flush=True)
        time.sleep(backoff)
    return download_with_resume(url, target,
                                expected_size=expected_size,
                                label=label)


def write_metadata(rec: dict[str, Any], dest_dir: Path) -> None:
    """Generate a metadata.json compatible with the existing
    curated-tree schema."""
    meta = {
        "name":             rec.get("title") or rec["ia_identifier"],
        "alternative names": [],
        "year":             rec.get("year") or "",
        "publisher":        "",
        "developer":        rec.get("creator") or "",
        "url":              rec.get("homepage") or "",
        "download":         rec.get("download") or "",
        "source":           rec.get("source") or "archive.org",
        "ia_identifier":    rec["ia_identifier"],
        "ia_kgt":           rec.get("kgt_filename") or "",
        "ia_exe_stem":      rec.get("exe_stem") or "",
    }
    p = dest_dir / "metadata.json"
    p.write_text(json.dumps(meta, indent=2, ensure_ascii=False) + "\n",
                 encoding="utf-8")


def process_record(rec: dict[str, Any], dest_root: Path,
                   *, dry_run: bool) -> tuple[bool, int]:
    """Download every .zip in `rec.files` to dest_root/<ident>/.
    Returns (success, bytes_downloaded_this_session)."""
    ident = rec["ia_identifier"]
    dest_dir = dest_root / ident
    zips = [f for f in rec.get("files", [])
            if f.lower().endswith(".zip")]
    if not zips:
        print(f"  ! {ident}: no .zip in files list, skipping")
        return True, 0

    if dry_run:
        for z in zips:
            print(f"  [dry] would fetch {ident}/{z}")
        return True, 0

    dest_dir.mkdir(parents=True, exist_ok=True)
    write_metadata(rec, dest_dir)

    bytes_total = 0
    all_ok = True
    for z in zips:
        url = DOWNLOAD_BASE.format(
            ident=ident, name=urllib.parse.quote(z))
        target = dest_dir / z
        size_before = target.stat().st_size if target.exists() else 0
        expected = head_size(url)
        ok = download_with_retry(url, target,
                                 expected_size=expected,
                                 label=f"{ident}/{z}")
        if ok:
            size_after = target.stat().st_size
            bytes_total += max(0, size_after - size_before)
        else:
            all_ok = False
    return all_ok, bytes_total


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--dest", type=Path, default=DEST_GAMES_DEFAULT)
    ap.add_argument("--sdk-dest", type=Path, default=DEST_SDK_DEFAULT)
    ap.add_argument("--local-root", type=Path,
                    default=Path("/mnt/d/games/2dfm"),
                    help="curated on-disk tree to dedup against. "
                         "we skip any IA record whose ident / "
                         "exe_stem / kgt_filename matches a .kgt "
                         "stem already living here.")
    ap.add_argument("--include-sdk", action="store_true",
                    help="also download FM2K-engine SDK records (to "
                         "--sdk-dest). off by default.")
    ap.add_argument("--include-dupes", action="store_true",
                    help="don't skip records whose exe_stem matches "
                         "a game we already have locally.")
    ap.add_argument("--only", default="",
                    help="comma-separated ident substrings to "
                         "restrict the run to.")
    ap.add_argument("--dry-run", action="store_true",
                    help="report what would be downloaded; no fetch.")
    args = ap.parse_args()

    if not SCRAPE_PATH.exists():
        print(f"error: {SCRAPE_PATH} missing — run "
              f"tools/inspect_ia_zips.py first", file=sys.stderr)
        return 1

    scrape = json.loads(SCRAPE_PATH.read_text(encoding="utf-8"))
    only = [s.strip() for s in args.only.split(",") if s.strip()]
    games, sdks, stats = filter_records(
        scrape,
        include_sdk=args.include_sdk,
        include_dupes=args.include_dupes,
        only=only,
        local_root=args.local_root)

    print(f"queue: {len(games)} game record(s), "
          f"{len(sdks)} SDK record(s)")
    print(f"  skipped — unverified (no .kgt):  {stats['unverified']}")
    print(f"  skipped — local-duplicate:       {stats['dupe']}")
    print(f"  skipped — --only filter:         {stats['filtered_out']}")
    print()

    n_ok = n_fail = 0
    bytes_total = 0
    started = time.monotonic()

    print(f"=== games → {args.dest} ===")
    for rec in games:
        ok, b = process_record(rec, args.dest, dry_run=args.dry_run)
        bytes_total += b
        if ok:
            n_ok += 1
        else:
            n_fail += 1

    if args.include_sdk:
        print()
        print(f"=== SDK → {args.sdk_dest} ===")
        for rec in sdks:
            ok, b = process_record(rec, args.sdk_dest,
                                   dry_run=args.dry_run)
            bytes_total += b
            if ok:
                n_ok += 1
            else:
                n_fail += 1

    elapsed = time.monotonic() - started
    print()
    if args.dry_run:
        print(f"(dry run) would have processed "
              f"{len(games)} game record(s)"
              + (f" + {len(sdks)} SDK record(s)"
                 if args.include_sdk else ""))
    else:
        print(f"done in {elapsed:.0f}s — "
              f"ok: {n_ok}, failed: {n_fail}, "
              f"transferred: {fmt_size(bytes_total)}")
    return 0 if not n_fail else 2


if __name__ == "__main__":
    sys.exit(main())
