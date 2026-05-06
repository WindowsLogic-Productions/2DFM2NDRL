#!/usr/bin/env python3
"""Cache banner images locally from registry-supplied URLs.

DO NOT POINT THIS AT mizuumi.wiki. Mizuumi is operated by a friend of
the project lead; mass-downloading their image hosting would be both
rude and a betrayal of trust. The mizuumi support code below is left
as scaffolding only — it stays inert unless someone explicitly opts
in via --source mizuumi --i-have-permission and the canonical run
profile is "use whatever URLs the registry has, leave third-party
sources alone".

The intended source for v1 is our own auto-captured screenshots —
see docs/dev/banner_pipeline.md for the design that drives the game
through intro → title → CSS, captures clean PNGs from the hook's
own framebuffer, and uploads them to our CDN. Once that pipeline
runs, the registry's banner_url / thumb_url point at the CDN, and
this script is the build-time bulk-downloader that fills
dist/banners/ before the stats site / installer ships.

What this does:
  1. Read games/registry.json and pick entries that name a mizuumi
     `lead_image` filename (set by tools/scrape_mizuumi.py).
  2. For each, hit the MediaWiki Special:Filepath endpoint:
        https://mizuumi.wiki/wiki/Special:Filepath/<filename>
     This 302-redirects to the actual upload URL; urllib follows
     automatically and we get the bytes.
  3. Validate magic bytes (PNG / JPG / GIF / WebP only). Skip
     anything else — no SVG / video / weird formats.
  4. Write bytes verbatim to dist/banners/<game_id>.<ext>. We do NOT
     resize / re-encode here — Pillow would be a new dep and the
     stats site's CSS already handles scaling via `object-fit: cover`.
     Resize / WebP conversion is a separate optimization pass when
     someone wants the bandwidth win.
  5. Patch registry.json: set banner_url + thumb_url to
        /static/banners/<game_id>.<ext>
     so the stats site's templates pick them up automatically.

Run:    python3 tools/cache_banners.py
        python3 tools/cache_banners.py --refresh    # re-download
        python3 tools/cache_banners.py --dry-run    # preview only

Idempotent: re-runs skip files that already exist (size > 0). Pass
--refresh to force re-download (e.g. when mizuumi rotated the file).
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

API_URL = "https://mizuumi.wiki/api.php"
USER_AGENT = ("fm2k-rollback-launcher / banner-cache "
              "(https://github.com/Armonte/wanwan)")
SLEEP_BETWEEN = 0.5
MAX_BYTES = 8 * 1024 * 1024     # 8MB hard cap; real banners are <500KB

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
    """Magic-byte sniff. Returns lowercase 3-letter extension or None
    when the bytes don't look like a supported raster image."""
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


def http_get_json(url: str) -> dict[str, Any] | None:
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    try:
        with urllib.request.urlopen(req, timeout=15) as resp:
            return json.loads(resp.read().decode("utf-8", errors="replace"))
    except Exception as e:
        print(f"  http_get_json failed: {url[:120]} -> {e}",
              file=sys.stderr)
        return None


def resolve_image_url(filename: str) -> str | None:
    """Ask MediaWiki's imageinfo API where the bytes actually live.

    mizuumi.wiki sits behind Cloudflare; the convenience URL
    https://mizuumi.wiki/wiki/Special:Filepath/<file> trips a CF JS
    challenge for non-browser clients (HTTP 403 cf-mitigated:
    challenge). The api.php endpoint is whitelisted and responds
    fine, and the imageinfo[].url it returns points at the canonical
    /images/<a>/<ab>/<filename> path which CF serves directly. So:
    one API hop to discover the URL, then a plain GET on the URL.
    """
    params = {
        "action":  "query",
        "titles":  f"File:{filename}",
        "prop":    "imageinfo",
        "iiprop":  "url|size|mime",
        "format":  "json",
    }
    url = f"{API_URL}?{urllib.parse.urlencode(params)}"
    d = http_get_json(url)
    if not d:
        return None
    pages = (d.get("query") or {}).get("pages") or {}
    for _, page in pages.items():
        infos = page.get("imageinfo") or []
        if infos and infos[0].get("url"):
            return infos[0]["url"]
    return None


def cached_image_for(game_id: str, *, refresh: bool = False) -> Path | None:
    """Return existing cached image path for `game_id` if any (any
    supported extension), else None."""
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
    ap.add_argument("--source", default="self",
                    choices=("self", "mizuumi"),
                    help="which registry source to pull from. "
                         "Default 'self' = only registry entries with "
                         "an explicit banner_url already set (= our "
                         "own CDN). 'mizuumi' OPT-IN-ONLY; never run "
                         "in CI / batch.")
    ap.add_argument("--i-have-permission", action="store_true",
                    help="acknowledgement gate for --source mizuumi")
    args = ap.parse_args()

    if args.source == "mizuumi" and not args.i_have_permission:
        print("error: --source mizuumi requires --i-have-permission. "
              "Mizuumi.wiki is third-party; don't bulk-download "
              "their images. See docs/dev/banner_pipeline.md.",
              file=sys.stderr)
        return 2

    if not REGISTRY_PATH.exists():
        print(f"error: {REGISTRY_PATH} not found "
              f"(run tools/build_registry.py first)", file=sys.stderr)
        return 1

    if not args.dry_run:
        BANNERS_DIR.mkdir(parents=True, exist_ok=True)
        STATIC_BANNERS_DIR.mkdir(parents=True, exist_ok=True)

    recs = json.loads(REGISTRY_PATH.read_text(encoding="utf-8"))

    # Two work-list selectors. Default (self) is "registry entries
    # where banner_url is already set to our own CDN" — the pipeline
    # the auto-capture tool feeds. Mizuumi (--i-have-permission) was
    # the original prototype path; left in place but gated.
    todo: list[tuple[dict[str, Any], str, str]] = []  # (rec, source, lead)
    if args.source == "self":
        for r in recs:
            gid = r.get("game_id") or ""
            if not gid:
                continue
            url = r.get("banner_url") or r.get("thumb_url")
            # Only fetch external (http/https) URLs. Already-relative
            # /static/banners/... paths are local and don't need a
            # download.
            if url and url.startswith(("http://", "https://")):
                todo.append((r, "self", url))
    else:
        for r in recs:
            gid = r.get("game_id") or ""
            if not gid:
                continue
            miz = r.get("_raw", {}).get("mizuumi.wiki", {}) or {}
            lead = miz.get("lead_image")
            if lead:
                todo.append((r, "mizuumi", lead))

    print(f"candidates ({args.source}): {len(todo)}")
    if not todo:
        if args.source == "self":
            print("  (no external banner_url in registry yet — run the "
                  "auto-capture pipeline first, or hand-edit "
                  "registry_overrides.json with CDN URLs)")
        return 0

    fetched = 0
    skipped = 0
    failed = 0

    for r, source, lead in todo:
        gid = r["game_id"]
        existing = cached_image_for(gid, refresh=args.refresh)
        if existing and not args.refresh:
            print(f"  skip (cached): {gid} -> "
                  f"{existing.relative_to(REPO_ROOT)}")
            # Still patch the registry URL — it might have been
            # missing if this is a re-run after manual cache copy.
            url = f"/static/banners/{existing.name}"
            r["banner_url"] = url
            r["thumb_url"] = url
            skipped += 1
            continue

        if args.limit and fetched >= args.limit:
            break

        print(f"fetch: {gid:30s} <- {lead}")
        if args.dry_run:
            fetched += 1
            continue

        # `self` source = lead is already a full URL. `mizuumi` source
        # = lead is a wiki filename; resolve via the imageinfo API
        # (which 200s) before downloading from /images/<sharded>/...
        if source == "self":
            buf = http_get_bytes(lead)
        else:
            direct_url = resolve_image_url(lead)
            time.sleep(SLEEP_BETWEEN)
            if not direct_url:
                print(f"  no imageinfo for {lead!r}; skip")
                failed += 1
                continue
            buf = http_get_bytes(direct_url)
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
        # Mirror to stats/static so dev serves work without rsync.
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
    print(f"banners dir: {BANNERS_DIR.relative_to(REPO_ROOT)}")
    print(f"mirror dir:  {STATIC_BANNERS_DIR.relative_to(REPO_ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
