#!/usr/bin/env python3
"""Bulk-download banner images from URLs already in the registry.

Builds dist/banners/<game_id>.<ext> from any record whose
banner_url / thumb_url is an explicit http(s) URL — i.e. our own
hosted CDN once it exists. Idempotent: re-runs skip files that
already exist on disk; --refresh forces re-download.

This script does NOT scrape third-party sites. The only legitimate
upstream is our own CDN that the auto-capture pipeline writes to;
populate the registry with those URLs first
(see docs/dev/banner_pipeline.md), then run this to pull them
locally for stats/static/banners and dist/banners.

Run:    python3 tools/cache_banners.py
        python3 tools/cache_banners.py --refresh
        python3 tools/cache_banners.py --dry-run
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
REGISTRY_PATH = REPO_ROOT / "games" / "registry.json"
BANNERS_DIR = REPO_ROOT / "dist" / "banners"
# Mirror to stats/static/banners so `uvicorn app:app` serves them
# directly without a separate nginx mount during dev. Production
# rsyncs dist/banners/ → /srv/fm2k/stats/static/banners.
STATIC_BANNERS_DIR = REPO_ROOT / "stats" / "static" / "banners"

USER_AGENT = ("fm2k-rollback-launcher / banner-cache "
              "(https://github.com/Armonte/wanwan)")
SLEEP_BETWEEN = 0.3
MAX_BYTES = 8 * 1024 * 1024   # 8MB cap; real banners are well under

# Magic-byte → extension. Filters out non-image responses (HTML
# error pages, SVG, videos). We only support raster formats the
# stats site can render with <img>.
MAGIC = {
    b"\x89PNG\r\n\x1a\n":         "png",
    b"\xff\xd8\xff":              "jpg",
    b"GIF87a":                    "gif",
    b"GIF89a":                    "gif",
    b"RIFF":                      "webp",   # also requires "WEBP" at offset 8
}


def detect_ext(buf: bytes) -> str | None:
    for prefix, ext in MAGIC.items():
        if buf.startswith(prefix):
            if ext == "webp" and buf[8:12] != b"WEBP":
                continue
            return ext
    return None


def http_get_bytes(url: str) -> bytes | None:
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    try:
        with urllib.request.urlopen(req, timeout=20) as resp:
            buf = resp.read(MAX_BYTES + 1)
            if len(buf) > MAX_BYTES:
                print(f"  oversize ({len(buf)} bytes), skipping",
                      file=sys.stderr)
                return None
            return buf
    except Exception as e:
        print(f"  http_get_bytes failed: {url[:120]} -> {e}",
              file=sys.stderr)
        return None


def cached_image_for(game_id: str, *, refresh: bool = False) -> Path | None:
    for ext in ("png", "jpg", "gif", "webp"):
        p = BANNERS_DIR / f"{game_id}.{ext}"
        if p.exists() and p.stat().st_size > 0 and not refresh:
            return p
    return None


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--refresh", action="store_true",
                    help="re-download even when cached file exists")
    ap.add_argument("--dry-run", action="store_true",
                    help="report what would be downloaded; no writes")
    ap.add_argument("--limit", type=int, default=0,
                    help="stop after N downloads (debug)")
    args = ap.parse_args()

    if not REGISTRY_PATH.exists():
        print(f"error: {REGISTRY_PATH} not found "
              f"(run tools/build_registry.py first)", file=sys.stderr)
        return 1

    if not args.dry_run:
        BANNERS_DIR.mkdir(parents=True, exist_ok=True)
        STATIC_BANNERS_DIR.mkdir(parents=True, exist_ok=True)

    recs = json.loads(REGISTRY_PATH.read_text(encoding="utf-8"))
    todo: list[tuple[dict[str, Any], str]] = []
    for r in recs:
        gid = r.get("game_id") or ""
        if not gid:
            continue
        url = r.get("banner_url") or r.get("thumb_url")
        if url and url.startswith(("http://", "https://")):
            todo.append((r, url))

    print(f"candidates with external banner_url: {len(todo)}")
    if not todo:
        print("  (registry has no http(s):// banner_urls. Run the "
              "auto-capture pipeline to populate them — see "
              "docs/dev/banner_pipeline.md)")
        return 0

    fetched = 0
    skipped = 0
    failed = 0
    for r, url in todo:
        gid = r["game_id"]
        existing = cached_image_for(gid, refresh=args.refresh)
        if existing and not args.refresh:
            print(f"  skip (cached): {gid} -> "
                  f"{existing.relative_to(REPO_ROOT)}")
            rel = f"/static/banners/{existing.name}"
            r["banner_url"] = rel
            r["thumb_url"] = rel
            skipped += 1
            continue
        if args.limit and fetched >= args.limit:
            break
        print(f"fetch: {gid:30s} <- {url}")
        if args.dry_run:
            fetched += 1
            continue
        buf = http_get_bytes(url)
        time.sleep(SLEEP_BETWEEN)
        if not buf:
            failed += 1
            continue
        ext = detect_ext(buf)
        if not ext:
            print(f"  unrecognized image magic; skip")
            failed += 1
            continue
        out = BANNERS_DIR / f"{gid}.{ext}"
        out.write_bytes(buf)
        mirror = STATIC_BANNERS_DIR / f"{gid}.{ext}"
        mirror.write_bytes(buf)
        rel = f"/static/banners/{gid}.{ext}"
        r["banner_url"] = rel
        r["thumb_url"] = rel
        fetched += 1
        print(f"  ✓ {len(buf)} bytes → {out.relative_to(REPO_ROOT)} "
              f"({ext})")

    if not args.dry_run and (fetched or skipped):
        REGISTRY_PATH.write_text(
            json.dumps(recs, indent=2, ensure_ascii=False) + "\n",
            encoding="utf-8")

    print()
    print(f"fetched: {fetched}")
    print(f"skipped (cached): {skipped}")
    print(f"failed: {failed}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
